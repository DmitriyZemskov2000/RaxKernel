/*
 * signal.c — обёртки для signal API.
 *
 * Большинство — простые syscall'ы. Ядро в этой итерации stub'ает
 * rt_sigaction/rt_sigprocmask, но возвращает успех — достаточно
 * для портируемого кода.
 */

#include <signal.h>
#include <stddef.h>

extern long syscall1(long, long);
extern long syscall2(long, long, long);
extern long syscall4(long, long, long, long, long);

#define SYS_RT_SIGACTION    13
#define SYS_RT_SIGPROCMASK  14
#define SYS_KILL            62
#define SYS_GETPID          39

int kill(int pid, int sig) {
    return (int)syscall2(SYS_KILL, pid, sig);
}

int raise(int sig) {
    long pid = syscall1(SYS_GETPID, 0);
    return (int)syscall2(SYS_KILL, pid, sig);
}

int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    return (int)syscall4(SYS_RT_SIGACTION, sig, (long)act, (long)oldact, sizeof(sigset_t));
}

int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    return (int)syscall4(SYS_RT_SIGPROCMASK, how, (long)set, (long)oldset, sizeof(sigset_t));
}

int sigemptyset(sigset_t* set) { if (set) set->__val = 0; return 0; }
int sigfillset(sigset_t* set)  { if (set) set->__val = (unsigned long)-1; return 0; }
int sigaddset(sigset_t* set, int sig) {
    if (set && sig > 0 && sig < 64) set->__val |= (1UL << (sig - 1));
    return 0;
}
int sigdelset(sigset_t* set, int sig) {
    if (set && sig > 0 && sig < 64) set->__val &= ~(1UL << (sig - 1));
    return 0;
}
int sigismember(const sigset_t* set, int sig) {
    if (set && sig > 0 && sig < 64) return (set->__val >> (sig - 1)) & 1;
    return 0;
}

sighandler_t signal(int signum, sighandler_t handler) {
    /* Эмуляция через sigaction */
    struct sigaction sa = { handler, {0}, 0, NULL };
    struct sigaction old = {0};
    if (sigaction(signum, &sa, &old) < 0) return SIG_ERR;
    return old.sa_handler;
}
