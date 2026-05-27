#ifndef WEBOS_SYSCALL_H
#define WEBOS_SYSCALL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Номера syscalls — совпадают с Linux x86_64 ABI.
   Когда дойдём до портирования софта — не придётся ничего перекладывать. */
#define SYS_READ          0
#define SYS_WRITE         1
#define SYS_MMAP          9
#define SYS_MUNMAP       11
#define SYS_BRK          12
#define SYS_SCHED_YIELD  24
#define SYS_NANOSLEEP    35
#define SYS_GETPID       39
#define SYS_EXIT         60
#define SYS_CLOCK_GETTIME 228

void syscall_init(void);
void syscall_set_kernel_stack(u64 rsp);

long syscall_dispatch(long num, long a1, long a2, long a3,
                      long a4, long a5, long a6);

#ifdef __cplusplus
}
#endif

#endif
