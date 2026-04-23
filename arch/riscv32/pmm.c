/* super_freertos/arch/riscv32/pmm.c */
#include "pmm.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#define ALIGN_UP(v,a)   (((v) + (a) - 1u) & ~((a) - 1u))
#define ALIGN_DOWN(v,a) ((v) & ~((a) - 1u))

struct free_node { struct free_node *next; };

struct order_info {
    struct free_node *freelist;
    uint32_t          nr_blocks;
    uint8_t          *alloc_map;
};

static struct order_info bd[PMM_NR_ORDERS];
static uintptr_t  pmm_base;           /* PA of managed window */
static uint32_t   pmm_window_pages;   /* power-of-two-rounded page count */
static uint32_t   pmm_free_pages_n;

/* Bitmaps sized for the worst case (256 pages). */
static uint8_t bm_ord0[PMM_MAX_PAGES / 8];
static uint8_t bm_ord1[PMM_MAX_PAGES / 16];
static uint8_t bm_ord2[PMM_MAX_PAGES / 32];
static uint8_t bm_ord3[PMM_MAX_PAGES / 64];
static uint8_t bm_ord4[PMM_MAX_PAGES / 128];
static uint8_t bm_ord5[1];
static uint8_t bm_ord6[1];
static uint8_t *const bm_ptrs[PMM_NR_ORDERS] = {
    bm_ord0, bm_ord1, bm_ord2, bm_ord3, bm_ord4, bm_ord5, bm_ord6,
};
static const uint32_t bm_sizes[PMM_NR_ORDERS] = {
    sizeof bm_ord0, sizeof bm_ord1, sizeof bm_ord2, sizeof bm_ord3,
    sizeof bm_ord4, sizeof bm_ord5, sizeof bm_ord6,
};

/* ---- bit helpers ---- */
static inline int  bit_get(uint8_t *m, uint32_t i){ return (m[i>>3] >> (i&7)) & 1; }
static inline void bit_set(uint8_t *m, uint32_t i){ m[i>>3] |=  (1u << (i&7)); }
static inline void bit_clr(uint8_t *m, uint32_t i){ m[i>>3] &= ~(1u << (i&7)); }

/* ---- pfn/block helpers ---- */
static inline uint32_t pfn_of(void *p) {
    return (uint32_t)(((uintptr_t)p - pmm_base) >> PAGE_SHIFT);
}
static inline void *addr_of_pfn(uint32_t pfn) {
    return (void *)(pmm_base + ((uintptr_t)pfn << PAGE_SHIFT));
}
static inline uint32_t blk_idx(uint32_t pfn, int k) { return pfn >> k; }

/* ---- freelist primitives ---- */
static void fl_push(int k, void *blk) {
    struct free_node *n = (struct free_node *)blk;
    n->next = bd[k].freelist;
    bd[k].freelist = n;
}
static void *fl_pop(int k) {
    struct free_node *n = bd[k].freelist;
    if (n) bd[k].freelist = n->next;
    return n;
}
static int fl_remove(int k, void *blk) {
    struct free_node **pp = &bd[k].freelist;
    while (*pp) {
        if ((void *)*pp == blk) { *pp = (*pp)->next; return 1; }
        pp = &(*pp)->next;
    }
    return 0;
}



/* ---- core: allocate 2^order contiguous pages ---- */
void *pmm_alloc_pages(int order)
{
    if (order < 0 || order > PMM_MAX_ORDER) return NULL;

    taskENTER_CRITICAL();

    int k = order;
    while (k <= PMM_MAX_ORDER && bd[k].freelist == NULL) ++k;
    if (k > PMM_MAX_ORDER) {
        taskEXIT_CRITICAL();
        return NULL;
    }

    void *blk = fl_pop(k);
    uint32_t pfn = pfn_of(blk);
    bit_set(bd[k].alloc_map, blk_idx(pfn, k));

    while (k > order) {
        --k;
        void *half = (void *)((uintptr_t)blk + (PAGE_SIZE << k));
        bit_clr(bd[k].alloc_map, blk_idx(pfn_of(half), k));
        fl_push(k, half);
        bit_set(bd[k].alloc_map, blk_idx(pfn, k));
    }

    pmm_free_pages_n -= (1u << order);
    taskEXIT_CRITICAL();
    return blk;
}

/* ---- core: free 2^order contiguous pages, coalesce up ---- */
void pmm_free_pages(void *blk, int order)
{
    if (!blk || order < 0 || order > PMM_MAX_ORDER) return;

    taskENTER_CRITICAL();

    uint32_t pfn = pfn_of(blk);
    int k = order;
    bit_clr(bd[k].alloc_map, blk_idx(pfn, k));

    while (k < PMM_MAX_ORDER) {
        uint32_t bi    = blk_idx(pfn, k);
        uint32_t buddy = bi ^ 1u;
        if (buddy >= bd[k].nr_blocks) break;
        if (bit_get(bd[k].alloc_map, buddy)) break;

        void *buddy_addr = addr_of_pfn(buddy << k);
        fl_remove(k, buddy_addr);
        bit_set(bd[k].alloc_map, bi);
        bit_set(bd[k].alloc_map, buddy);

        if (buddy < bi) {
            pfn = buddy << k;
            blk = buddy_addr;
        }
        ++k;
        bit_clr(bd[k].alloc_map, blk_idx(pfn, k));
    }
    fl_push(k, blk);

    pmm_free_pages_n += (1u << order);
    taskEXIT_CRITICAL();
}

/* ---- bootstrap: greedy-largest-aligned-first release ---- */
static int largest_order(uintptr_t s, uintptr_t e)
{
    uintptr_t remaining = e - s;

    for (int k = PMM_MAX_ORDER; k > 0; --k) {
        uintptr_t block_size    = (uintptr_t)PAGE_SIZE << k;
        int aligned_at_s  = (s % block_size) == 0;
        int fits_before_e = block_size <= remaining;

        if (aligned_at_s && fits_before_e)
            return k;
    }
    return 0;
}

static void pmm_add_range(uintptr_t s, uintptr_t e)
{
    s = ALIGN_UP(s, PAGE_SIZE);
    e = ALIGN_DOWN(e, PAGE_SIZE);
    while (s < e) {
        int k = largest_order(s, e);
        pmm_free_pages((void *)s, k);
        s += (uintptr_t)PAGE_SIZE << k;
    }
}

/* ---- init ---- */
void pmm_init(uintptr_t ram_start, uintptr_t ram_end,
              const struct pmm_reserved *resv, size_t nresv)
{
    /* Round the managed window to a multiple of 2^MAX_ORDER pages so that
       block-index math is well defined. The bitmap size we reserved
       statically assumes PMM_MAX_PAGES, so assert we fit. */
    uintptr_t span = (uintptr_t)PAGE_SIZE << PMM_MAX_ORDER;
    pmm_base = ALIGN_UP(ram_start, span);
    uintptr_t top = ALIGN_DOWN(ram_end, span);
    pmm_window_pages = (uint32_t)((top - pmm_base) >> PAGE_SHIFT);
    configASSERT(pmm_window_pages <= PMM_MAX_PAGES);

    for (int k = 0; k < PMM_NR_ORDERS; ++k) {
        bd[k].freelist  = NULL;
        bd[k].nr_blocks = pmm_window_pages >> k;
        bd[k].alloc_map = bm_ptrs[k];
        memset(bm_ptrs[k], 0xFF, bm_sizes[k]);   /* everything taken */
    }
    pmm_free_pages_n = 0;

    /* Walk the real-RAM part of the window and carve out reserved holes. */
    uintptr_t cursor = ram_start;
    for (size_t i = 0; i < nresv; ++i) {
        uintptr_t rs = ALIGN_DOWN(resv[i].start, PAGE_SIZE);
        uintptr_t re = ALIGN_UP(resv[i].end,     PAGE_SIZE);
        if (re <= cursor) continue;
        if (rs > cursor) pmm_add_range(cursor, rs);
        cursor = re;
    }
    if (cursor < ram_end) pmm_add_range(cursor, ram_end);
}

uint32_t pmm_free_pages_count(void) { return pmm_free_pages_n; }
