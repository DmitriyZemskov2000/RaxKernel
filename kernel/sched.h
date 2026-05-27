#ifndef WEBOS_SCHED_H
#define WEBOS_SCHED_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct task task_t;

void     sched_init(void);
task_t*  task_create(const char* name, void (*entry)(void*), void* arg);
task_t*  task_create_user(const char* name, u64 user_entry_rip, u64 user_rsp);
void     sched_yield(void);
void     sched_tick(void);     /* вызывается из IRQ0 */
void     sched_mark_current_dead(void);
u64      sched_current_kernel_stack(void);

const char* sched_current_name(void);
int         sched_current_id(void);
task_t*     sched_current_task(void);
void        task_exit(int code) __attribute__((noreturn));

/* Блокировка/разблокировка для futex и signal */
void        task_block(void);
void        task_unblock(task_t* t);

/* clone — новый thread в том же адресном пространстве */
task_t*     task_clone_user(const char* name, u64 child_entry, u64 child_stack, u64 arg);

/* fork — новая user task с копированием parent register snapshot. */
struct fork_regs {
    u64 rax, rbx, rcx, rdx, rsi, rdi, rbp;
    u64 r8, r9, r10, r11, r12, r13, r14, r15;
    u64 rip, rflags, rsp;
};
task_t*     task_fork_user(const char* name, struct fork_regs* regs);
void        task_set_cr3(task_t* t, u64 cr3);
void        task_set_process(task_t* t, void* p);
void*       task_get_process(task_t* t);

#ifdef __cplusplus
}
#endif

#endif
