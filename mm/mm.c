/* ============================================================
 * mm.c — Per-process address space for Sv32 / super_ibex
 *
 * Implements:
 *   §1  Per-task address spaces  (mm_create/destroy, mm_walk,
 *                                  mm_map_pages, mm_activate)
 *   §2  Hanging an mm off FreeRTOS TCBs via TLS slot 0
 *       (mm_current, mm_task_set, vApplicationTaskSwitchedIn)
 * ============================================================ */

#include "mm.h"
#include "../arch/riscv32/pmm.h"
#include "../arch/riscv32/slab.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* ============================================================
 * §0  Debug: per-mm physical page accounting
 *
 * Bumped on every PMM page charged to an mm (root PT, L2 tables,
 * anon user pages) and printed each time so the human can watch
 * a process's footprint grow and shrink.  Output goes straight to
 * the simple_system UART MMIO; this file has no printf.
 * ============================================================ */
#define MM_DBG_UART  ((volatile uint32_t *)0x00020000)

static void mm_dbg_putc(char c) { *MM_DBG_UART = (uint8_t)c; }
static void mm_dbg_puts(const char *s) { while (*s) mm_dbg_putc(*s++); }
static void mm_dbg_putu(uint32_t v)
{
    char buf[11];
    int  i = 0;
    if (v == 0) { mm_dbg_putc('0'); return; }
    while (v) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i--) mm_dbg_putc(buf[i]);
}

/* Categories: PT = root PT + L2 tables, TEXT = eager kernel-image-backed
 * pages (mm_map_pages: code/rodata), ANON = demand-paged (stack/bss/heap). */
enum { MM_CAT_PT = 0, MM_CAT_TEXT, MM_CAT_ANON };

static void mm_charge(struct mm *mm, int cat, const char *what)
{
    (void)what;
    mm->pages_used++;
    if      (cat == MM_CAT_PT)   mm->pt_pages++;
    else if (cat == MM_CAT_TEXT) mm->text_pages++;
    else                         mm->anon_pages++;
}

static void mm_uncharge(struct mm *mm, int cat, const char *what)
{
    (void)what;
    if (mm->pages_used) mm->pages_used--;
    if      (cat == MM_CAT_PT   && mm->pt_pages)   mm->pt_pages--;
    else if (cat == MM_CAT_TEXT && mm->text_pages) mm->text_pages--;
    else if (cat == MM_CAT_ANON && mm->anon_pages) mm->anon_pages--;
}

void mm_dbg_summary(struct mm *mm)
{
    if (!mm) return;

    /* Sum reserved anon VMA pages (stack/bss/heap) — these are demand-paged,
     * so mm->anon_pages is still 0 at spawn time.  Report the reservation
     * so the per-process footprint reflects the whole address space. */
    uint32_t anon_reserved = 0;
    for (struct vma *v = mm->vmas; v; v = v->next)
        if (v->flags & VM_ANON)
            anon_reserved += (uint32_t)((v->end - v->start) >> PAGE_SHIFT);

    uint32_t total_pages = mm->pt_pages + mm->text_pages + anon_reserved;
    uint32_t total_kb    = total_pages * (PAGE_SIZE / 1024u);

    mm_dbg_puts("[mm:");
    mm_dbg_puts(mm->dbg_name[0] ? mm->dbg_name : "?");
    mm_dbg_puts("] footprint: total=");
    mm_dbg_putu(total_pages);
    mm_dbg_puts(" pages (pt=");
    mm_dbg_putu(mm->pt_pages);
    mm_dbg_puts(" text=");
    mm_dbg_putu(mm->text_pages);
    mm_dbg_puts(" stack=");
    mm_dbg_putu(anon_reserved);
    mm_dbg_puts(") = ");
    mm_dbg_putu(total_kb);
    mm_dbg_puts(" KB\n");
}

void mm_set_dbg_name(struct mm *mm, const char *name)
{
    if (!mm || !name) return;
    size_t i = 0;
    while (i + 1 < sizeof(mm->dbg_name) && name[i]) {
        mm->dbg_name[i] = name[i];
        i++;
    }
    mm->dbg_name[i] = '\0';
}

/* ============================================================
 * §1.1  Physical address ↔ kernel virtual address
 *
 * After mmu_install_physmap(), every PA is reachable at
 * VA = PA + PHYSMAP_BASE.  We touch page tables exclusively
 * through this window; no recursive mapping needed.
 * ============================================================ */
static inline pte_t *pa_to_kva_pt(uintptr_t pa)
{
    return (pte_t *)(pa + PHYSMAP_BASE);
}

/* ============================================================
 * §1.2  VM flags → PTE attribute bits
 * ============================================================ */
static uint32_t vm_flags_to_pte(uint32_t vm)
{
    /* Pin A and D bits to avoid hardware PTE-update faults on cores
     * that don't implement hardware A/D management (Ibex does not). */
    uint32_t p = PTE_V | PTE_A | PTE_D;
    if (vm & VM_R) p |= PTE_R;
    if (vm & VM_W) p |= PTE_W;
    if (vm & VM_X) p |= PTE_X;
    if (vm & VM_U) p |= PTE_U;
    return p;
}

/* ============================================================
 * §1.3  mm_create — allocate a root PT and copy the kernel half
 *
 * Cost: one PMM page (root PT) + one memcpy of 512 words.
 * The upper L1 entries (indices 512..1023, VA 0x80000000+) are
 * copied from g_kernel_root_pt — they point to the *same* kernel
 * L2 tables, so the kernel is directly reachable from any process
 * page table.  Kernel leaf PTEs have PTE_U=0, so U-mode cannot
 * follow them.
 * ============================================================ */
uintptr_t g_kernel_root_pt_pa;   /* set by main() after boot */

struct mm *mm_create(void)
{
    struct mm *mm = (struct mm *)kmalloc(sizeof(*mm));
    if (!mm) return NULL;

    uintptr_t pa = (uintptr_t)pmm_alloc_page();
    if (!pa) { kfree(mm); return NULL; }

    pte_t *root = pa_to_kva_pt(pa);
    memset(root, 0, PAGE_SIZE);

    /* Copy L1[0] (identity-mapped MMIO + kernel low alias) and
     * L1[512..1023] (kernel high half + physmap).  Ibex's TLB does
     * NOT honor PTE_G, so sfence.vma flushes globals too — the
     * kernel must find its own mappings in every process's root PT.
     * L1[0] is a leaf megapage with PTE_U=0, so U-mode cannot reach
     * it.  User code must live at VA >= 0x00400000 (L1[1+]). */
    pte_t *kroot = pa_to_kva_pt(g_kernel_root_pt_pa);
    root[0] = kroot[0];
    for (int i = 512; i < 1024; i++)
        root[i] = kroot[i];

    mm->root_pt_pa = pa;
    mm->vmas       = NULL;
    mm->brk        = 0x00400000u;   /* placeholder for step 3 heap */
    mm->refcount   = 1;
    mm->pages_used = 0;
    mm->pt_pages   = 0;
    mm->text_pages = 0;
    mm->anon_pages = 0;
    mm->dbg_name[0] = '\0';
    mm_charge(mm, MM_CAT_PT, "root_pt");
    return mm;
}

/* ============================================================
 * §1.4  mm_walk — return a pointer to the L2 leaf PTE slot for va
 *
 * alloc_l2=1: allocate the L2 table if it does not exist yet.
 * alloc_l2=0: return NULL if the L2 table is missing.
 *
 * We never create L1 megapage leaves in user space.
 * ============================================================ */
pte_t *mm_walk(struct mm *mm, uintptr_t va, int alloc_l2)
{
    pte_t *l1 = pa_to_kva_pt(mm->root_pt_pa);
    pte_t  e1 = l1[VPN1(va)];

    uintptr_t l2_pa;
    if (e1 & PTE_V) {
        /* Must be a non-leaf pointer (R=W=X=0). */
        if (e1 & (PTE_R | PTE_W | PTE_X)) return NULL;
        l2_pa = PTE_TO_PA(e1);
    } else {
        if (!alloc_l2) return NULL;
        l2_pa = (uintptr_t)pmm_alloc_page();
        if (!l2_pa) return NULL;
        memset(pa_to_kva_pt(l2_pa), 0, PAGE_SIZE);
        /* Install non-leaf L1 entry: V=1, R=W=X=0, no U, no G. */
        l1[VPN1(va)] = PA_TO_PTE(l2_pa) | PTE_V;
        mm_charge(mm, MM_CAT_PT, "L2_table");
    }

    pte_t *l2 = pa_to_kva_pt(l2_pa);
    return &l2[VPN0(va)];
}

/* ============================================================
 * §1.5  mm_map_pages — install npages consecutive 4 KiB mappings
 * ============================================================ */
int mm_map_pages(struct mm *mm, uintptr_t va, uintptr_t pa,
                 size_t npages, uint32_t vm_flags)
{
    if ((va | pa) & PAGE_MASK) return -1;
    uint32_t attr = vm_flags_to_pte(vm_flags);

    for (size_t i = 0; i < npages; i++) {
        pte_t *slot = mm_walk(mm, va + i * PAGE_SIZE, /*alloc=*/1);
        if (!slot) return -1;
        if (*slot & PTE_V) return -1;   /* already mapped */
        *slot = PA_TO_PTE(pa + i * PAGE_SIZE) | attr;
    }
    /* Account for text/rodata pages in the footprint summary even though
     * they're kernel-image-backed (no PMM page consumed). */
    if (vm_flags & VM_U) mm->text_pages += npages;
    /* No sfence needed: the target PT is not active in any hart's satp yet.
     * The flush happens in mm_activate when the task first runs. */
    return 0;
}

/* ============================================================
 * §1.5a  VMA helpers — sorted, non-overlapping list
 *
 * The VMA list is the *contract* kept after PTEs may have been torn
 * down or not-yet-installed (demand paging).  On a page fault we
 * consult this list to decide whether the access is legal.
 * ============================================================ */
struct vma *vma_find(struct mm *mm, uintptr_t va)
{
    for (struct vma *v = mm->vmas; v; v = v->next)
        if (va >= v->start && va < v->end) return v;
    return NULL;
}

static int vma_insert(struct mm *mm, uintptr_t start, uintptr_t end,
                      uint32_t flags)
{
    if (start >= end || (start | end) & PAGE_MASK) return -1;

    /* Find sorted insertion point and check overlap. */
    struct vma **pp = &mm->vmas;
    while (*pp && (*pp)->start < start) {
        if ((*pp)->end > start) return -1;   /* overlap with earlier VMA */
        pp = &(*pp)->next;
    }
    if (*pp && (*pp)->start < end) return -1;   /* overlap with next VMA */

    struct vma *v = (struct vma *)kmalloc(sizeof(*v));
    if (!v) return -1;
    v->start = start;
    v->end   = end;
    v->flags = flags;
    v->next  = *pp;
    *pp      = v;
    return 0;
}

/* Reserve an anonymous, demand-paged range.  No PTEs are installed;
 * pages are allocated lazily in mm_fill_anon_page on the first fault. */
int mm_reserve_anon(struct mm *mm, uintptr_t start, uintptr_t end,
                    uint32_t vm_flags)
{
    return vma_insert(mm, start, end, vm_flags | VM_ANON);
}

/* Allocate+zero one page and install its leaf PTE.  Called from the
 * page fault handler.  Ibex has no hardware A/D, so we set PTE_A here
 * (and PTE_D if writable) via vm_flags_to_pte above.  A single-VA
 * sfence.vma is enough because the faulting hart's TLB held the old
 * invalid translation for *this* page only. */
int mm_fill_anon_page(struct mm *mm, uintptr_t va, uint32_t vm_flags)
{
    va &= ~(uintptr_t)PAGE_MASK;

    pte_t *slot = mm_walk(mm, va, /*alloc=*/1);
    if (!slot) return -1;
    if (*slot & PTE_V) return 0;   /* already filled (race or reentry) */

    uintptr_t pa = (uintptr_t)pmm_alloc_page();
    if (!pa) return -1;
    memset((void *)(pa + PHYSMAP_BASE), 0, PAGE_SIZE);

    *slot = PA_TO_PTE(pa) | vm_flags_to_pte(vm_flags);
    mm_charge(mm, MM_CAT_ANON, "anon_page");

    /* Flush just this VA. rs1 holds the VA; rs2=x0 means all ASIDs. */
    __asm__ volatile("sfence.vma %0, zero" :: "r"(va) : "memory");
    return 0;
}

/* ============================================================
 * §1.6  mm_destroy — free all user-owned pages and the root PT
 *
 * Only frees user-half L2 tables (L1 indices 0..511) and leaves
 * marked PTE_U.  Kernel-half L1 entries (512..1023) are shared
 * pointers — freeing their L2 tables would unmap the kernel from
 * every other process.
 * ============================================================ */
void mm_destroy(struct mm *mm)
{
    if (!mm) return;
    if (--mm->refcount > 0) return;

    /* Free physical pages backing anonymous VMAs only.  Text/data VMAs
     * map pages from the kernel ELF image (e.g. _user_hello_lma); those
     * pages were never PMM-allocated and must not be pushed onto the
     * PMM freelist. */
    for (struct vma *v = mm->vmas; v; v = v->next) {
        if (!(v->flags & VM_ANON)) continue;
        for (uintptr_t va = v->start; va < v->end; va += PAGE_SIZE) {
            pte_t *slot = mm_walk(mm, va, /*alloc=*/0);
            if (!slot) continue;
            pte_t pte = *slot;
            if ((pte & PTE_V) && (pte & PTE_U)) {
                pmm_free_page((void *)PTE_TO_PA(pte));
                mm_uncharge(mm, MM_CAT_ANON, "anon_page");
            }
        }
    }

    /* Free L2 page tables for the user half, then the root PT. */
    pte_t *l1 = pa_to_kva_pt(mm->root_pt_pa);
    for (int i = 0; i < 512; i++) {
        pte_t e = l1[i];
        if (!(e & PTE_V)) continue;
        if (e & (PTE_R | PTE_W | PTE_X)) continue;   /* no megaleaves in user */
        pmm_free_page((void *)PTE_TO_PA(e));
        mm_uncharge(mm, MM_CAT_PT, "L2_table");
    }
    pmm_free_page((void *)mm->root_pt_pa);
    mm_uncharge(mm, MM_CAT_PT, "root_pt");

    struct vma *v = mm->vmas;
    while (v) { struct vma *n = v->next; kfree(v); v = n; }
    kfree(mm);
}

/* ============================================================
 * §1.7  mm_activate — write satp and flush user TLB entries
 *
 * sfence.vma x0, t0 (where t0 == 0) flushes ASID-0 entries but
 * preserves PTE_G-tagged kernel entries — unlike sfence.vma x0, x0
 * which flushes everything including globals.
 * See Priv §12.1.2.1: use rs2 = non-x0 register holding value 0.
 * ============================================================ */

#define SATP_MODE_SV32    (1u << 31)
#define SATP_ASID_SHIFT   22

static inline uint32_t make_satp(uintptr_t root_pa, uint32_t asid)
{
    return SATP_MODE_SV32
         | ((asid & 0x1FFu) << SATP_ASID_SHIFT)
         | ((uint32_t)(root_pa >> PAGE_SHIFT) & 0x003FFFFFu);
}

void mm_activate(struct mm *mm)
{
    uintptr_t pa = mm ? mm->root_pt_pa : g_kernel_root_pt_pa;
    uint32_t  s  = make_satp(pa, /*asid=*/0);
    __asm__ volatile("csrw satp, %0" :: "r"(s) : "memory");
    /* Flush user entries; keep global (kernel) entries intact. */
    __asm__ volatile("li t0, 0; sfence.vma x0, t0" ::: "t0", "memory");
}

/* ============================================================
 * §2  Hanging an mm off every FreeRTOS task (TLS slot 0)
 *
 * Kernel-only tasks leave TLS[0] as NULL; mm_activate(NULL)
 * falls back to the global kernel root PT.
 * ============================================================ */

#define TLS_MM_SLOT  0

struct mm *mm_current(void)
{
    return (struct mm *)pvTaskGetThreadLocalStoragePointer(NULL, TLS_MM_SLOT);
}

void mm_task_set(void *task_handle, struct mm *mm)
{
    vTaskSetThreadLocalStoragePointer(
        (TaskHandle_t)task_handle, TLS_MM_SLOT, (void *)mm);
}

/* ============================================================
 * §2.1  Context-switch hook
 *
 * traceTASK_SWITCHED_IN() in FreeRTOSConfig.h expands to this
 * function.  It fires after pxCurrentTCB has been updated to the
 * incoming task, with interrupts disabled — exactly the right
 * moment to swap satp.
 *
 * sscratch is updated here too (port_set_kstack_top) so that the
 * first U-mode trap from the incoming task lands on its kernel
 * stack.  That helper lives in port.c and is declared extern below.
 * ============================================================ */
void vApplicationTaskSwitchedIn(void)
{
    /* sscratch is owned by restore_context (portASM.S) — it sets it from
     * the outgoing frame's SPP.  Touching sscratch here is incorrect for
     * a task that is currently in S-mode (mid-syscall): it would break
     * the S-mode invariant (sscratch == 0) and cause the next trap entry
     * to misclassify as U-mode, landing on kstack_top and clobbering the
     * outer syscall's frame. */
    mm_activate(mm_current());
}
