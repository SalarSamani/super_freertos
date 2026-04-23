/* portASM.S — S-mode trap entry, context switch, first-task launch.
 *
 * All traps (timer IRQ, ecall from U or S, unexpected exceptions) land at
 * s_trap_entry.  The handler saves context onto the current stack, dispatches
 * on scause via s_trap_dispatch (C), then restores context from
 * pxCurrentTCB->pxTopOfStack and srets back to the (possibly new) task.
 *
 * Section 5: sscratch-swap protocol
 * ----------------------------------
 * Invariant: sscratch == 0 when running in S-mode; sscratch == kstack_top
 *            when the current task is about to run in U-mode.
 *
 * On entry, csrrw sp, sscratch, sp atomically swaps sp <-> sscratch:
 *   - Trap from S-mode: sscratch was 0 → sp becomes 0, sscratch gets S-sp.
 *     We swap back immediately; sp is the real kernel sp again.
 *   - Trap from U-mode: sscratch was kstack_top → sp becomes kstack_top,
 *     sscratch holds the user sp we must save into CTX_X2.
 */

#include "portContext.h"

    .equ SCAUSE_STI,     5   /* supervisor timer interrupt */

    .section .text, "ax"

    .extern pxCurrentTCB
    .extern vTaskSwitchContext
    .extern xTaskIncrementTick
    .extern s_trap_dispatch

/* -----------------------------------------------------------------------
 * s_trap_entry  (stvec, direct mode)
 * ----------------------------------------------------------------------- */
    .align 4
    .globl s_trap_entry
    .type  s_trap_entry, @function
s_trap_entry:
    /* ---- sscratch swap ---- */
    csrrw  sp, sscratch, sp       /* swap sp <-> sscratch */
    bnez   sp, .Lfrom_u           /* sp != 0 → came from U-mode */

    /* Came from S-mode: sscratch now holds the real S-mode sp; swap back. */
    csrrw  sp, sscratch, sp
    j      .Lframe_build

.Lfrom_u:
    /* Came from U-mode: sp = kstack_top (from sscratch),
     * sscratch = user sp (will be saved into CTX_X2 below). */

.Lframe_build:
    addi  sp, sp, -CTX_SIZE

    /* Save all GPRs except x2 (sp) — handled separately below. */
    sw    x1,  CTX_X1(sp)
    sw    x3,  CTX_X3(sp)
    sw    x4,  CTX_X4(sp)
    sw    x5,  CTX_X5(sp)
    sw    x6,  CTX_X6(sp)
    sw    x7,  CTX_X7(sp)
    sw    x8,  CTX_X8(sp)
    sw    x9,  CTX_X9(sp)
    sw    x10, CTX_X10(sp)
    sw    x11, CTX_X11(sp)
    sw    x12, CTX_X12(sp)
    sw    x13, CTX_X13(sp)
    sw    x14, CTX_X14(sp)
    sw    x15, CTX_X15(sp)
    sw    x16, CTX_X16(sp)
    sw    x17, CTX_X17(sp)
    sw    x18, CTX_X18(sp)
    sw    x19, CTX_X19(sp)
    sw    x20, CTX_X20(sp)
    sw    x21, CTX_X21(sp)
    sw    x22, CTX_X22(sp)
    sw    x23, CTX_X23(sp)
    sw    x24, CTX_X24(sp)
    sw    x25, CTX_X25(sp)
    sw    x26, CTX_X26(sp)
    sw    x27, CTX_X27(sp)
    sw    x28, CTX_X28(sp)
    sw    x29, CTX_X29(sp)
    sw    x30, CTX_X30(sp)
    sw    x31, CTX_X31(sp)

    /* Save sstatus; use SPP bit to decide how to save x2. */
    csrr  t0, sstatus
    sw    t0, CTX_SSTAT(sp)
    andi  t1, t0, (1 << 8)        /* SPP: 1=S-mode, 0=U-mode */
    bnez  t1, .Lsave_sp_s

    /* From U-mode: user sp is in sscratch; clear sscratch (→ S-mode invariant). */
    csrrw t2, sscratch, zero
    sw    t2, CTX_X2(sp)
    j     .Lsave_sepc

.Lsave_sp_s:
    /* From S-mode: real sp at trap time was sp + CTX_SIZE. */
    addi  t2, sp, CTX_SIZE
    sw    t2, CTX_X2(sp)

.Lsave_sepc:
    csrr  t0, sepc
    sw    t0, CTX_SEPC(sp)

    /* Save the previous pxTopOfStack into this frame, then update
     * pxTopOfStack to point at this (possibly nested) frame.  On return
     * restore_context will write the saved value back, so nested S→S
     * traps do not leave pxTopOfStack dangling at a popped inner frame. */
    la    t0, pxCurrentTCB
    lw    t1, 0(t0)
    lw    t3, 0(t1)
    sw    t3, CTX_PREVTOS(sp)
    sw    sp, 0(t1)

    /* Dispatch: timer interrupt is handled inline; everything else goes to C. */
    csrr  t0, scause
    bgez  t0, .Lsync_dispatch     /* MSB clear → synchronous */

    /* Interrupt path: check for STI. */
    slli  t0, t0, 1
    srli  t0, t0, 1
    li    t1, SCAUSE_STI
    bne   t0, t1, .Lto_c_dispatch

    /* Timer interrupt — bump mtimecmp inline (same as before). */
    li    t1, 0x30000
    lw    t0, 0(t1)
    lw    t2, 4(t1)
    li    ra, CONFIG_TIMER_RELOAD
    add   t0, t0, ra
    sltu  ra, t0, ra
    add   t2, t2, ra
    li    t1, 0x30008
    sw    zero, 4(t1)
    sw    t0,   0(t1)
    sw    t2,   4(t1)

    call  xTaskIncrementTick
    beqz  a0, 1f
    call  vTaskSwitchContext
1:
    j     restore_context

.Lsync_dispatch:
.Lto_c_dispatch:
    /* All synchronous exceptions (ecall U=8, ecall S=9, faults, etc.)
     * and unexpected interrupts go to the C dispatcher. */
    mv    a0, sp
    call  s_trap_dispatch
    /* fall through */

restore_context:
    /* Load sp from the (possibly new) current task's TCB. */
    la    t0, pxCurrentTCB
    lw    t1, 0(t0)
    lw    sp, 0(t1)

    /* Restore the previous pxTopOfStack (undoing the write in s_trap_entry).
     * After this, pxTopOfStack once again points at the outer frame (if any)
     * or the top-of-stack marker set by prvInitialiseUserStack. */
    lw    t2, CTX_PREVTOS(sp)
    sw    t2, 0(t1)

    lw    t0, CTX_SEPC(sp);  csrw  sepc,    t0
    lw    t0, CTX_SSTAT(sp); csrw  sstatus, t0

    lw    x1,  CTX_X1(sp)
    lw    x3,  CTX_X3(sp)
    lw    x4,  CTX_X4(sp)
    lw    x5,  CTX_X5(sp)
    lw    x6,  CTX_X6(sp)
    lw    x7,  CTX_X7(sp)
    lw    x8,  CTX_X8(sp)
    lw    x9,  CTX_X9(sp)
    lw    x10, CTX_X10(sp)
    lw    x11, CTX_X11(sp)
    lw    x12, CTX_X12(sp)
    lw    x13, CTX_X13(sp)
    lw    x14, CTX_X14(sp)
    lw    x15, CTX_X15(sp)
    lw    x16, CTX_X16(sp)
    lw    x17, CTX_X17(sp)
    lw    x18, CTX_X18(sp)
    lw    x19, CTX_X19(sp)
    lw    x20, CTX_X20(sp)
    lw    x21, CTX_X21(sp)
    lw    x22, CTX_X22(sp)
    lw    x23, CTX_X23(sp)
    lw    x24, CTX_X24(sp)
    lw    x25, CTX_X25(sp)
    lw    x26, CTX_X26(sp)
    lw    x27, CTX_X27(sp)
    lw    x28, CTX_X28(sp)
    lw    x29, CTX_X29(sp)
    lw    x30, CTX_X30(sp)
    lw    x31, CTX_X31(sp)

    /* If returning to U-mode (SPP==0 in sstatus), restore sscratch to
     * kstack_top so the next U→S trap finds a valid kernel stack.
     * kstack_top == current sp + CTX_SIZE (frame's base before entry). */
    csrr  t0, sstatus
    andi  t0, t0, (1 << 8)
    bnez  t0, .Lskip_scratch
    addi  t0, sp, CTX_SIZE
    csrw  sscratch, t0
.Lskip_scratch:

    /* Restore sp last (CTX_X2 holds either user sp or kernel sp+CTX_SIZE). */
    lw    sp, CTX_X2(sp)
    sret

panic:
    li    t0, 0x20008
    li    t1, 1
    sw    t1, 0(t0)
1:  j     1b
    .size s_trap_entry, .-s_trap_entry

/* -----------------------------------------------------------------------
 * xPortStartFirstTask — load the first task's context and sret into it.
 * ----------------------------------------------------------------------- */
    .globl xPortStartFirstTask
    .type  xPortStartFirstTask, @function
xPortStartFirstTask:
    la    t0, pxCurrentTCB
    lw    t1, 0(t0)
    lw    sp, 0(t1)
    j     restore_context
    .size xPortStartFirstTask, .-xPortStartFirstTask
