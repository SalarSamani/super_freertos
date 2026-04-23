/* app/spawn.c — create U-mode tasks from a user_prog_desc. */
#include "FreeRTOS.h"
#include "task.h"
#include "mm.h"
#include "pmm.h"
#include "proc.h"
#include "spawn.h"
#include <string.h>
#include <stdint.h>

/* Per-program linker symbols.  Each program's VMA is the nominal user
 * entry (0x00400000, shared); the LMA is the physical address inside
 * the kernel image where the blob was loaded; size is the rounded-up
 * page extent. */
extern char _user_init_vma[],  _user_init_lma[],  _user_init_size[];
extern char _user_hello_vma[], _user_hello_lma[], _user_hello_size[];
extern char _user_bad_vma[],   _user_bad_lma[],   _user_bad_size[];

static const struct user_prog_desc g_progs[] = {
    { "init",  (uintptr_t)_user_init_vma,  (uintptr_t)_user_init_lma,
      (uint64_t)(uintptr_t)_user_init_size,  (uintptr_t)_user_init_vma },
    { "hello", (uintptr_t)_user_hello_vma, (uintptr_t)_user_hello_lma,
      (uint64_t)(uintptr_t)_user_hello_size, (uintptr_t)_user_hello_vma },
    { "bad",   (uintptr_t)_user_bad_vma,   (uintptr_t)_user_bad_lma,
      (uint64_t)(uintptr_t)_user_bad_size,   (uintptr_t)_user_bad_vma },
};

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

const struct user_prog_desc *find_user_prog(const char *name)
{
    for (unsigned i = 0; i < sizeof(g_progs)/sizeof(g_progs[0]); i++)
        if (streq(g_progs[i].name, name)) return &g_progs[i];
    return NULL;
}

/* User stack: 4 pages (16 KiB) below 0x00800000.  Must live in
 * L1[1+]; L1[0] is reserved for the kernel MMIO identity map and
 * is unreachable from U-mode (PTE_U=0).  User text is at VA
 * 0x00400000 (L1[1]); the stack sits at the top of the same L1
 * slot so both share one L2 table.  Stack pages are demand-paged. */
#define USER_STACK_TOP    0x00800000u
#define USER_STACK_PAGES  1
#define USER_STACK_BASE   (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)

/* Kernel stack for each U-mode task: 2 KiB.  One static arena per
 * proc slot — NPROC is small (8) and we never grow. */
#define KSTACK_WORDS      512

/* Declared in port.c */
StackType_t *prvInitialiseUserStack(StackType_t *pxTopOfStack,
                                    uintptr_t user_entry_va,
                                    uintptr_t user_sp_va);
void port_set_task_top_of_stack(TaskHandle_t h, StackType_t *sp);
void port_set_task_kstack_top(void *task_handle, uintptr_t kstack_top);

/* Declared in mm.c */
void mm_task_set(void *task_handle, struct mm *mm);

/* Per-slot static storage.  proc->pid is assigned from a slot index,
 * but allocation order isn't deterministic — walk in PID order by
 * using the slot implied by proc->pid (pid 1 -> slot 0, others ->
 * slot pid-1).  Keeps lifetimes tied to the proc slot without any
 * dynamic allocation. */
#define PID_TO_SLOT(pid)  ((pid) == 1 ? 0 : (pid) - 1)

static StaticTask_t s_tcb_pool[NPROC];
static StackType_t  s_kstack_pool[NPROC][KSTACK_WORDS];

int spawn_user_prog(const struct user_prog_desc *desc, pid_t parent_pid)
{
    if (!desc || desc->size == 0) return -1;

    /* 1. Allocate a proc slot. */
    struct proc *proc = proc_alloc(desc->name, parent_pid);
    if (!proc) return -1;

    /* 2. Address space: text mapped eagerly (RX+U), stack reserved anon. */
    struct mm *mm = mm_create();
    if (!mm) { proc_free_slot(proc); return -1; }
    mm_set_dbg_name(mm, desc->name);

    size_t text_pgs = ((size_t)desc->size + PAGE_MASK) >> PAGE_SHIFT;
    if (mm_map_pages(mm, desc->vma, desc->lma, text_pgs,
                     VM_R | VM_X | VM_U) < 0) goto fail;

    if (mm_reserve_anon(mm, USER_STACK_BASE, USER_STACK_TOP,
                        VM_R | VM_W | VM_U) < 0) goto fail;

    /* 3. Kernel stack slab + static TCB for this proc slot. */
    int slot = PID_TO_SLOT(proc->pid);
    StackType_t *kstack     = s_kstack_pool[slot];
    StackType_t *kstack_top = kstack + KSTACK_WORDS;

    TaskHandle_t h = xTaskCreateStatic(
        (TaskFunction_t)(uintptr_t)desc->entry,  /* ignored — frame drives PC */
        desc->name,
        KSTACK_WORDS,
        NULL,
        tskIDLE_PRIORITY + 1,
        kstack,
        &s_tcb_pool[slot]);
    if (!h) goto fail;

    /* 4. Overwrite FreeRTOS's S-mode frame with a U-mode trap frame. */
    StackType_t *sp_after = prvInitialiseUserStack(
        kstack_top,
        desc->entry,                 /* sepc: user entry VA */
        (uintptr_t)USER_STACK_TOP);  /* x2:  user SP (grows down) */

    port_set_task_top_of_stack(h, sp_after);

    /* 5. Hook mm, kstack_top, and proc back-pointer into TLS. */
    mm_task_set(h, mm);
    port_set_task_kstack_top(h, (uintptr_t)kstack_top);
    proc->mm   = mm;
    proc->task = h;
    vTaskSetThreadLocalStoragePointer(h, PROC_TLS_SLOT, (void *)proc);

    /* One-shot footprint print: total pages used by this process
     * (page tables + text + stack/anon) before it starts running. */
    mm_dbg_summary(mm);

    return (int)proc->pid;

fail:
    mm_destroy(mm);
    proc_free_slot(proc);
    return -1;
}
