/*
 * process.c — управление процессами (отдельно от thread'ов).
 *
 * До итерации 8 у нас были только task_t (threads), все в одном
 * глобальном address space. Теперь:
 *   - process_t хранит CR3 (свой PML4), FD-таблицу, parent_pid,
 *     status code для wait()
 *   - task_t (thread) принадлежит одному process_t
 *   - fork() копирует процесс + создаёт новый task в новом адресном
 *     пространстве
 *   - execve() заменяет память текущего процесса новой программой
 *
 * Файловые дескрипторы пока остаются глобальными (как раньше) —
 * per-process FD-таблица появится когда у нас будет настоящий
 * pipe()-механизм.
 */

#include "types.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "process.h"
#include "pmm.h"
#include "vmm.h"
#include "sched.h"

#define MAX_PROCESSES 32

static process_t processes[MAX_PROCESSES];
static int next_pid = 1;

process_t* process_create(int parent_pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (!processes[i].in_use) {
            process_t* p = &processes[i];
            memset(p, 0, sizeof(*p));
            p->in_use = 1;
            p->pid = next_pid++;
            p->parent_pid = parent_pid;
            p->exit_status = -1;
            p->state = PROC_RUNNING;
            return p;
        }
    }
    return NULL;
}

process_t* process_find(int pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].in_use && processes[i].pid == pid) {
            return &processes[i];
        }
    }
    return NULL;
}

process_t* process_find_zombie_child(int parent_pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].in_use &&
            processes[i].parent_pid == parent_pid &&
            processes[i].state == PROC_ZOMBIE) {
            return &processes[i];
        }
    }
    return NULL;
}

int process_has_children(int parent_pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].in_use &&
            processes[i].parent_pid == parent_pid) {
            return 1;
        }
    }
    return 0;
}

void process_free(process_t* p) {
    if (!p) return;
    p->in_use = 0;
}

void process_set_zombie(process_t* p, int status) {
    if (!p) return;
    p->exit_status = status;
    p->state = PROC_ZOMBIE;
}

/*
 * Копирование адресного пространства (для fork).
 *
 * Linux делает COW (copy-on-write). У нас — простое полное копирование
 * страниц в новый PML4. Это дороже, но проще и корректно.
 *
 * Что копируем: все страницы пользовательского диапазона (0..256 GiB).
 * Higher-half (kernel) у нас замаплен в каждом процессе — это делает
 * vmm_init для самого первого PML4. При создании нового PML4 нужно
 * скопировать kernel-mapping тоже.
 */

extern u64 vmm_clone_address_space(void);   /* реализуем в vmm.c */

int process_fork_address_space(process_t* parent, process_t* child) {
    (void)parent;
    u64 new_cr3 = vmm_clone_address_space();
    if (!new_cr3) return -1;
    child->cr3 = new_cr3;
    return 0;
}
