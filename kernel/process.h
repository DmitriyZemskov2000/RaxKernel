#ifndef WEBOS_PROCESS_H
#define WEBOS_PROCESS_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PROC_RUNNING,
    PROC_ZOMBIE,    /* exited, ждёт wait() от parent */
} proc_state_t;

typedef struct {
    int          in_use;
    int          pid;
    int          parent_pid;
    proc_state_t state;
    int          exit_status;
    u64          cr3;             /* физ. адрес PML4 */
    u64          brk_start;
    u64          brk_current;
    u64          mmap_top;
} process_t;

process_t* process_create(int parent_pid);
process_t* process_find(int pid);
process_t* process_find_zombie_child(int parent_pid);
int        process_has_children(int parent_pid);
void       process_free(process_t* p);
void       process_set_zombie(process_t* p, int status);

int  process_fork_address_space(process_t* parent, process_t* child);

#ifdef __cplusplus
}
#endif

#endif
