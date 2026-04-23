/* trap.c — S-mode trap dispatcher and syscall implementation. */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "portContext.h"
#include "syscall.h"
#include "pmm.h"
#include "mm.h"
#include "proc.h"
#include "spawn.h"
#include <string.h>
#include <stdint.h>

/* Frame word indices for argument registers. */
#define F_A(i)   ((CTX_X10 / 4) + (i))   /* a0..a7 for i=0..7 */
#define F_SEPC   (CTX_SEPC  / 4)
#define F_SSTAT  (CTX_SSTAT / 4)

/* Declared in port.c */
extern void portasm_handle_exception(uint32_t scause, uint32_t stval, uint32_t sepc);

/* -----------------------------------------------------------------------
 * copy_from_user — copy n bytes from user VA uva to kernel buffer kbuf.
 * Validates PTE_V | PTE_U | PTE_R for every page crossed.
 * ----------------------------------------------------------------------- */
static int copy_from_user(struct mm *mm, void *kbuf, uintptr_t uva, size_t n)
{
    uint8_t *dst = (uint8_t *)kbuf;
    while (n) {
        pte_t *slot = mm_walk(mm, uva & ~PAGE_MASK, /*alloc=*/0);
        if (!slot) return -1;
        pte_t pte = *slot;
        if (!(pte & PTE_V) || !(pte & PTE_U) || !(pte & PTE_R)) return -1;
        uintptr_t pa    = PTE_TO_PA(pte) | (uva & PAGE_MASK);
        size_t    chunk = PAGE_SIZE - (uva & PAGE_MASK);
        if (chunk > n) chunk = n;
        memcpy(dst, (void *)(pa + PHYSMAP_BASE), chunk);
        dst += chunk;
        uva += chunk;
        n   -= chunk;
    }
    return 0;
}

/* copy_from_user_str — copy a NUL-terminated user string of up to
 * max-1 bytes into kbuf.  kbuf is always NUL-terminated on success.
 * Returns strlen on success, -1 on fault or overflow. */
static int copy_from_user_str(struct mm *mm, char *kbuf, uintptr_t uva,
                              size_t max)
{
    if (max == 0) return -1;
    for (size_t i = 0; i < max - 1; i++) {
        char c;
        if (copy_from_user(mm, &c, uva + i, 1) < 0) return -1;
        kbuf[i] = c;
        if (c == '\0') return (int)i;
    }
    return -1;   /* not terminated within max-1 */
}

/* copy_to_user — mirror of copy_from_user.  Validates V|U|W on every page. */
static int copy_to_user(struct mm *mm, uintptr_t uva, const void *ksrc, size_t n)
{
    const uint8_t *src = (const uint8_t *)ksrc;
    while (n) {
        pte_t *slot = mm_walk(mm, uva & ~PAGE_MASK, /*alloc=*/0);
        if (!slot) return -1;
        pte_t pte = *slot;
        if (!(pte & PTE_V) || !(pte & PTE_U) || !(pte & PTE_W)) return -1;
        uintptr_t pa    = PTE_TO_PA(pte) | (uva & PAGE_MASK);
        size_t    chunk = PAGE_SIZE - (uva & PAGE_MASK);
        if (chunk > n) chunk = n;
        memcpy((void *)(pa + PHYSMAP_BASE), src, chunk);
        src += chunk;
        uva += chunk;
        n   -= chunk;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * sys_print_impl — write len bytes from user VA ubuf to the UART.
 * ----------------------------------------------------------------------- */
static int sys_print_impl(struct mm *mm, uintptr_t ubuf, int len)
{
    extern int putchar_sim(int c);
    if (len <= 0 || len > 4096) return -1;
    if (!mm) return -1;

    char kbuf[256];
    int  total = 0;

    while (total < len) {
        int n = len - total;
        if (n > (int)sizeof(kbuf)) n = (int)sizeof(kbuf);
        if (copy_from_user(mm, kbuf, ubuf + (uintptr_t)total, (size_t)n) < 0)
            return -1;
        for (int i = 0; i < n; i++)
            putchar_sim((unsigned char)kbuf[i]);
        total += n;
    }
    return total;
}

/* -----------------------------------------------------------------------
 * sys_exit_impl — mark caller as ZOMBIE, wake its parent, self-delete.
 * ----------------------------------------------------------------------- */
static void sys_exit_impl(int code)
{
    extern int putchar_sim(int c);
    const char *m = "[user] exit code=";
    while (*m) putchar_sim((unsigned char)*m++);

    /* Print decimal exit code (no div/mod — this core has no M ext). */
    int c2 = code;
    if (c2 < 0) { putchar_sim('-'); c2 = -c2; }
    char buf[12];
    int  i = 0;
    if (c2 == 0) {
        buf[i++] = '0';
    } else {
        while (c2) {
            int d = 0;
            while (c2 >= 10) { c2 -= 10; d++; }
            buf[i++] = '0' + c2;
            c2 = d;
        }
    }
    while (i > 0) putchar_sim((unsigned char)buf[--i]);
    putchar_sim('\n');

    struct proc *p = proc_current();
    if (p) {
        p->exit_code = code;
        p->state     = PROC_ZOMBIE;
        /* Wake the parent's wait channel.  The parent's wait_sem is
         * its own sleep channel; children signal it on exit. */
        struct proc *par = proc_by_pid(p->parent_pid);
        if (par && par->wait_sem)
            xSemaphoreGive(par->wait_sem);
    }
    vTaskDelete(NULL);
    /* Not reached — vTaskDelete triggers a context switch. */
}

/* -----------------------------------------------------------------------
 * sys_spawn_impl — look up a program by name, spawn it as a child.
 * ----------------------------------------------------------------------- */
static int sys_spawn_impl(struct mm *mm, uintptr_t uname)
{
    if (!mm) return -1;
    char name[16];
    if (copy_from_user_str(mm, name, uname, sizeof(name)) < 0) return -1;

    const struct user_prog_desc *desc = find_user_prog(name);
    if (!desc) return -1;

    struct proc *cur = proc_current();
    pid_t parent = cur ? cur->pid : 0;
    return spawn_user_prog(desc, parent);
}

/* -----------------------------------------------------------------------
 * sys_wait_impl — block until any child of the caller exits; reap it.
 * On success returns child pid, writes exit code to *ustatus (if set).
 * Returns -1 if the caller has no children.
 * ----------------------------------------------------------------------- */
static int sys_wait_impl(struct mm *mm, uintptr_t ustatus)
{
    extern int putchar_sim(int c);
    struct proc *cur = proc_current();
    if (!cur) { const char *m="[WAIT] !cur\n"; while(*m)putchar_sim(*m++); return -1; }

    for (;;) {
        int has = 0;
        struct proc *z = proc_first_zombie_child(cur->pid, &has);
        if (!has) return -1;
        if (z) {
            pid_t   cpid = z->pid;
            int32_t code = z->exit_code;

            if (z->mm) { mm_destroy(z->mm); z->mm = NULL; }
            /* Let idle drain xTasksWaitingTermination before we reuse this
             * slot's static TCB — otherwise xTaskCreateStatic overwrites
             * list pointers that idle later dereferences. */
            vTaskDelay(1);
            proc_free_slot(z);

            if (ustatus && mm)
                copy_to_user(mm, ustatus, &code, sizeof(code));
            return (int)cpid;
        }
        xSemaphoreTake(cur->wait_sem, portMAX_DELAY);
    }
}

/* -----------------------------------------------------------------------
 * syscall_handle — called from s_trap_dispatch for scause=8 (ecall U).
 * Advances sepc by 4 for all syscalls except SYS_EXIT.
 * ----------------------------------------------------------------------- */
static void syscall_handle(uint32_t *frame)
{
    struct mm *mm = mm_current();
    uint32_t   nr = frame[F_A(7)];
    uint32_t   a0 = frame[F_A(0)];
    uint32_t   a1 = frame[F_A(1)];
    int32_t    ret;

    switch (nr) {
    case SYS_YIELD:
        vTaskSwitchContext();
        ret = 0;
        break;
    case SYS_PRINT:
        ret = sys_print_impl(mm, (uintptr_t)a0, (int)a1);
        break;
    case SYS_EXIT:
        sys_exit_impl((int)a0);
        return;   /* not reached; no sepc advance needed */
    case SYS_SPAWN:
        ret = sys_spawn_impl(mm, (uintptr_t)a0);
        break;
    case SYS_WAIT:
        ret = sys_wait_impl(mm, (uintptr_t)a0);
        break;
    default:
        ret = -1;
        break;
    }

    frame[F_A(0)] = (uint32_t)ret;
    frame[F_SEPC] += 4;
}

/* -----------------------------------------------------------------------
 * fault_perm_ok — does vma.flags permit the access that faulted?
 * scause: 12 = instruction PF (needs X), 13 = load PF (needs R),
 *         15 = store/AMO PF (needs W).
 * ----------------------------------------------------------------------- */
static int fault_perm_ok(uint32_t scause, uint32_t vm_flags)
{
    switch (scause) {
    case 12: return (vm_flags & VM_X) ? 1 : 0;
    case 13: return (vm_flags & VM_R) ? 1 : 0;
    case 15: return (vm_flags & VM_W) ? 1 : 0;
    default: return 0;
    }
}

/* -----------------------------------------------------------------------
 * do_page_fault — service a U-mode page fault.
 * Returns 0 if the faulting instruction may be retried (sret without
 * advancing sepc), -1 if the access is illegal (caller routes to
 * segv_kill to terminate the offending task).
 * ----------------------------------------------------------------------- */
static int do_page_fault(uint32_t scause, uint32_t stval, uint32_t *frame)
{
    (void)frame;
    struct mm *mm = mm_current();
    if (!mm) return -1;

    struct vma *v = vma_find(mm, (uintptr_t)stval);
    if (!v) return -1;
    if (!fault_perm_ok(scause, v->flags)) return -1;
    if (!(v->flags & VM_ANON)) return -1;

    return mm_fill_anon_page(mm, (uintptr_t)stval,
                             v->flags & (VM_R|VM_W|VM_X|VM_U));
}

/* -----------------------------------------------------------------------
 * segv_kill — terminate the current U-mode task after an illegal fault.
 * Prints a single diagnostic line and deletes the FreeRTOS task so the
 * scheduler picks a different runnable one.  The proc slot's state is
 * advanced to ZOMBIE with a signal-style exit code so a future SYS_WAIT
 * (Step 4b) can reap it.
 * ----------------------------------------------------------------------- */
static void segv_puthex(uint32_t v)
{
    extern int putchar_sim(int c);
    const char *hex = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) putchar_sim(hex[(v >> i) & 0xF]);
}

static void segv_kill(uint32_t scause, uint32_t stval, uint32_t sepc)
{
    extern int putchar_sim(int c);
    struct proc *p = proc_current();

    const char *m = "[SEGV] pid=";
    while (*m) putchar_sim((unsigned char)*m++);
    segv_puthex(p ? (uint32_t)p->pid : 0xFFFFFFFFu);
    m = " scause=0x"; while (*m) putchar_sim((unsigned char)*m++);
    segv_puthex(scause);
    m = " stval=0x";  while (*m) putchar_sim((unsigned char)*m++);
    segv_puthex(stval);
    m = " sepc=0x";   while (*m) putchar_sim((unsigned char)*m++);
    segv_puthex(sepc);
    putchar_sim('\n');

    if (p) {
        /* Signal-style exit code: 128 + SIGSEGV(11) = 139. */
        p->exit_code = 139;
        p->state     = PROC_ZOMBIE;
        struct proc *par = proc_by_pid(p->parent_pid);
        if (par && par->wait_sem)
            xSemaphoreGive(par->wait_sem);
    }
    vTaskDelete(NULL);
    /* Not reached. */
}

/* -----------------------------------------------------------------------
 * s_trap_dispatch — entry point from portASM.S for all non-timer traps.
 * frame points at the saved context on the kernel stack.
 * ----------------------------------------------------------------------- */
void s_trap_dispatch(uint32_t *frame)
{
    uint32_t scause;
    __asm__ volatile("csrr %0, scause" : "=r"(scause));

    if (scause & 0x80000000u) {
        /* Unexpected interrupt (not STI — STI is handled inline in asm). */
        return;
    }

    switch (scause) {
    case 8:   /* ecall from U-mode */
        syscall_handle(frame);
        return;

    case 9:   /* ecall from S-mode (portYIELD / kernel yield) */
        frame[F_SEPC] += 4;
        vTaskSwitchContext();
        return;

    case 12:  /* instruction page fault */
    case 13:  /* load page fault */
    case 15:  /* store/AMO page fault */
    {
        uint32_t stval;
        __asm__ volatile("csrr %0, stval" : "=r"(stval));
        if (do_page_fault(scause, stval, frame) == 0)
            return;   /* sret retries faulting insn */
        /* Illegal U-mode access: terminate the offending task only.
         * The kernel itself keeps running — tasks A/B and the idle
         * task are unaffected. */
        segv_kill(scause, stval, frame[F_SEPC]);
        return;   /* vTaskDelete(NULL) context-switches away */
    }

    default:
        /* Unexpected synchronous exception: print and halt. */
        portasm_handle_exception(scause,
                                 ({ uint32_t v; __asm__("csrr %0,stval":"=r"(v)); v; }),
                                 frame[F_SEPC]);
        /* portasm_handle_exception never returns (halts). */
        break;
    }
}

