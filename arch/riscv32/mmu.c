/* ============================================================
 * mmu.c — Build the initial Sv32 page table.
 *
 * Strategy:
 *   - One L1 root table, 1024 entries, 4 MiB per entry.
 *   - Identity-map the entire low 2 GiB as kernel RWX megapages
 *     (cheap; covers MMIO, RAM, and leaves the low-half alias used
 *     only during the brief moment between `csrw satp` and the
 *     jump into the high half).
 *   - Map one 4 MiB megapage at VA 0x80000000 → PA 0x00000000 so
 *     the kernel's high-half symbols are reachable and so the
 *     kernel text/data physically placed at 0x00100000 lies
 *     inside that 4 MiB window (since 0x0 + 4 MiB = 0x00400000 >
 *     0x00200000, our entire RAM is covered).
 * ============================================================ */

#include "mmu.h"
#include "pmm.h"
#include "FreeRTOS.h"
#include <string.h>

/* Definition of the single root page table.
 * Placed in .bss.pagetable so the linker can align it to 4 KiB.         */
uint32_t g_kernel_root_pt[ROOT_PT_ENTRIES]
    __attribute__((aligned(4096), section(".bss.pagetable")));

/* Build one megapage PTE:
 *   Leaf, covers 4 MiB of PA starting at pa_base (must be 4 MiB aligned).
 *   PPN[1] carries bits [33:22] of PA (here just PA>>22), PPN[0]=0.      */
static inline uint32_t make_megapage_pte(uint32_t pa_base, uint32_t perms)
{
    /* For a megapage leaf, PPN[0] MUST be zero. The standard encoding
     * still uses bits 31:10 for the full PPN, but the hardware ignores
     * bits 19:10 when it walks to a megapage leaf. We set them to 0
     * to keep the PTE unambiguous.                                       */
    return ((pa_base >> 22) << 20) | perms;
}

void mmu_build_identity_and_highhalf(void)
{
    /* 1. Identity map the whole low 2 GiB as kernel RWX megapages.
     *    1024/2 = 512 entries covering 0..0x80000000.
     *    This is lazy but free — the TLB only populates entries we use. */
    for (unsigned i = 0; i < 512; ++i) {
        uint32_t pa = (uint32_t)i << 22;
        g_kernel_root_pt[i] = make_megapage_pte(pa, PTE_KERN_RWX);
    }

    /* 2. High-half window. We want VA 0x80000000..0x803FFFFF (one 4 MiB
     *    megapage) to resolve to PA 0x00000000..0x003FFFFF, so that the
     *    kernel image (loaded at PA 0x00100000) is reachable via
     *    VA 0x80100000.                                                  */
    unsigned highhalf_idx = KERNEL_VA_BASE >> 22;              /* = 512 */
    g_kernel_root_pt[highhalf_idx] = make_megapage_pte(0x00000000u,
                                                       PTE_KERN_RWX);

    /* 3. (Optional) Leave the remaining high half unmapped. Anything
     *    not V=1 will trap as a page fault, which is what we want.       */
}

/* ============================================================
 * Refine kernel megapage into per-section 4 KiB PTEs.
 * ============================================================ */

/* Linker-exported section boundaries (virtual addresses). */
extern char _stext[], _etext[], _srodata[], _erodata[], _sdata[], _ebss[];

/* The megapage at L1[512] maps VA 0x80000000 → PA 0x00000000.
 * KVA_TO_PA strips the high bit to recover the physical address. */
#define KVA_TO_PA(v)  ((uint32_t)(v) - KERNEL_VA_BASE)

typedef uint32_t pte_t;

/* L2 table for the kernel image slot — lives in .bss, inside the megapage
 * it replaces. 4 KiB aligned so it can be installed as a page-table page. */
static pte_t kernel_l2[1024] __attribute__((aligned(4096)));

static inline void sfence_vma_all(void) {
    __asm__ volatile ("sfence.vma zero, zero" ::: "memory");
}
static inline void mem_fence(void) {
    __asm__ volatile ("fence rw,rw" ::: "memory");
}

static inline pte_t make_leaf_pte(uint32_t pa, uint32_t flags) {
    return ((pa >> PAGE_SHIFT) << 10) | flags | PTE_V;
}
static inline pte_t make_nonleaf_pte(uint32_t pt_pa) {
    return ((pt_pa >> PAGE_SHIFT) << 10) | PTE_V;
}

#define PAGE_SHIFT  12
#define VPN1(va)    (((va) >> 22) & 0x3FF)
#define VPN0(va)    (((va) >> 12) & 0x3FF)

static uint32_t kernel_page_flags(uint32_t va)
{
    uint32_t base = PTE_G | PTE_A | PTE_D;
    if (va >= (uint32_t)_stext   && va < (uint32_t)_etext)   return base | PTE_R | PTE_X;
    if (va >= (uint32_t)_srodata && va < (uint32_t)_erodata) return base | PTE_R;
    if (va >= (uint32_t)_sdata   && va < (uint32_t)_ebss)    return base | PTE_R | PTE_W;
    /* Boot code, page tables, ISR stack, heap — all RW. */
    return base | PTE_R | PTE_W;
}

void mmu_refine_kernel_megapage(void)
{
    /* 1. Build 1024 leaf PTEs covering VA 0x80000000..0x803FFFFF. */
    for (uint32_t i = 0; i < 1024; ++i) {
        uint32_t va = KERNEL_VA_BASE + i * (1u << PAGE_SHIFT);
        uint32_t pa = KVA_TO_PA(va);
        ((volatile pte_t *)kernel_l2)[i] = make_leaf_pte(pa, kernel_page_flags(va));
    }

    /* 2. Ensure all L2 stores are visible before the L1 flip. */
    mem_fence();

    /* 3. Replace the L1 leaf megapage with a pointer to the new L2 table. */
    uint32_t l2_pa = KVA_TO_PA((uint32_t)(uintptr_t)kernel_l2);
    ((volatile pte_t *)g_kernel_root_pt)[VPN1(KERNEL_VA_BASE)] =
        make_nonleaf_pte(l2_pa);

    /* 4. Flush TLB — non-leaf PTE change requires sfence.vma with rs1=x0.
     *    Also reload satp to force a total TLB flush (covers global PTEs
     *    on implementations where sfence.vma may retain them).            */
    sfence_vma_all();
    { uint32_t _s; __asm__ volatile("csrr %0,satp":"=r"(_s));
      __asm__ volatile("csrw satp,%0" :: "r"(_s) : "memory");
      __asm__ volatile("sfence.vma zero,zero":::"memory"); }

}

/* ============================================================
 * Install direct physmap at VA 0xC0000000.
 * After this call: any PA can be accessed at VA = PA + 0xC0000000.
 * ============================================================ */
void mmu_install_physmap(uintptr_t ram_start, uintptr_t ram_end)
{
    void *pt_pa = pmm_alloc_page();
    configASSERT(pt_pa != NULL);
    pte_t *pt_kva = (pte_t *)((uintptr_t)pt_pa + KERNEL_VA_BASE);
    memset(pt_kva, 0, 1u << PAGE_SHIFT);

    for (uintptr_t pa = ram_start; pa < ram_end; pa += (1u << PAGE_SHIFT)) {
        uintptr_t va = pa + PHYSMAP_BASE;
        uint32_t flags = PTE_G | PTE_A | PTE_D | PTE_R | PTE_W;
        pt_kva[VPN0(va)] = make_leaf_pte((uint32_t)pa, flags);
    }

    mem_fence();
    ((volatile pte_t *)g_kernel_root_pt)[VPN1(PHYSMAP_BASE)] =
        make_nonleaf_pte((uint32_t)pt_pa);
    sfence_vma_all();
}