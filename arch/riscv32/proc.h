/* ============================================================
 * proc.h — Unix-style process table on top of FreeRTOS TCBs.
 *
 * A FreeRTOS TaskHandle_t is not enough to be a process: we need
 * PIDs, parent tracking, exit codes, and a blocking channel so a
 * parent can sleep on wait().  struct proc adds that layer; the
 * TCB is linked to its proc via TLS slot PROC_TLS_SLOT.
 * ============================================================ */
#ifndef PROC_H
#define PROC_H

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "mm.h"

#define NPROC          8
#define PROC_TLS_SLOT  2    /* TLS slot 2: struct proc *   (0=mm, 1=kstack_top) */

typedef int pid_t;

enum proc_state { PROC_FREE = 0, PROC_RUNNING, PROC_ZOMBIE };

struct proc {
    pid_t              pid;
    pid_t              parent_pid;
    enum proc_state    state;
    struct mm         *mm;
    TaskHandle_t       task;
    void              *kstack;        /* malloc-owned; freed by reaper */
    int                exit_code;
    SemaphoreHandle_t  wait_sem;      /* parent's sleep channel */
    StaticSemaphore_t  wait_sem_buf;
    char               name[16];
};

void          proc_table_init(void);
struct proc  *proc_alloc(const char *name, pid_t parent);
void          proc_free_slot(struct proc *p);
struct proc  *proc_by_pid(pid_t pid);
struct proc  *proc_current(void);

/* Iterate children of parent_pid.  Returns the first zombie child if
 * any, else any running child, else NULL.  *has_children_out (if
 * non-NULL) is set to 1 when at least one non-free child exists. */
struct proc  *proc_first_zombie_child(pid_t parent_pid, int *has_children_out);

#endif /* PROC_H */
