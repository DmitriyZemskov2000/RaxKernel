#ifndef _SYS_UCONTEXT_H
#define _SYS_UCONTEXT_H

#include <sys/types.h>

/* Минимальная заглушка ucontext для tccrun (backtrace при -run).
   Полноценный signal-based backtrace не поддержан — поля присутствуют
   чтобы tccrun.c скомпилировался. */

typedef struct {
    unsigned long gregs[23];
} mcontext_t;

/* Индексы регистров (x86_64), как в glibc */
#define REG_R8   0
#define REG_R9   1
#define REG_R10  2
#define REG_R11  3
#define REG_R12  4
#define REG_R13  5
#define REG_R14  6
#define REG_R15  7
#define REG_RDI  8
#define REG_RSI  9
#define REG_RBP  10
#define REG_RBX  11
#define REG_RDX  12
#define REG_RAX  13
#define REG_RCX  14
#define REG_RSP  15
#define REG_RIP  16

typedef struct ucontext_t {
    unsigned long      uc_flags;
    struct ucontext_t* uc_link;
    struct {
        void*         ss_sp;
        int           ss_flags;
        size_t        ss_size;
    } uc_stack;
    mcontext_t         uc_mcontext;
    unsigned long      uc_sigmask;
} ucontext_t;

#endif
