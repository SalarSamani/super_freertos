/* port.c — FreeRTOS S-mode port for super_ibex (RV32 + Sv32 MMU). */

#include "FreeRTOS.h"
#include "task.h"
#include "portContext.h"
#include "mm.h"
#include <stdint.h>

#define SSTATUS_SPP   (1u << 8)   /* previous privilege = S */
#define SSTATUS_SPIE  (1u << 5)   /* enable interrupts on sret */

volatile UBaseType_t uxCriticalNesting = 0xaaaaaaaa;  /* init sentinel; 0 after scheduler start */

extern void xPortStartFirstTask(void);

#define csrw(reg, val)  __asm volatile ("csrw " #reg ", %0" :: "r"(val) : "memory")
#define csrr(reg)       ({ uint32_t __v; __asm volatile ("csrr %0, " #reg : "=r"(__v)); __v; })
#define csrs(reg, val)  __asm volatile ("csrs " #reg ", %0" :: "r"(val) : "memory")

/* Build a fake trap frame so restore_context in portASM.S can sret into the task. */
StackType_t *pxPortInitialiseStack(StackType_t *pxTopOfStack,
                                   TaskFunction_t pxCode,
                                   void *pvParameters)
{
    StackType_t *task_sp = (StackType_t *)(((uintptr_t)pxTopOfStack) & ~0xFu);  /* task's runtime sp */
    pxTopOfStack = task_sp - (CTX_SIZE / sizeof(StackType_t));
    uint8_t *frame = (uint8_t *)pxTopOfStack;

    for (unsigned i = 0; i < CTX_SIZE; i += 4)
        *(uint32_t *)(frame + i) = 0;

    extern void prvTaskExitError(void);
    extern void port_task_launch_shim(void);

    *(uint32_t *)(frame + CTX_X1)   = (uint32_t)prvTaskExitError;    /* ra: if task returns */
    *(uint32_t *)(frame + CTX_X2)   = (uint32_t)task_sp;             /* sp: top of task stack */
    *(uint32_t *)(frame + CTX_X10)  = (uint32_t)pvParameters;        /* a0: task argument */
    *(uint32_t *)(frame + CTX_X11)  = (uint32_t)pxCode;              /* a1: task entry (shim reads this) */
    *(uint32_t *)(frame + CTX_SEPC) = (uint32_t)port_task_launch_shim;
    *(uint32_t *)(frame + CTX_SSTAT) = SSTATUS_SPP | SSTATUS_SPIE;   /* sret → S-mode, interrupts on */
    /* CTX_PREVTOS = pointer to this frame itself: on restore, pxTopOfStack
     * stays pointing at a valid (the outermost) frame even if the task
     * never entered via a trap. */
    *(uint32_t *)(frame + CTX_PREVTOS) = (uint32_t)pxTopOfStack;

    return pxTopOfStack;
}

/* First entry into a task: a0=pvParameters, a1=pxCode. Jump to task code. */
__attribute__((naked)) void port_task_launch_shim(void)
{
    __asm volatile ("jr a1\n");
}

void prvTaskExitError(void)
{
    extern int putchar_sim(int c);
    const char *m = "\n[FATAL] task returned\n";
    while (*m) putchar_sim(*m++);
    portDISABLE_INTERRUPTS();
    for (;;) { }
}

/* ---- Timer setup --------------------------------------------------------
 * simple_system timer MMIO: mtime @ 0x30000, mtimecmp @ 0x30008.
 * Ibex routes irq_timer directly to mip.STIP (via mideleg), so S-mode owns
 * the timer entirely — no M-mode forwarding needed.
 * ------------------------------------------------------------------------- */
#define MTIME_LO    ((volatile uint32_t *)0x00030000)
#define MTIME_HI    ((volatile uint32_t *)0x00030004)
#define MTIMECMP_LO ((volatile uint32_t *)0x00030008)
#define MTIMECMP_HI ((volatile uint32_t *)0x0003000C)

static void vPortSetupTimerInterrupt(void)
{
    uint64_t now  = ((uint64_t)*MTIME_HI << 32) | *MTIME_LO;
    uint64_t next = now + (configCPU_CLOCK_HZ / configTICK_RATE_HZ);

    /* Write HI=max first, then LO, then real HI — prevents spurious early match. */
    *MTIMECMP_HI = 0xFFFFFFFFu;
    *MTIMECMP_LO = (uint32_t)(next & 0xFFFFFFFFu);
    *MTIMECMP_HI = (uint32_t)(next >> 32);

    csrs(sie, (1u << 5));      /* STIE: enable supervisor timer interrupt */
    csrs(sstatus, (1u << 1));  /* SIE:  global S-mode interrupt enable */
}

BaseType_t xPortStartScheduler(void)
{
    uxCriticalNesting = 0;
    vPortSetupTimerInterrupt();
    xPortStartFirstTask();  /* never returns */
    return pdFAIL;
}

void vPortEndScheduler(void) { for (;;) { } }

/* Called from portASM.S for any unexpected synchronous exception. Prints cause and halts. */
void portasm_handle_exception(uint32_t scause_, uint32_t stval_, uint32_t sepc_)
{
    extern int putchar_sim(int c);
    const char *hex = "0123456789ABCDEF";
    const char *m;

    m = "\n[PANIC] S-mode exception\n  scause=0x"; while (*m) putchar_sim(*m++);
    for (int i = 28; i >= 0; i -= 4) putchar_sim(hex[(scause_ >> i) & 0xF]);
    m = "\n  stval =0x"; while (*m) putchar_sim(*m++);
    for (int i = 28; i >= 0; i -= 4) putchar_sim(hex[(stval_  >> i) & 0xF]);
    m = "\n  sepc  =0x"; while (*m) putchar_sim(*m++);
    for (int i = 28; i >= 0; i -= 4) putchar_sim(hex[(sepc_   >> i) & 0xF]);
    putchar_sim('\n');
    *(volatile uint32_t *)0x20008 = 1;
    for (;;) { }
}

/* ============================================================
 * prvInitialiseUserStack — initial trap frame for a U-mode task
 *
 * Builds a context frame on the kernel stack that sret will
 * use to enter U-mode: SPP=0, SPIE=1, SUM=1, sepc=user_entry,
 * x2(sp)=user_sp, everything else zero.
 * ============================================================ */
#define SSTATUS_SUM   (1u << 18)   /* allow S-mode to touch U pages */

StackType_t *prvInitialiseUserStack(StackType_t *pxTopOfStack,
                                    uintptr_t user_entry_va,
                                    uintptr_t user_sp_va)
{
    pxTopOfStack = (StackType_t *)((uintptr_t)pxTopOfStack - CTX_SIZE);
    uint8_t *f = (uint8_t *)pxTopOfStack;
    for (unsigned i = 0; i < CTX_SIZE; i += 4)
        *(uint32_t *)(f + i) = 0;

    *(uint32_t *)(f + CTX_X2)   = (uint32_t)user_sp_va;
    *(uint32_t *)(f + CTX_SEPC) = (uint32_t)user_entry_va;
    /* SPP=0 → sret returns to U-mode; SPIE=1 → interrupts on after sret;
     * SUM=1 → kernel can access user pages (needed for copy_from_user). */
    *(uint32_t *)(f + CTX_SSTAT) = SSTATUS_SPIE | SSTATUS_SUM;
    /* CTX_PREVTOS = pointer to this frame itself (see pxPortInitialiseStack). */
    *(uint32_t *)(f + CTX_PREVTOS) = (uint32_t)pxTopOfStack;

    return pxTopOfStack;
}

/* Write the TCB's pxTopOfStack field (first word) for a task whose
 * initial frame was pre-built by prvInitialiseUserStack. */
void port_set_task_top_of_stack(TaskHandle_t h, StackType_t *sp)
{
    *(StackType_t **)h = sp;
}

/* ============================================================
 * port_set_kstack_top — update sscratch for the incoming task
 *
 * Called from vApplicationTaskSwitchedIn (mm.c) on every context
 * switch.  Full implementation lives in step 5 (sscratch swap /
 * U-mode trap entry).  Stub: kernel tasks keep sscratch = 0.
 * ============================================================ */

#define TLS_KSTACK_TOP  1

void port_set_task_kstack_top(void *task_handle, uintptr_t kstack_top)
{
    vTaskSetThreadLocalStoragePointer(
        (TaskHandle_t)task_handle, TLS_KSTACK_TOP, (void *)kstack_top);
}

void port_set_kstack_top(struct mm *mm)
{
    uintptr_t k = (uintptr_t)pvTaskGetThreadLocalStoragePointer(
                      NULL, TLS_KSTACK_TOP);
    /* Kernel-only task: no U-mode, keep sscratch = 0. */
    if (mm == NULL) k = 0;
    __asm__ volatile("csrw sscratch, %0" :: "r"(k) : "memory");
}
