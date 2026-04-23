/* super_freertos/arch/riscv32/slab.c */
#include "slab.h"
#include "pmm.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* ---- list ops ---- */
static inline void list_init(list_node_t *h) { h->prev = h->next = h; }
static inline bool list_empty(const list_node_t *h) { return h->next == h; }
static inline void list_remove(list_node_t *n) {
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = n->next = n;
}
static inline void list_push_front(list_node_t *h, list_node_t *n) {
    n->next = h->next;
    n->prev = h;
    h->next->prev = n;
    h->next = n;
}

/* ---- size classes ---- */
#define N_KMALLOC_CACHES 6
static const uint16_t kmalloc_sizes[N_KMALLOC_CACHES] = {
    32, 64, 128, 256, 512, 1024
};
static const char *const kmalloc_names[N_KMALLOC_CACHES] = {
    "k-32", "k-64", "k-128", "k-256", "k-512", "k-1024"
};
static kmem_cache_t kmalloc_caches[N_KMALLOC_CACHES];

static inline uint16_t align_up16(uint16_t v, uint16_t a) {
    return (uint16_t)((v + a - 1) & ~(a - 1));
}

/* ---- PA/VA translation ----
 * Bootstrap: megapage VA 0x80000000 → PA 0x00000000, so VA = PA + 0x80000000.
 * After mmu_install_physmap(): physmap VA 0xC0000000 → PA 0x00000000,
 * so VA = PA + 0xC0000000.
 *
 * We use a function pointer toggled by slab_switch_to_physmap() so the same
 * slab code works before and after physmap install without a branch per call. */
static void *pa_to_kva_bootstrap(void *pa) {
    return (void *)((uintptr_t)pa + 0x80000000u);
}
static void *kva_to_pa_bootstrap(void *kva) {
    return (void *)((uintptr_t)kva - 0x80000000u);
}
static void *pa_to_kva_physmap(void *pa) {
    return (void *)((uintptr_t)pa + 0xC0000000u);
}
static void *kva_to_pa_physmap(void *kva) {
    return (void *)((uintptr_t)kva - 0xC0000000u);
}

static void *(*s_pa_to_kva)(void *) = pa_to_kva_bootstrap;
static void *(*s_kva_to_pa)(void *) = kva_to_pa_bootstrap;

static inline void *pa_to_kva(void *pa)  { return s_pa_to_kva(pa); }
static inline void *kva_to_pa(void *kva) { return s_kva_to_pa(kva); }

/* Called by main() immediately after mmu_install_physmap(). */
void slab_switch_to_physmap(void)
{
    s_pa_to_kva = pa_to_kva_physmap;
    s_kva_to_pa = kva_to_pa_physmap;
}

/* ---- cache init ---- */
static void kmem_cache_init_one(kmem_cache_t *c, const char *name,
                                uint16_t obj_size)
{
    if (obj_size < sizeof(void *)) obj_size = sizeof(void *);
    obj_size = align_up16(obj_size, (uint16_t)sizeof(void *));
    c->name          = name;
    c->obj_size      = obj_size;
    c->hdr_size      = align_up16((uint16_t)sizeof(slab_t), obj_size);
    c->objs_per_slab = (uint16_t)((PAGE_SIZE - c->hdr_size) / obj_size);
    c->empty_cache   = NULL;
    c->n_alloc = c->n_free = c->n_slabs = 0;
    list_init(&c->partial);
    list_init(&c->full);
}

void slab_init(void)
{
    for (int i = 0; i < N_KMALLOC_CACHES; ++i)
        kmem_cache_init_one(&kmalloc_caches[i],
                            kmalloc_names[i], kmalloc_sizes[i]);
}

/* ---- page <-> slab plumbing ---- */
static slab_t *slab_new_from_buddy(kmem_cache_t *c)
{
    void *pa = pmm_alloc_page();
    if (!pa) return NULL;
    slab_t *s = (slab_t *)pa_to_kva(pa);
    s->cache    = c;
    s->inuse    = 0;
    s->total    = c->objs_per_slab;
    list_init(&s->link);

    uint8_t *base = (uint8_t *)s + c->hdr_size;
    void *prev = NULL;
    for (unsigned i = 0; i < s->total; ++i) {
        void *obj = base + i * c->obj_size;
        *(void **)obj = prev;
        prev = obj;
    }
    s->freelist = prev;
    c->n_slabs++;
    return s;
}

static void slab_release_to_buddy(kmem_cache_t *c, slab_t *s)
{
    c->n_slabs--;
    pmm_free_page(kva_to_pa(s));
}

/* ---- allocation fast path ---- */
void *kmem_cache_alloc(kmem_cache_t *c)
{
    slab_t *s;

    taskENTER_CRITICAL();

    if (!list_empty(&c->partial)) {
        s = (slab_t *)((uint8_t *)c->partial.next - offsetof(slab_t, link));
    } else if (c->empty_cache) {
        s = c->empty_cache;
        c->empty_cache = NULL;
        list_push_front(&c->partial, &s->link);
    } else {
        taskEXIT_CRITICAL();         /* buddy has its own critical section */
        s = slab_new_from_buddy(c);
        taskENTER_CRITICAL();
        if (!s) { taskEXIT_CRITICAL(); return NULL; }
        list_push_front(&c->partial, &s->link);
    }

    void *obj = s->freelist;
    s->freelist = *(void **)obj;
    s->inuse++;
    c->n_alloc++;

    if (s->inuse == s->total) {
        list_remove(&s->link);
        list_push_front(&c->full, &s->link);
    }
    taskEXIT_CRITICAL();
    return obj;
}

void kmem_cache_free(kmem_cache_t *c, void *p)
{
    slab_t *s = (slab_t *)PAGE_BASE(p);

    taskENTER_CRITICAL();
    bool was_full = (s->inuse == s->total);
    *(void **)p = s->freelist;
    s->freelist = p;
    s->inuse--;
    c->n_free++;

    if (was_full) {
        list_remove(&s->link);
        list_push_front(&c->partial, &s->link);
    }
    if (s->inuse == 0) {
        list_remove(&s->link);
        if (c->empty_cache == NULL) {
            c->empty_cache = s;
            taskEXIT_CRITICAL();
        } else {
            taskEXIT_CRITICAL();
            slab_release_to_buddy(c, s);
        }
        return;
    }
    taskEXIT_CRITICAL();
}

/* ---- public kmalloc / kfree ---- */
kmem_cache_t *kmem_cache_lookup(size_t sz)
{
    for (int i = 0; i < N_KMALLOC_CACHES; ++i)
        if (sz <= kmalloc_caches[i].obj_size) return &kmalloc_caches[i];
    return NULL;
}

void *kmalloc(size_t sz)
{
    if (sz == 0) return NULL;

    kmem_cache_t *c = kmem_cache_lookup(sz);
    if (c) return kmem_cache_alloc(c);

    /* Large path: direct page allocation. Mark the slab header with
       cache=NULL so kfree distinguishes this case. */
    if (sz > PAGE_SIZE - sizeof(slab_t)) return NULL;
    void *pa = pmm_alloc_page();
    if (!pa) return NULL;
    slab_t *s = (slab_t *)pa_to_kva(pa);
    s->cache    = NULL;
    s->total    = 1;
    s->inuse    = 1;
    s->freelist = NULL;
    return (uint8_t *)s + sizeof(slab_t);
}

void kfree(void *p)
{
    if (!p) return;
    slab_t *s = (slab_t *)PAGE_BASE(p);
    if (s->cache == NULL) {
        pmm_free_page(kva_to_pa(s));
        return;
    }
    kmem_cache_free(s->cache, p);
}
