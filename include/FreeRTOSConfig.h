/* ============================================================
 * FreeRTOSConfig.h — super_ibex + S-mode + Sv32.
 * ============================================================ */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* --------- CPU & tick ------------------------------------- */
#define configCPU_CLOCK_HZ                  50000000UL  /* 50 MHz sim */
#define configTICK_RATE_HZ                  ((TickType_t)100)
/* Used by the M-mode timer reload path in start.S.
 * Must equal configCPU_CLOCK_HZ / configTICK_RATE_HZ.       */
#define CONFIG_TIMER_RELOAD                 500000

/* --------- Scheduler -------------------------------------- */
#define configUSE_PREEMPTION                1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configUSE_IDLE_HOOK                 0
#define configUSE_TICK_HOOK                 0
#define configMAX_PRIORITIES                5
#define configMINIMAL_STACK_SIZE            ((uint16_t)256)  /* 1 KiB */
#define configMAX_TASK_NAME_LEN             16
#define configUSE_16_BIT_TICKS              0
#define configIDLE_SHOULD_YIELD             1
#define configUSE_TASK_NOTIFICATIONS        1
#define configUSE_MUTEXES                   1
#define configUSE_COUNTING_SEMAPHORES       1
#define configQUEUE_REGISTRY_SIZE           0
#define configUSE_QUEUE_SETS                0
#define configUSE_TIME_SLICING              1

/* --------- Memory ----------------------------------------- */
#define configSUPPORT_STATIC_ALLOCATION     0
#define configSUPPORT_DYNAMIC_ALLOCATION    1
#define configTOTAL_HEAP_SIZE               ((size_t)(64 * 1024))
#define configAPPLICATION_ALLOCATED_HEAP    0
#define configISR_STACK_SIZE_WORDS          2048             /* 8 KiB */

/* --------- Hook functions --------------------------------- */
#define configCHECK_FOR_STACK_OVERFLOW      2
#define configUSE_MALLOC_FAILED_HOOK        1
#define configUSE_DAEMON_TASK_STARTUP_HOOK  0

/* --------- Run-time stats & trace ------------------------- */
#define configGENERATE_RUN_TIME_STATS       0
#define configUSE_TRACE_FACILITY            0
#define configUSE_STATS_FORMATTING_FUNCTIONS 0

/* --------- Software timers -------------------------------- */
#define configUSE_TIMERS                    1
#define configTIMER_TASK_PRIORITY           (configMAX_PRIORITIES - 1)
#define configTIMER_QUEUE_LENGTH            8
#define configTIMER_TASK_STACK_DEPTH        (configMINIMAL_STACK_SIZE * 2)

/* --------- Optional API ----------------------------------- */
#define INCLUDE_vTaskPrioritySet            1
#define INCLUDE_uxTaskPriorityGet           1
#define INCLUDE_vTaskDelete                 1
#define INCLUDE_vTaskCleanUpResources       0
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_vTaskDelayUntil             1
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_xTaskGetSchedulerState      1
#define INCLUDE_xTaskGetCurrentTaskHandle   1

/* --------- Simple_system peripheral addresses ------------- */
#define configMTIME_BASE_ADDRESS            0x00030000UL
#define configMTIMECMP_BASE_ADDRESS         0x00030008UL
#define configSIM_CTRL_OUT_ADDRESS          0x00020000UL
#define configSIM_CTRL_HALT_ADDRESS         0x00020008UL

/* --------- Assert ----------------------------------------- */
#define configASSERT(x)  do { if (!(x)) { \
    *(volatile unsigned *)configSIM_CTRL_HALT_ADDRESS = 1; \
    for (;;) ; } } while (0)

#endif /* FREERTOS_CONFIG_H */