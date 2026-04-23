/* heap_placement.c — Defines ucHeap in the named .heap4 section so the
 * linker script can expose _heap_start/_heap_end for the PMM to reserve. */
#include "FreeRTOS.h"
#include <stdint.h>

__attribute__((section(".heap4"), aligned(8)))
uint8_t ucHeap[configTOTAL_HEAP_SIZE];
