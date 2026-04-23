/* ============================================================
 * mm.h — Per-process address space (Sv32, S-mode kernel)
 * ============================================================ */
#ifndef MM_H
#define MM_H

#include <stdint.h>
#include <stddef.h>
#include "mmu.h"   /* PTE_* bits, PHYSMAP_BASE, g_kernel_root_pt */

/* ---- Sv32 constants (mirror of mmu.h where not already defined) ---- */
#define PTES_PER_TABLE  1024u
#define VPN1(va)        (((uint32_t)(va) >> 22) & 0x3FFu)
#define VPN0(va)        (((uint32_t)(va) >> 12) & 0x3FFu)

/* PTE helpers */
#define PTE_PPN_SHIFT   10
#define PA_TO_PTE(pa)   ((uint32_t)(((uint32_t)(pa) >> PAGE_SHIFT) << PTE_PPN_SHIFT))
#define PTE_TO_PA(pte)  ((uintptr_t)(((uint32_t)(pte) >> PTE_PPN_SHIFT) << PAGE_SHIFT))

typedef uint32_t pte_t;

/* ---- VM flags (projected onto PTE bits in mm_map_pages) ---- */
#define VM_R     0x01u
#define VM_W     0x02u
#define VM_X     0x04u
#define VM_U     0x08u
#define VM_ANON  0x10u   /* demand-paged; fill on first fault */

/* ---- VMA: one contiguous virtual memory area ---- */
struct vma {
    uintptr_t    start;   /* inclusive, page-aligned */
    uintptr_t    end;     /* exclusive, page-aligned */
    uint32_t     flags;   /* VM_R | VM_W | VM_X | VM_U */
    struct vma  *next;
};

/* ---- Per-process address space descriptor ---- */
struct mm {
    uintptr_t    root_pt_pa;  /* physical address of the L1 root page table */
    struct vma  *vmas;        /* singly-linked list for accounting */
    uintptr_t    brk;         /* user heap top (unused in step 2, reserved) */
    int          refcount;
    uint32_t     pages_used;  /* PMM pages charged to this mm (root PT + L2 + anon) */
    uint32_t     pt_pages;    /* root PT + L2 tables */
    uint32_t     text_pages;  /* eager kernel-image-backed mappings (text/rodata) */
    uint32_t     anon_pages;  /* demand-paged anon pages (stack, bss, heap) */
    char         dbg_name[16];/* label printed by mm debug counter */
};

/* Lifecycle */
struct mm *mm_create(void);
void       mm_destroy(struct mm *mm);
void       mm_set_dbg_name(struct mm *mm, const char *name);

/* One-shot footprint print: total pages by category (PT, text, anon). */
void       mm_dbg_summary(struct mm *mm);

/* Page-table manipulation */
int    mm_map_pages(struct mm *mm, uintptr_t va, uintptr_t pa,
                    size_t npages, uint32_t vm_flags);
pte_t *mm_walk(struct mm *mm, uintptr_t va, int alloc_l2);

/* Context switch: write satp + sfence.vma for this mm (NULL = kernel PT). */
void mm_activate(struct mm *mm);

/* VMA bookkeeping (step 3: user_fault). */
struct vma *vma_find(struct mm *mm, uintptr_t va);
int         mm_reserve_anon(struct mm *mm, uintptr_t start, uintptr_t end,
                            uint32_t vm_flags);
int         mm_fill_anon_page(struct mm *mm, uintptr_t va, uint32_t vm_flags);

/* FreeRTOS TLS helpers — called from vApplicationTaskSwitchedIn. */
struct mm *mm_current(void);
void       mm_task_set(void *task_handle, struct mm *mm);

/* Kernel root PT physical address (set once at boot by main). */
extern uintptr_t g_kernel_root_pt_pa;

#endif /* MM_H */
