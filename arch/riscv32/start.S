/* ============================================================
 * start.S  —  Reset → M-mode setup → mret into S-mode → MMU on
 * Ibex simple_system, super_ibex (S-mode + Sv32)
 * ============================================================ */

    .section .text.boot, "ax"
    .globl _start
    .type  _start, @function

/* --------- Linker-provided symbols -------------------------- */
    .extern __bss_start
    .extern __bss_end
    .extern __freertos_irq_stack_top   /* top of ISR stack, HIGH-HALF VA */
    .extern __boot_stack_top           /* small stack used pre-satp, PA */
    .extern g_kernel_root_pt           /* root page table, PA */
    .extern mmu_build_identity_and_highhalf  /* C helper in mmu.c */
    .extern main                        /* kernel entry, linked at 0x80000000+ */
    .extern s_trap_entry                /* S-mode trap vector in portASM.S */
    .extern m_trap_entry                /* thin M-mode trap stub (below) */

/* ======================================================================
 *  Reset entry — M-mode, MMU off, PC=0x00100080
 * ====================================================================== */
/* ---- Macro: write a single ASCII char to UART (0x20000), clobbers t6 -- */
.macro UART_CHAR c
    li    t6, 0x20000
    li    t5, \c
    sw    t5, 0(t6)
.endm

/* ---- Macro: write NUL-terminated string literal to UART --------------- */
.macro UART_STR label
    la    t4, \label
99: lbu   t5, 0(t4)
    beqz  t5, 100f
    li    t6, 0x20000
    sw    t5, 0(t6)
    addi  t4, t4, 1
    j     99b
100:
.endm

_start:
    /* Disable all interrupts until we're ready. */
    csrw  mie,  zero
    csrw  mip,  zero

    UART_STR boot_msg_mmode

    /* Load a tiny stack in physical RAM so C helpers can run before MMU.  */
    la    sp,  __boot_stack_top

    /* Install M-mode trap stub. It only catches bugs during boot and, once
     * we're in S-mode, forwards the machine timer interrupt to S-mode by
     * asserting SIP.STIP.                                                  */
    la    t0,  m_trap_entry
    csrw  mtvec, t0              /* direct mode (low bits = 00) */

    /* --------------------------------------------------------
     * Delegate exceptions and interrupts to S-mode.
     *
     *   medeleg: one bit per synchronous exception cause (0..15)
     *            0xB1FF delegates everything EXCEPT:
     *              - ecall-from-M  (bit 11) — must stay in M
     *              - reserved (bits 10, 14)
     *            For simplicity & to match the Linux convention we use
     *            0xB1FF = 0b1011_0001_1111_1111 .
     *
     *   mideleg: SSIE(1) | STIE(5) | SEIE(9)  = 0x222
     * -------------------------------------------------------- */
    li    t0, 0xB3FF
    csrw  medeleg, t0
    li    t0, 0x222
    csrw  mideleg, t0

    /* Enable the machine timer interrupt only in M-mode; S-mode handles
     * its own supervisor timer via software forwarding.                   */
    li    t0, (1 << 7)           /* MTIE */
    csrw  mie, t0

    /* Stage mstatus for mret-into-S-mode:
     *   MPP  = 01 (S)  bits 12:11
     *   MPIE = 1       bit 7   → after mret, mstatus.MIE = 1
     *   SUM  = 0, MXR = 0
     *
     * MIE is only consulted when the hart is in M-mode. When we're in
     * S-mode, an M-level IRQ (like MTIP) is delivered whenever it is
     * pending & enabled in mie, regardless of mstatus.MIE. But some
     * cores are strict, and leaving MIE=1 on mret costs nothing, so
     * set MPIE=1 so that post-mret mstatus.MIE=1.                       */
    li    t0, (1 << 11) | (1 << 7)   /* MPP=S, MPIE=1 */
    csrs  mstatus, t0
    li    t0, (1 << 12)
    csrc  mstatus, t0                /* ensure bit 12 cleared → MPP=01, not 11 */

    /* Set mepc = _start_smode (physical addr — MMU still off). */
    la    t0, _start_smode
    csrw  mepc, t0

    mret                          /* → S-mode, PC = _start_smode */

/* ======================================================================
 *  S-mode bring-up — still running at physical addresses, MMU off
 * ====================================================================== */
    .globl _start_smode
    .type  _start_smode, @function
_start_smode:
    UART_STR boot_msg_smode

    /* We are now in S-mode. MMU is still OFF, so all memory accesses use
     * physical addresses. But every section — INCLUDING .text.boot —
     * is linked with a uniform VMA−LMA skew of +0x80000000, so
     * PC-relative `la` of any kernel symbol (code, rodata, or bss) from
     * pre-MMU code running at the low PA produces the correct *physical*
     * address automatically. No manual VA→PA subtraction needed here.
     *
     * Build the root page table IN PLACE at its physical address.
     *   Table has 1024 entries; we fill:
     *     - entries 0..511: identity megapage PA = (i << 22), flags=0xEF
     *     - entry     512 : megapage PA = 0,       flags=0xEF
     *     - entries 513..1023: zero
     *
     *   PTE for megapage i (identity): ((i << 22) >> 22) << 20 | 0xEF
     *                                = (i << 20) | 0xEF
     *   PTE for entry 512 (high-half): 0 | 0xEF = 0xEF
     */
    UART_STR boot_msg_mmu_build

    la    t0, g_kernel_root_pt    /* t0 = &g_kernel_root_pt (PA, because
                                     PC is running at low PA and `la` is
                                     PC-relative). */

    /* Fill identity entries [0..511]. */
    li    t2, 0                   /* i */
    li    t3, 512                 /* end index */
    li    t4, 0xEF                /* kernel RWX leaf flags */
3:  beq   t2, t3, 4f
    slli  t5, t2, 20              /* (i << 22) >> 22 << 20 = i << 20  */
    or    t5, t5, t4              /* | flags */
    slli  t6, t2, 2               /* byte offset = i * 4 */
    add   t6, t6, t0
    sw    t5, 0(t6)
    addi  t2, t2, 1
    j     3b
4:
    /* Entry 512: high-half window mapping VA 0x80000000 → PA 0x00000000.
     * Kernel image is linked at VMA 0x80000000 but physically loaded at
     * PA 0x00100000, so we actually need VA 0x80000000 → PA 0x00000000
     * to also cover the low MiB where the image sits — but the megapage
     * is 4 MiB wide so a single PTE with PPN=0 covers PA [0, 4MiB) which
     * includes the image at PA 0x00100000. Flags 0xEF = V|R|W|X|U?|G|A|D
     * (we want D|A|G|X|W|R|V = 0xEF with U cleared → kernel-only).
     *
     * PTE format (Sv32 megapage): [PPN1(31:20)|PPN0(19:10)|flags(9:0)]
     * For PA=0: PPN1=0, PPN0=0 → PTE = 0x000000EF.
     *
     * Offset 512*4 = 2048 is out of range for sw's 12-bit signed imm,
     * so compute the address in a register first.                      */
    li    t5, 2048
    add   t5, t0, t5
    sw    t4, 0(t5)

    /* Zero entries [513..1023]. */
    li    t2, 513
    li    t3, 1024
5:  beq   t2, t3, 6f
    slli  t6, t2, 2
    add   t6, t6, t0
    sw    zero, 0(t6)
    addi  t2, t2, 1
    j     5b
6:

    /* Write satp = MODE_Sv32 (bit 31) | (root_pt_PA >> 12). */
    srli  t0, t0, 12
    li    t1, (1 << 31)
    or    t0, t0, t1
    csrw  satp, t0
    sfence.vma zero, zero
    UART_STR boot_msg_mmu_on

    /* Jump to the high-half VA of _start_highhalf.
     *
     * Important: with -mcmodel=medany, `la` expands to auipc+addi — it is
     * PC-relative. Our PC is currently in the low identity window (PA
     * 0x001xxxxx), so `la _start_highhalf` produces a low-PA address, and
     * execution would stay in the identity window forever. We need an
     * ABSOLUTE load of the VA so the next instruction fetch uses bit-31-set
     * PC and hits the high-half mapping (megapage PTE 512). Use lui+addi
     * with explicit hi/lo relocs against the VA symbol.                  */
    lui   t0, %hi(_start_highhalf)
    addi  t0, t0, %lo(_start_highhalf)
    jr    t0

    .size _start_smode, .-_start_smode

/* ======================================================================
 *  High-half entry — PC is now 0x8xxxxxxx
 * ====================================================================== */
    .section .text, "ax"
    .globl _start_highhalf
    .type  _start_highhalf, @function
_start_highhalf:
    /* MMU is now on; high-half VAs resolve. We DIDN'T clear BSS earlier
     * because it lives at high-half VAs that weren't mapped yet. Clear
     * it now — but skip the already-populated page table region.         */
    la    t0, __bss_start
    la    t1, __bss_end
7:  beq   t0, t1, 8f
    sw    zero, 0(t0)
    addi  t0, t0, 4
    j     7b
8:

    /* Switch to the real kernel ISR stack in the high-half. */
    la    sp, __freertos_irq_stack_top

    /* Clear sscratch: convention — 0 means "we were already in S-mode". */
    csrw  sscratch, zero

    /* Re-install trap vector using its high-half VA (same symbol, now
     * resolved at 0x8xxxxxxx).                                            */
    la    t0, s_trap_entry
    csrw  stvec, t0

    UART_STR boot_msg_highhalf

    /* Hand off to C. main() creates tasks and starts the scheduler.      */
    call  main

    /* main() must never return. If it does, spin. */
3:  wfi
    j     3b
    .size _start_highhalf, .-_start_highhalf

/* ---- Boot diagnostic strings — kept in .text.boot so they sit at a
 *      physical address reachable before and after satp is set.
 *      Use a non-executable sub-section ("a" not "ax") so the linker
 *      doesn't try to apply instruction relaxation over .asciz data.   */
    .section .text.boot.str, "a", @progbits
    .balign 4
boot_msg_mmode:
    .asciz "[BOOT] M-mode entry\n"
boot_msg_smode:
    .asciz "[BOOT] S-mode entry (MMU off)\n"
boot_msg_mmu_build:
    .asciz "[BOOT] building page table...\n"
boot_msg_mmu_on:
    .asciz "[BOOT] satp set, MMU on, jumping to high-half\n"
boot_msg_highhalf:
    .asciz "[BOOT] high-half reached, handing off to main()\n"
    .balign 4

/* ======================================================================
 *  M-mode trap stub — forwards machine timer IRQ to S-mode
 *
 *  Simple_system's timer is memory-mapped and raises the MACHINE timer
 *  interrupt (MTIP). Because we delegated STIP to S-mode but the hardware
 *  only raises MTIP, we catch MTIP in M-mode, clear it by bumping
 *  mtimecmp, and set SIP.STIP so the S-mode handler runs.
 * ====================================================================== */
    .section .text.boot, "ax"
    .align 2
    .globl m_trap_entry
m_trap_entry:
    /* On Ibex, mideleg.STIP=1 routes the timer irq directly to mip.STIP,
     * so M-mode never sees timer traps. This stub exists only to catch
     * unexpected M-mode traps during boot. Panic on anything we hit.   */
    /* Re-enter using a tiny save area on the stack.  We don't have a
     * dedicated M-mode stack, so we reuse the current one.                */
    addi  sp, sp, -16
    sw    t0, 0(sp)
    sw    t1, 4(sp)
    sw    t2, 8(sp)
    sw    ra, 12(sp)

    csrr  t0, mcause
    bltz  t0, m_irq                /* bit 31 set → interrupt */

    /* Exception in M-mode: we didn't expect any. Halt the simulator.     */
    li    t1, 0x20008
    li    t2, 1
    sw    t2, 0(t1)
1:  wfi
    j     1b

m_irq:
    /* mcause low bits == 7 → Machine Timer. Anything else: panic.        */
    slli  t0, t0, 1                /* drop interrupt bit */
    srli  t0, t0, 1
    li    t1, 7
    bne   t0, t1, m_panic

    /* Bump mtimecmp = mtime + TICK_RELOAD. This both acknowledges the IRQ
     * (mtime < mtimecmp again) and schedules the next tick.               */
    li    t1, 0x00030000           /* mtime_lo */
    lw    t0, 0(t1)                /* cur_lo */
    lw    t2, 4(t1)                /* cur_hi */
    li    ra, CONFIG_TIMER_RELOAD  /* see FreeRTOSConfig.h; passed by -D */
    add   t0, t0, ra
    sltu  ra, t0, ra               /* carry */
    add   t2, t2, ra
    li    t1, 0x00030008           /* mtimecmp_lo */
    sw    zero, 4(t1)              /* write high = 0 to avoid race */
    sw    t0, 0(t1)
    sw    t2, 4(t1)

    /* Assert SIP.STIP so S-mode takes the supervisor timer interrupt. */
    li    t0, (1 << 5)
    csrs  sip, t0
    /* Also re-enable SIE.STIE in case S-mode handler masked it as a
     * workaround for STIP being sticky.                              */
    csrs  sie, t0

    lw    ra, 12(sp)
    lw    t2, 8(sp)
    lw    t1, 4(sp)
    lw    t0, 0(sp)
    addi  sp, sp, 16
    mret

m_panic:
    li    t1, 0x20008
    li    t2, 1
    sw    t2, 0(t1)
1:  wfi
    j     1b
    .size m_trap_entry, .-m_trap_entry