/* ============================================================
 * mmu.h — Sv32 page-table helpers (S-mode, kernel only)
 * ============================================================ */
#ifndef ARCH_RISCV32_MMU_H
#define ARCH_RISCV32_MMU_H

#include <stdint.h>

/* PTE bit positions */
#define PTE_V   (1u << 0)
#define PTE_R   (1u << 1)
#define PTE_W   (1u << 2)
#define PTE_X   (1u << 3)
#define PTE_U   (1u << 4)
#define PTE_G   (1u << 5)
#define PTE_A   (1u << 6)
#define PTE_D   (1u << 7)

/* Combined leaf perms */
#define PTE_KERN_RWX  (PTE_V|PTE_R|PTE_W|PTE_X|PTE_G|PTE_A|PTE_D) /* 0xEF */
#define PTE_KERN_RW_  (PTE_V|PTE_R|PTE_W      |PTE_G|PTE_A|PTE_D) /* 0xE7 */

/* Convert PA → PTE PPN field (bits 31:10 of PTE).
 * In Sv32, PPN is 22 bits; PA is 34 bits (max), but our PA fits in 32. */
#define PA_TO_PTE_PPN(pa)   ((uint32_t)(((uint32_t)(pa)) >> 12) << 10)

/* Root PT is 1024 entries × 4 bytes = 4 KiB, must be 4 KiB aligned. */
#define ROOT_PT_ENTRIES 1024

extern uint32_t g_kernel_root_pt[ROOT_PT_ENTRIES]
    __attribute__((aligned(4096)));

/* Layout constants — edit in one place if the SoC ever moves. */
#define PHYS_RAM_BASE   0x00100000u
#define PHYS_RAM_SIZE   0x00100000u      /* 1 MiB */
#define KERNEL_VA_BASE  0x80000000u

void mmu_build_identity_and_highhalf(void);

#endif