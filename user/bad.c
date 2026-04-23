/* user/bad.c — U-mode program that deliberately faults.
 *
 * Not yet spawned at runtime (Step 4 wires SYS_SPAWN + do_page_fault);
 * present so the linker's .user_text.bad/.user_rodata.bad sections are
 * non-empty and g_progs["bad"] resolves to real LMA+size.
 *
 * Mode selection will become an argument once SYS_SPAWN lands; for now
 * the single entry just performs mode 1 (load from kernel VA). */
#include "syscall.h"

#define USEC    __attribute__((section(".user_text.bad"), used))
#define USEC_RO __attribute__((section(".user_rodata.bad"), used))

USEC_RO static const volatile unsigned bad_mode = 1;

USEC void _bad_entry(void)
{
    switch (bad_mode) {
    case 1: {
        volatile unsigned *p = (unsigned *)0x80000000u;
        volatile unsigned x = *p;       /* scause=13 expected (Step 4) */
        (void)x; break;
    }
    case 2: {
        volatile unsigned *p = (unsigned *)0x00400000u;
        *p = 0xDEADBEEFu;               /* scause=15 expected (Step 4) */
        break;
    }
    case 3: {
        void (*fn)(void) = (void (*)(void))0x00000000u;
        fn();                           /* scause=12 expected (Step 4) */
        break;
    }
    }
    sys_exit(0);
    for (;;) { }
}
