/*
 * signal.h — POSIX signal API.
 *
 * Текущая реализация: API-stub. Регистрируем хендлеры, но реальной
 * доставки нет (кроме kill(SIGKILL) → exit). Этого достаточно для
 * портируемого кода, который защищается от SIGPIPE через SIG_IGN
 * и не критически зависит от async-signal обработки.
 */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int sig_atomic_t;

typedef struct {
    unsigned long __val;
} sigset_t;

#define SIG_DFL ((void (*)(int))0)
#define SIG_IGN ((void (*)(int))1)
#define SIG_ERR ((void (*)(int))-1)

#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGSEGV 11
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGTRAP  5
#define SIGSYS  31
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SA_SIGINFO   0x00000004
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000

/* si_code значения для SIGFPE */
#define FPE_INTDIV   1
#define FPE_INTOVF   2
#define FPE_FLTDIV   3
#define FPE_FLTOVF   4
#define FPE_FLTUND   5
#define FPE_FLTRES   6
#define FPE_FLTINV   7
#define FPE_FLTSUB   8
/* si_code для SIGSEGV */
#define SEGV_MAPERR  1
#define SEGV_ACCERR  2
/* si_code для SIGBUS */
#define BUS_ADRALN   1
#define BUS_ADRERR   2
#define BUS_OBJERR   3

typedef struct {
    int      si_signo;
    int      si_errno;
    int      si_code;
    int      si_pid;
    int      si_uid;
    void*    si_addr;
    int      si_status;
    long     si_band;
} siginfo_t;

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t*, void*);
    };
    sigset_t sa_mask;
    int      sa_flags;
    void   (*sa_restorer)(void);
};

typedef void (*sighandler_t)(int);

int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact);
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset);
int sigemptyset(sigset_t* set);
int sigfillset(sigset_t* set);
int sigaddset(sigset_t* set, int sig);
int sigdelset(sigset_t* set, int sig);
int sigismember(const sigset_t* set, int sig);

sighandler_t signal(int signum, sighandler_t handler);

int kill(int pid, int sig);
int raise(int sig);

#ifdef __cplusplus
}
#endif

#endif
