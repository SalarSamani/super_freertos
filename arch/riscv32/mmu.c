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