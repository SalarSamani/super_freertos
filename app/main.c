/* ============================================================
 * app/main.c — Two demo tasks printing via simple_system UART.
 * ============================================================ */
#include "FreeRTOS.h"
#include "task.h"
#include "pmm.h"
#include "slab.h"
#include "mmu.h"
#include "mm.h"
#include "proc.h"
#include "spawn.h"
#include <stdint.h>

extern char _kernel_phys_end[];
extern char _user_init_lma[];
extern char _user_bad_lma[];
extern char _user_bad_size[];

/* Static memory for idle and timer tasks (required by configSUPPORT_STATIC_ALLOCATION). */
static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE];
void vApplicationGetIdleTaskMemory(StaticTask_t **ppTCB,
                                   StackType_t  **ppStack,
                                   configSTACK_DEPTH_TYPE *pSize)
{
    *ppTCB   = &s_idle_tcb;
    *ppStack = s_idle_stack;
    *pSize   = configMINIMAL_STACK_SIZE;
}

static StaticTask_t s_timer_tcb;
static StackType_t  s_timer_stack[configTIMER_TASK_STACK_DEPTH];
void vApplicationGetTimerTaskMemory(StaticTask_t **ppTCB,
                                    StackType_t  **ppStack,
                                    configSTACK_DEPTH_TYPE *pSize)
{
    *ppTCB   = &s_timer_tcb;
    *ppStack = s_timer_stack;
    *pSize   = configTIMER_TASK_STACK_DEPTH;
}

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
        vTaskDelay(pdMS_TO_TICKS(50));
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

    /* 1. Physical memory manager: owns all free RAM above the kernel image.
     *    Reserve both the kernel image AND the contiguous user-program
     *    blob (init|hello|bad) — otherwise PMM would hand out the pages
     *    that hold U-mode code and the first sret lands in a freshly-
     *    zeroed PT.  The blob runs from _user_init_lma through
     *    _user_bad_lma + _user_bad_size (see kernel.ld). */
    uintptr_t ublob_end = (uintptr_t)_user_bad_lma
                        + (uintptr_t)(size_t)_user_bad_size;
    const struct pmm_reserved resv[] = {
        { 0x00100000u, (uintptr_t)_kernel_phys_end },
        { (uintptr_t)_user_init_lma, ublob_end },
    };
    pmm_init(0x00100000u, 0x00200000u,
             resv, sizeof resv / sizeof resv[0]);

    /* 2. Slab allocator: carves small objects from PMM pages.
     *    pvPortMalloc/vPortFree are now backed by kmalloc/kfree. */
    slab_init();

    /* 3. Refine the kernel megapage into per-section 4 KiB PTEs.
     *    text=RX, rodata=R, data/bss=RW — any stray write to .text now faults. */
    mmu_refine_kernel_megapage();

    /* 4. Install direct physmap at VA 0xC0000000.
     *    After this, PA can be accessed at VA = PA + 0xC0000000.
     *    Switch slab helpers to physmap formula immediately after. */
    mmu_install_physmap(0x00100000u, 0x00200000u);
    slab_switch_to_physmap();

    /* 5. Record the kernel root PT physical address for mm_activate(NULL).
     *    g_kernel_root_pt[] is in .bss.pagetable, linked at high VMA. */
    g_kernel_root_pt_pa = (uintptr_t)g_kernel_root_pt - KERNEL_VA_BASE;

    /* 5a. Initialise the process table before any proc is allocated. */
    proc_table_init();

    xTaskCreate(vTaskA, "A", configMINIMAL_STACK_SIZE, NULL, 2, NULL);
    xTaskCreate(vTaskB, "B", configMINIMAL_STACK_SIZE, NULL, 1, NULL);

    /* 6. Spawn init (pid 1).  parent_pid=0 marks it as the root. */
    const struct user_prog_desc *init = find_user_prog("init");
    if (!init || spawn_user_prog(init, /*parent=*/0) < 0)
        puts_sim("[WARN] spawn init failed\n");

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