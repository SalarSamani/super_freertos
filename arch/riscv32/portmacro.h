/* ============================================================
 * portmacro.h — FreeRTOS port macros for S-mode RV32.
 *
 * NOTE: These are deliberately distinct from the official
 * FreeRTOS RISC-V port, which is M-mode only. The official
 * port uses mstatus/mepc/mret; we mirror that layout under
 * sstatus/sepc/sret.
 * ============================================================ */
#ifndef PORTMACRO_H
#define PORTMACRO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Data types ------------------------------------------- */
#define portCHAR        char
#define portFLOAT       float
#define portDOUBLE      double
#define portLONG        long
#define portSHORT       short
#define portSTACK_TYPE  uint32_t
#define portBASE_TYPE   long
#define portPOINTER_SIZE_TYPE  uint32_t

typedef portSTACK_TYPE StackType_t;
typedef long           BaseType_t;
typedef unsigned long  UBaseType_t;
typedef uint32_t       TickType_t;
#define portMAX_DELAY  ((TickType_t)0xffffffffUL)
#define portTICK_TYPE_IS_ATOMIC  1

/* ---- Architecture details --------------------------------- */
#define portSTACK_GROWTH        (-1)
#define portTICK_PERIOD_MS      ((TickType_t)1000 / configTICK_RATE_HZ)
#define portBYTE_ALIGNMENT      16          /* RV32 ABI requires 16-byte SP */
#define portCRITICAL_NESTING_IN_TCB  1      /* we track nesting in TCB */

/* ---- Interrupt control via sstatus.SIE (bit 1) ------------ */
#define portSSTATUS_SIE   (1u << 1)

static inline void vPortDisableInterrupts(void)
{
    __asm volatile ("csrc sstatus, %0" :: "r"(portSSTATUS_SIE) : "memory");
}
static inline void vPortEnableInterrupts(void)
{
    __asm volatile ("csrs sstatus, %0" :: "r"(portSSTATUS_SIE) : "memory");
}
static inline uint32_t vPortReadAndDisable(void)
{
    uint32_t old;
    __asm volatile ("csrrc %0, sstatus, %1"
                    : "=r"(old) : "r"(portSSTATUS_SIE) : "memory");
    return old & portSSTATUS_SIE;
}
static inline void vPortRestoreInterrupts(uint32_t old_sie)
{
    if (old_sie) {
        __asm volatile ("csrs sstatus, %0" :: "r"(portSSTATUS_SIE) : "memory");
    }
}

#define portDISABLE_INTERRUPTS()   vPortDisableInterrupts()
#define portENABLE_INTERRUPTS()    vPortEnableInterrupts()

/* Critical-section API used by FreeRTOS core. We count nesting in
 * xCriticalNesting, updated by the macros below.                        */
extern volatile UBaseType_t uxCriticalNesting;

#define portENTER_CRITICAL()   do {                     \
    vPortDisableInterrupts();                           \
    uxCriticalNesting++;                                \
} while (0)

#define portEXIT_CRITICAL()    do {                     \
    if (--uxCriticalNesting == 0) {                     \
        vPortEnableInterrupts();                        \
    }                                                    \
} while (0)

/* ---- Yield --------------------------------------------------
 * FreeRTOS yields via ecall from S-mode. scause will read 9
 * ("environment call from S-mode") and the trap handler will
 * invoke vTaskSwitchContext.                                    */
#define portYIELD()             __asm volatile ("ecall")
#define portEND_SWITCHING_ISR(x) if (x) portYIELD()
#define portYIELD_FROM_ISR(x)   portEND_SWITCHING_ISR(x)

/* ---- Task function prototype ------------------------------ */
#define portTASK_FUNCTION_PROTO(fn, param)  void fn(void *param)
#define portTASK_FUNCTION(fn, param)        void fn(void *param)

/* ---- Clear ISR-level mask for FreeRTOS API checks --------- */
#define portNOP()               __asm volatile ("nop")

#ifdef __cplusplus
}
#endif
#endif /* PORTMACRO_H */