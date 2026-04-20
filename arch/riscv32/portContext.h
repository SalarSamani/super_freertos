/* ============================================================
 * portContext.h — Saved-register layout and CSR indices.
 *
 * One saved context = 32 GPRs + sstatus + sepc = 34 × 4 = 136 B.
 * SP (x2) is the saved stack-top of the task; we save it twice
 * (once at offset 8 for convenience, and again by the fact that
 * the TCB's pxTopOfStack points at the saved frame).
 * ============================================================ */
#ifndef PORT_CONTEXT_H
#define PORT_CONTEXT_H

/* Offsets into a context frame. Must be kept in sync with the asm. */
#define CTX_X1    0       /* ra */
#define CTX_X2    4       /* sp  */
#define CTX_X3    8       /* gp  */
#define CTX_X4    12      /* tp  */
#define CTX_X5    16      /* t0  */
#define CTX_X6    20
#define CTX_X7    24
#define CTX_X8    28      /* s0/fp */
#define CTX_X9    32
#define CTX_X10   36      /* a0 */
#define CTX_X11   40
#define CTX_X12   44
#define CTX_X13   48
#define CTX_X14   52
#define CTX_X15   56
#define CTX_X16   60
#define CTX_X17   64
#define CTX_X18   68
#define CTX_X19   72
#define CTX_X20   76
#define CTX_X21   80
#define CTX_X22   84
#define CTX_X23   88
#define CTX_X24   92
#define CTX_X25   96
#define CTX_X26   100
#define CTX_X27   104
#define CTX_X28   108
#define CTX_X29   112
#define CTX_X30   116
#define CTX_X31   120
#define CTX_SEPC  124
#define CTX_SSTAT 128
#define CTX_PAD   132     /* keep 16-byte alignment */
#define CTX_SIZE  136

#endif