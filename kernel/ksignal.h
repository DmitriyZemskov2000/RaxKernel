#ifndef WEBOS_SIGNAL_H
#define WEBOS_SIGNAL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

long sys_kill(int pid, int sig);
long sys_rt_sigaction(int sig, const void* act, void* oldact, size_t sigsetsize);
long sys_rt_sigprocmask(int how, const void* set, void* oldset, size_t sigsetsize);

#ifdef __cplusplus
}
#endif

#endif
