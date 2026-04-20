/* ============================================================
 * app/main.c — Two demo tasks printing via simple_system UART.
 * ============================================================ */
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>

/* Low-level output. Since MMIO is identity-mapped, the same VA = PA
 * works whether we're running boot-low or kernel-high.                  */
int putchar_sim(int c)
{
    *(volatile uint32_t *)0x00020000 = (uint8_t)c;
    return c;
}
static void puts_sim(const char *s) { while (*s) putchar_sim(*s++); }

/* Two tasks, different priorities, same mechanism. */
static void vTaskA(void *pv)
{
    (void)pv;
    for (;;) {
        puts_sim("[A] hello from task A\n");
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void vTaskB(void *pv)
{
    (void)pv;
    for (;;) {
        puts_sim("[B] ...and task B is alive\n");
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

int main(void)
{
    puts_sim("==========================================\n");
    puts_sim("super_ibex FreeRTOS (S-mode + Sv32)\n");
    puts_sim("==========================================\n");

    xTaskCreate(vTaskA, "A", configMINIMAL_STACK_SIZE, NULL, 2, NULL);
    xTaskCreate(vTaskB, "B", configMINIMAL_STACK_SIZE, NULL, 1, NULL);

    vTaskStartScheduler();

    puts_sim("[FATAL] scheduler returned - should never happen\n");
    *(volatile unsigned *)0x20008 = 1;
    for (;;) ;
    return 0;
}

/* FreeRTOS hooks. */
void vApplicationMallocFailedHook(void) {
    puts_sim("[FATAL] malloc failed\n");
    *(volatile unsigned *)0x20008 = 1; for (;;) ;
}
void vApplicationStackOverflowHook(TaskHandle_t t, char *n) {
    (void)t; (void)n;
    puts_sim("[FATAL] stack overflow\n");
    *(volatile unsigned *)0x20008 = 1; for (;;) ;
}