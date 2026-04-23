/* spawn.h — user program table + spawn entry point. */
#ifndef SPAWN_H
#define SPAWN_H

#include "FreeRTOS.h"
#include "task.h"
#include "proc.h"
#include <stdint.h>

struct user_prog_desc {
    const char *name;
    uintptr_t   vma;
    uintptr_t   lma;
    uint64_t    size;
    uintptr_t   entry;
};

const struct user_prog_desc *find_user_prog(const char *name);

/* Spawn a U-mode task running desc->entry, parented to parent_pid
 * (0 for the init task).  Returns the child's pid (>=1) or -1. */
int spawn_user_prog(const struct user_prog_desc *desc, pid_t parent_pid);

#endif /* SPAWN_H */
