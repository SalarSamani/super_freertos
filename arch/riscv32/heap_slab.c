/* super_freertos/arch/riscv32/heap_slab.c
 * FreeRTOS MemMang shim: routes pvPortMalloc/vPortFree through the slab. */
#include "FreeRTOS.h"
#include "task.h"
#include "slab.h"
#include "pmm.h"

void *pvPortMalloc(size_t xWantedSize)
{
    void *p;
    vTaskSuspendAll();
    p = kmalloc(xWantedSize);
    (void)xTaskResumeAll();

    traceMALLOC(p, xWantedSize);
#if (configUSE_MALLOC_FAILED_HOOK == 1)
    if (!p) {
        extern void vApplicationMallocFailedHook(void);
        vApplicationMallocFailedHook();
    }
#endif
    return p;
}

void vPortFree(void *pv)
{
    if (!pv) return;
    vTaskSuspendAll();
    kfree(pv);
    (void)xTaskResumeAll();
    traceFREE(pv, 0);
}

size_t xPortGetFreeHeapSize(void)
{
    return pmm_free_pages_count() * PAGE_SIZE;
}

size_t xPortGetMinimumEverFreeHeapSize(void) { return 0; }
void   vPortInitialiseBlocks(void)            { }
