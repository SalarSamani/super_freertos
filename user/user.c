/* user/user.c — init (pid 1).
 *
 * Spawns two children in sequence and waits for each:
 *   1. "hello" — prints and exits cleanly with code 0.
 *   2. "bad"   — dereferences a kernel VA; segv_kill reaps it with
 *                exit code 139 (128 + SIGSEGV).
 *
 * All functions live in .user_text so the linker groups them at
 * _user_text_vma (0x00400000).  No globals, no libc, kernel reached
 * only via ecall (syscall.h stubs).  User space has no rodata mapped,
 * so every string — including program names — is built on the stack. */
#include "syscall.h"

#define USER_TEXT  __attribute__((section(".user_text.init"), used))

/* Tiny helpers — kept inline so they emit into .user_text.init. */
static inline void print_lit(const char *buf, int len) { sys_print(buf, len); }

/* Build "hello\0" on the stack, spawn + wait, report exit code. */
USER_TEXT void _user_entry(void)
{
    /* Banner: "[init] start\n" */
    {
        char m[13];
        m[0]='['; m[1]='i'; m[2]='n'; m[3]='i'; m[4]='t'; m[5]=']';
        m[6]=' '; m[7]='s'; m[8]='t'; m[9]='a'; m[10]='r'; m[11]='t';
        m[12]='\n';
        print_lit(m, 13);
    }

    /* --- Spawn "hello" --- */
    {
        char name[6];
        name[0]='h'; name[1]='e'; name[2]='l'; name[3]='l';
        name[4]='o'; name[5]=0;

        int pid = sys_spawn(name);
        int status = -1;
        int reaped = sys_wait(&status);
        (void)pid; (void)reaped;

        /* "[init] hello status=XXX\n"  — status rendered as 3 decimal digits. */
        char m[24];
        m[0]='['; m[1]='i'; m[2]='n'; m[3]='i'; m[4]='t'; m[5]=']';
        m[6]=' '; m[7]='h'; m[8]='e'; m[9]='l'; m[10]='l'; m[11]='o';
        m[12]=' '; m[13]='s'; m[14]='t'; m[15]='a'; m[16]='t';
        m[17]='=';
        int s = status < 0 ? 0 : status;
        m[18] = '0' + (s / 100) % 10;
        m[19] = '0' + (s / 10)  % 10;
        m[20] = '0' + (s       ) % 10;
        m[21] = '\n';
        print_lit(m, 22);
    }

    /* --- Spawn "bad" --- */
    {
        char name[4];
        name[0]='b'; name[1]='a'; name[2]='d'; name[3]=0;

        int pid = sys_spawn(name);
        int status = -1;
        int reaped = sys_wait(&status);
        (void)pid; (void)reaped;

        /* "[init] bad status=XXX\n" */
        char m[22];
        m[0]='['; m[1]='i'; m[2]='n'; m[3]='i'; m[4]='t'; m[5]=']';
        m[6]=' '; m[7]='b'; m[8]='a'; m[9]='d'; m[10]=' ';
        m[11]='s'; m[12]='t'; m[13]='a'; m[14]='t'; m[15]='=';
        int s = status < 0 ? 0 : status;
        m[16] = '0' + (s / 100) % 10;
        m[17] = '0' + (s / 10)  % 10;
        m[18] = '0' + (s       ) % 10;
        m[19] = '\n';
        print_lit(m, 20);
    }

    /* --- Spawn "hello" again — proves kernel survived the SEGV --- */
    {
        char name[6];
        name[0]='h'; name[1]='e'; name[2]='l'; name[3]='l';
        name[4]='o'; name[5]=0;

        int pid = sys_spawn(name);
        int status = -1;
        int reaped = sys_wait(&status);
        (void)pid; (void)reaped;

        /* "[init] hello2 stat=XXX\n" */
        char m[25];
        m[0]='['; m[1]='i'; m[2]='n'; m[3]='i'; m[4]='t'; m[5]=']';
        m[6]=' '; m[7]='h'; m[8]='e'; m[9]='l'; m[10]='l'; m[11]='o';
        m[12]='2'; m[13]=' '; m[14]='s'; m[15]='t'; m[16]='a'; m[17]='t';
        m[18]='=';
        int s = status < 0 ? 0 : status;
        m[19] = '0' + (s / 100) % 10;
        m[20] = '0' + (s / 10)  % 10;
        m[21] = '0' + (s       ) % 10;
        m[22] = '\n';
        print_lit(m, 23);
    }

    /* "[init] done\n" */
    {
        char m[12];
        m[0]='['; m[1]='i'; m[2]='n'; m[3]='i'; m[4]='t'; m[5]=']';
        m[6]=' '; m[7]='d'; m[8]='o'; m[9]='n'; m[10]='e';
        m[11]='\n';
        print_lit(m, 12);
    }

    sys_exit(0);
    for (;;) { }   /* unreachable */
}
