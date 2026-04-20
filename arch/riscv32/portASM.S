/* portASM.S — S-mode trap entry, context switch, first-task launch.
 *
 * All traps (timer IRQ, ecall yield, unexpected exceptions) land at
 * s_trap_entry. The handler saves context onto the current stack, dispatches
 * on scause, optionally calls vTaskSwitchContext, then restores context from
 * pxCurrentTCB->pxTopOfStack and srets back to the (possibly new) task.
 */

#include "portContext.h"

    .equ SCAUSE_ECALL_S, 9   /* ecall from S-mode */
    .equ SCAUSE_STI,     5   /* supervisor timer interrupt */

    .section .text, "ax"

    .extern pxCurrentTCB
    .extern vTaskSwitchContext
    .extern xTaskIncrementTick
    .extern portasm_handle_exception

/* -----------------------------------------------------------------------
 * s_trap_entry  (stvec, direct mode)
 *
 * Entry: sp is the interrupted task's kernel stack pointer.
 *        sscratch = 0 (we only handle S-mode traps for now).
 * ----------------------------------------------------------------------- */
    .align 4
    .globl s_trap_entry
    .type  s_trap_entry, @function
s_trap_entry:
    addi  sp, sp, -CTX_SIZE   /* reserve context frame */

    /* Save all GPRs. x2 (sp) needs its pre-trap value. */
    sw    x1,  CTX_X1(sp)
    addi  t0, sp, CTX_SIZE
    sw    t0,  CTX_X2(sp)     /* original sp */
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

    csrr  t0, sepc
    sw    t0, CTX_SEPC(sp)
    csrr  t0, sstatus
    sw    t0, CTX_SSTAT(sp)

    /* pxCurrentTCB->pxTopOfStack = sp  (first field of TCB) */
    la    t0, pxCurrentTCB
    lw    t1, 0(t0)
    sw    sp, 0(t1)

    csrr  t0, scause
    bgez  t0, exception_path   /* MSB clear → synchronous exception */

interrupt_path:
    slli  t0, t0, 1            /* strip interrupt bit */
    srli  t0, t0, 1
    li    t1, SCAUSE_STI
    beq   t0, t1, handle_timer
    j     panic                /* unexpected interrupt */

handle_timer:
    /* Bump mtimecmp to deassert STIP (Ibex: irq_timer is level-sensitive
     * and wired directly to mip.STIP via mideleg — only mtimecmp clears it). */
    li    t1, 0x30000              /* mtime_lo */
    lw    t0, 0(t1)
    lw    t2, 4(t1)               /* mtime_hi */
    li    ra, CONFIG_TIMER_RELOAD
    add   t0, t0, ra
    sltu  ra, t0, ra              /* carry into hi */
    add   t2, t2, ra
    li    t1, 0x30008              /* mtimecmp_lo */
    sw    zero, 4(t1)             /* write hi=0 first to avoid spurious match */
    sw    t0,   0(t1)
    sw    t2,   4(t1)

    call  xTaskIncrementTick      /* returns non-zero if switch needed */
    beqz  a0, restore_context
    call  vTaskSwitchContext
    j     restore_context

exception_path:
    li    t1, SCAUSE_ECALL_S
    beq   t0, t1, handle_ecall

    /* Unexpected exception — print scause/stval/sepc and halt. */
    mv    a0, t0
    csrr  a1, stval
    csrr  a2, sepc
    call  portasm_handle_exception
    j     panic

handle_ecall:
    /* Advance sepc past the ecall so sret resumes at the next instruction. */
    lw    t0, CTX_SEPC(sp)
    addi  t0, t0, 4
    sw    t0, CTX_SEPC(sp)
    call  vTaskSwitchContext
    /* fall through to restore_context */

restore_context:
    /* Load sp from the (possibly new) current task's TCB. */
    la    t0, pxCurrentTCB
    lw    t1, 0(t0)
    lw    sp, 0(t1)

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
    addi  sp, sp, CTX_SIZE    /* restore sp last */
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
