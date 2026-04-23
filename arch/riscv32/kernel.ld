/* ============================================================
 * kernel.ld — Higher-half linker script for super_ibex FreeRTOS.
 *
 *   Physical RAM : 0x00100000 .. 0x001FFFFF  (1 MiB)
 *   Boot VA      : 0x00100080  (reset PC)
 *   Kernel VMA   : 0x80000000  (after MMU on)
 *
 *   .text.boot is linked AND loaded at the low PA so the CPU can fetch
 *   the very first instruction after reset (MMU off).
 *   .text/.rodata/.data/.bss are linked at VMA 0x80000000+offset but
 *   LOADED at PA 0x00100000+offset, so the .vmem file lays bytes into
 *   physical RAM.
 * ============================================================ */

OUTPUT_ARCH(riscv)
ENTRY(_start)

/* KERNEL_VMA must sit at the same 4 MiB-megapage-internal offset as
 * KERNEL_LMA so a single Sv32 L1 megapage PTE can map them.
 * Megapage at entry 512 (VA base 0x80000000) has PPN=0, covering
 * VA [0x80000000, 0x80400000) → PA [0x00000000, 0x00400000). Our RAM
 * sits at PA 0x00100000, so the kernel must be linked at VA 0x80100000
 * to ride that same mapping.                                           */
MEMORY {
    RAM_PHYS (rwx) : ORIGIN = 0x00100000, LENGTH = 0x00100000    /* 1 MiB */
    RAM_HIGH (rwx) : ORIGIN = 0x80100000, LENGTH = 0x00100000    /* high-half alias */
}

/* The difference between VMA and LMA for high-half sections. */
KERNEL_VMA = 0x80100000;
KERNEL_LMA = 0x00100000;

SECTIONS
{
    /* ----------------------------------------------------------
     *  BOOT section — linked at high-half VA, loaded at low PA.
     *  We give .text.boot a VMA in the high-half window so that the
     *  VMA↔LMA relationship is a uniform +0x80000000 for EVERY
     *  loadable section. This lets a single Sv32 megapage (entry 512,
     *  PPN=0) translate every high-half kernel VA correctly after the
     *  MMU comes on.
     *
     *  Pre-MMU code in .text.boot is fetched by the CPU from its
     *  physical address (0x00100080 = Ibex reset PC). Any reference
     *  to a high-half symbol (e.g. g_kernel_root_pt) is converted to
     *  a PA using VA_TO_PA_OFFSET = 0x80000000 in start.S.
     * ---------------------------------------------------------- */
    . = KERNEL_VMA;
    .text.boot : AT(ADDR(.text.boot) - 0x80000000) {
        KEEP(*(.vectors))
        . = 0x80;                     /* reset PC lands here (PA 0x00100080) */
        KEEP(*(.text.boot))
        . = ALIGN(4);
        KEEP(*(.text.boot.str))
        . = ALIGN(4);
    } > RAM_HIGH

    /* ----------------------------------------------------------
     *  HIGH-HALF .text — every loadable section gets its LMA fixed
     *  at (VMA - 0x80000000), so the skew is EXACTLY +0x80000000
     *  per section regardless of sizes. This guarantees that a
     *  single Sv32 megapage mapping VA 0x80000000 → PA 0x00000000
     *  translates every kernel VA to the correct physical location.
     * ---------------------------------------------------------- */
    .text : AT(ADDR(.text) - 0x80000000) ALIGN(4096) {
        __text_start = .;
        _stext = .;
        *(.text.init)
        *(.text .text.*)
        __text_end = .;
        _etext = ALIGN(4096);
    } > RAM_HIGH

    .rodata : AT(ADDR(.rodata) - 0x80000000) ALIGN(4096) {
        _srodata = .;
        *(.rodata .rodata.* .srodata .srodata.*)
        _erodata = ALIGN(4096);
    } > RAM_HIGH

    .data : AT(ADDR(.data) - 0x80000000) ALIGN(4096) {
        __data_start = .;
        _sdata = .;
        *(.data .data.* .sdata .sdata.*)
        __data_end = .;
        _edata = .;
    } > RAM_HIGH

    /* ----------------------------------------------------------
     *  BSS — zeroed by start.S AFTER the pagetable region.
     *  .bss.pagetable MUST be first so start.S can skip it
     *  (it's been populated in-place before MMU turn-on).
     * ---------------------------------------------------------- */
    .bss (NOLOAD) : {
        . = ALIGN(4096);
        __pagetable_start = .;
        *(.bss.pagetable)
        __pagetable_end = .;

        __bss_start = .;
        _sbss = .;
        *(.bss .bss.* .sbss .sbss.* COMMON)

        /* ISR stack lives in high-half, used after we jump high. */
        . = ALIGN(16);
        __freertos_irq_stack_bottom = .;
        . += 0x2000;                     /* 8 KiB ISR stack */
        __freertos_irq_stack_top = .;

        __bss_end = .;
        _ebss = .;
    } > RAM_HIGH

    /* heap_4 arena in its own named section so the PMM can reserve it. */
    .heap4 (NOLOAD) : {
        _heap_start = .;
        KEEP(*(.heap4))
        _heap_end = .;
    } > RAM_HIGH

    /* Kernel VA end and derived PA end for PMM bootstrap.
     * Page-align so .user_text LMA is 4 KiB-aligned (required by Sv32). */
    . = ALIGN(4096);
    _kernel_end = .;
    _kernel_vma_start = 0x80100000;
    _kernel_phys_end = _kernel_end - 0x80000000;

    /* ----------------------------------------------------------
     *  User programs: each gets its own output section whose VMA is
     *  the same (0x00400000) but whose LMA is chained after the
     *  previous one inside the kernel image. The kernel maps each
     *  blob into a fresh address space at spawn time via
     *  mm_map_pages(vma, lma, R|X|U). The shared VMA is a fiction —
     *  no kernel code ever fetches from 0x00400000 directly — so we
     *  pass -Wl,--no-check-sections in LDFLAGS to silence the
     *  overlap complaint.
     * ---------------------------------------------------------- */
    .user_text.init 0x00400000 : AT(_kernel_phys_end) ALIGN(4096)
    {
        _user_init_vma = .;
        KEEP(*(.user_text.init))
        KEEP(*(.user_rodata.init))
        . = ALIGN(4096);
        _user_init_end = .;
    }
    _user_init_lma  = LOADADDR(.user_text.init);
    _user_init_size = _user_init_end - _user_init_vma;

    .user_text.hello 0x00400000 : AT(_user_init_lma + _user_init_size) ALIGN(4096)
    {
        _user_hello_vma = .;
        KEEP(*(.user_text.hello))
        KEEP(*(.user_rodata.hello))
        . = ALIGN(4096);
        _user_hello_end = .;
    }
    _user_hello_lma  = LOADADDR(.user_text.hello);
    _user_hello_size = _user_hello_end - _user_hello_vma;

    .user_text.bad 0x00400000 : AT(_user_hello_lma + _user_hello_size) ALIGN(4096)
    {
        _user_bad_vma = .;
        KEEP(*(.user_text.bad))
        KEEP(*(.user_rodata.bad))
        . = ALIGN(4096);
        _user_bad_end = .;
    }
    _user_bad_lma  = LOADADDR(.user_text.bad);
    _user_bad_size = _user_bad_end - _user_bad_vma;

    /* Back-compat aliases so existing code that still references
     * _user_text_vma/_lma/_size keeps working until we migrate it
     * (spawn.c still spawns exactly one user task: init). */
    _user_text_vma  = _user_init_vma;
    _user_text_lma  = _user_init_lma;
    _user_text_size = _user_init_size;
    _user_text_end  = _user_init_end;

    /* ----------------------------------------------------------
     *  Tiny physical-address stack used before MMU is on.
     *  Its symbols must be physical addresses (pre-MMU use).
     *  Place it at a fixed low PA (near the end of RAM) so it does
     *  not collide with loaded sections.
     * ---------------------------------------------------------- */
    . = ORIGIN(RAM_PHYS) + LENGTH(RAM_PHYS) - 0x100;
    .boot_stack (NOLOAD) : {
        . = ALIGN(16);
        __boot_stack_bottom = .;
        . += 256;
        __boot_stack_top = .;
    } > RAM_PHYS

    /* Sanity: make sure kernel + all user programs fit in 1 MiB RAM. */
    ASSERT((_user_bad_lma + _user_bad_size - ORIGIN(RAM_PHYS))
           < LENGTH(RAM_PHYS),
           "ERROR: kernel+user image does not fit in 1 MiB RAM")

    /DISCARD/ : { *(.comment) *(.note.*) *(.eh_frame) }
}