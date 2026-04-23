/* ============================================================
 * proc.c — Process table.
 *
 * PID 1 is reserved for init and is produced by allocating slot 0
 * when parent==0; every other allocation takes slots 1..NPROC-1
 * and gets pid = slot + 1.  This matches the xv6 convention where
 * init is always pid 1 regardless of allocation order.
 * ============================================================ */
#include "proc.h"
#include <string.h>

static struct proc g_proc[NPROC];

void proc_table_init(void)
{
    memset(g_proc, 0, sizeof(g_proc));
    for (int i = 0; i < NPROC; i++) g_proc[i].state = PROC_FREE;
}

struct proc *proc_alloc(const char *name, pid_t parent)
{
    /* Slot 0 is reserved for the first-ever allocation (parent==0 → init). */
    int start = (parent == 0) ? 0 : 1;
    for (int i = start; i < NPROC; i++) {
        if (g_proc[i].state == PROC_FREE) {
            struct proc *p = &g_proc[i];
            memset(p, 0, sizeof(*p));
            p->pid        = (i == 0) ? 1 : (i + 1);
            p->parent_pid = parent;
            p->state      = PROC_RUNNING;
            p->wait_sem   = xSemaphoreCreateBinaryStatic(&p->wait_sem_buf);
            if (name) {
                size_t n = 0;
                while (n < sizeof(p->name) - 1 && name[n]) {
                    p->name[n] = name[n];
                    n++;
                }
                p->name[n] = 0;
            }
            return p;
        }
    }
    return NULL;
}

void proc_free_slot(struct proc *p)
{
    if (!p) return;
    p->state  = PROC_FREE;
    p->mm     = NULL;
    p->task   = NULL;
    p->kstack = NULL;
}

struct proc *proc_by_pid(pid_t pid)
{
    for (int i = 0; i < NPROC; i++)
        if (g_proc[i].state != PROC_FREE && g_proc[i].pid == pid)
            return &g_proc[i];
    return NULL;
}

struct proc *proc_current(void)
{
    TaskHandle_t t = xTaskGetCurrentTaskHandle();
    if (!t) return NULL;
    return (struct proc *)pvTaskGetThreadLocalStoragePointer(t, PROC_TLS_SLOT);
}

struct proc *proc_first_zombie_child(pid_t parent_pid, int *has_children_out)
{
    int has = 0;
    for (int i = 0; i < NPROC; i++) {
        struct proc *p = &g_proc[i];
        if (p->state == PROC_FREE) continue;
        if (p->parent_pid != parent_pid) continue;
        has = 1;
        if (p->state == PROC_ZOMBIE) {
            if (has_children_out) *has_children_out = 1;
            return p;
        }
    }
    if (has_children_out) *has_children_out = has;
    return NULL;
}
