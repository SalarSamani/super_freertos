/* user/hello.c — minimal U-mode program that prints and exits.
 *
 * Not yet spawned at runtime (Step 4 wires SYS_SPAWN); present so
 * the linker's .user_text.hello/.user_rodata.hello sections are
 * non-empty and g_progs["hello"] resolves to real LMA+size. */
#include "syscall.h"

#define USEC    __attribute__((section(".user_text.hello"), used))
#define USEC_RO __attribute__((section(".user_rodata.hello"), used))

USEC void _hello_entry(void)
{
    char msg[18];
    msg[0]='h'; msg[1]='e'; msg[2]='l'; msg[3]='l'; msg[4]='o';
    msg[5]=':'; msg[6]=' '; msg[7]='f'; msg[8]='r'; msg[9]='o';
    msg[10]='m'; msg[11]=' '; msg[12]='h'; msg[13]='e'; msg[14]='l';
    msg[15]='l'; msg[16]='o'; msg[17]='\n';

    sys_print(msg, 18);
    sys_exit(0);
    for (;;) { }
}
