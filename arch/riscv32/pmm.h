/* super_freertos/arch/riscv32/pmm.h */
#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>

#define PAGE_SHIFT      12
#define PAGE_SIZE       (1u << PAGE_SHIFT)
#define PAGE_MASK       (PAGE_SIZE - 1u)

#define PMM_MAX_ORDER   6
#define PMM_NR_ORDERS   (PMM_MAX_ORDER + 1)
#define PMM_MAX_PAGES   256u

struct pmm_reserved {
    uintptr_t start;    /* physical */
    uintptr_t end;      /* physical, exclusive */
};

/* Initialise over the physical window [ram_start, ram_end). Any sub-ranges
   inside that window that must be protected are listed in 'resv'. */
void  pmm_init(uintptr_t ram_start, uintptr_t ram_end,
               const struct pmm_reserved *resv, size_t nresv);

/* Returns the PHYSICAL address of a 2^order contiguous page run. */
void *pmm_alloc_pages(int order);

/* Free a previously allocated run. Caller must supply the same order. */
void  pmm_free_pages(void *pa, int order);

/* Convenience: one page. */
static inline void *pmm_alloc_page(void) { return pmm_alloc_pages(0); }
static inline void  pmm_free_page(void *p) { pmm_free_pages(p, 0); }

/* Stats */
uint32_t pmm_free_pages_count(void);

#endif
