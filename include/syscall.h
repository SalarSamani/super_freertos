/* syscall.h — shared between kernel and user; user-side inline stubs. */
#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>
#include <stddef.h>

#define SYS_YIELD  0
#define SYS_PRINT  1
#define SYS_EXIT   2
#define SYS_SPAWN  3
#define SYS_WAIT   4

static inline int sys_yield(void)
{
    register int a0 asm("a0");
    register int a7 asm("a7") = SYS_YIELD;
    asm volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int sys_print(const char *buf, int len)
{
    register int a0 asm("a0") = (int)(uintptr_t)buf;
    register int a1 asm("a1") = len;
    register int a7 asm("a7") = SYS_PRINT;
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}

static inline int sys_exit(int code)
{
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

/* sys_spawn: pass the program name as a short NUL-terminated user-VA string.
 * Returns new pid (>=1) or -1 on error. */
static inline int sys_spawn(const char *name)
{
    register int a0 asm("a0") = (int)(uintptr_t)name;
    register int a7 asm("a7") = SYS_SPAWN;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

/* sys_wait: block until any child exits.  On return a0 = pid,
 * *status_out = exit code.  status_out may be NULL. */
static inline int sys_wait(int *status_out)
{
    register int a0 asm("a0") = (int)(uintptr_t)status_out;
    register int a7 asm("a7") = SYS_WAIT;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

#endif /* SYSCALL_H */
