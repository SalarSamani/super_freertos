/* super_freertos/arch/riscv32/slab.h */
#ifndef SLAB_H
#define SLAB_H

#include "pmm.h"   /* PAGE_SIZE, PAGE_MASK */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* PAGE_SIZE is defined in pmm.h; include that before slab.h */
#define PAGE_BASE(p) ((void *)((uintptr_t)(p) & ~(uintptr_t)(PAGE_MASK)))

typedef struct list_node {
    struct list_node *prev, *next;
} list_node_t;

struct kmem_cache;

typedef struct slab {
    struct kmem_cache *cache;     /* owning cache; NULL = raw page */
    void              *freelist;  /* singly-linked list of free objs */
    uint16_t           inuse;
    uint16_t           total;
    list_node_t        link;
} slab_t;

typedef struct kmem_cache {
    const char  *name;
    uint16_t     obj_size;
    uint16_t     objs_per_slab;
    uint16_t     hdr_size;
    list_node_t  partial;
    list_node_t  full;
    slab_t      *empty_cache;     /* at most one empty slab kept hot */
    uint32_t     n_alloc, n_free, n_slabs;
} kmem_cache_t;

void  slab_init(void);
void  slab_switch_to_physmap(void); /* call after mmu_install_physmap() */

void *kmalloc(size_t sz);
void  kfree(void *p);

/* Typed fast path: callers who already have a cache pointer skip the
   size-class lookup. */
kmem_cache_t *kmem_cache_lookup(size_t sz);
void *kmem_cache_alloc(kmem_cache_t *c);
void  kmem_cache_free(kmem_cache_t *c, void *p);

#endif
