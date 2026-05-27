/*
 * unistd.c — POSIX-обёртки для userspace.
 */

#include <unistd.h>
#include <stdarg.h>
#include <stddef.h>

extern long syscall0(long num);
extern long syscall1(long num, long a1);
extern long syscall2(long num, long a1, long a2);
extern long syscall3(long num, long a1, long a2, long a3);

#define SYS_READ           0
#define SYS_WRITE          1
#define SYS_OPEN           2
#define SYS_CLOSE          3
#define SYS_LSEEK          8
#define SYS_BRK            12
#define SYS_NANOSLEEP      35
#define SYS_YIELD          24
#define SYS_GETPID         39
#define SYS_EXIT           60

struct timespec_user { long tv_sec; long tv_nsec; };

ssize_t write(int fd, const void* buf, size_t count) {
    return (ssize_t)syscall3(SYS_WRITE, fd, (long)buf, (long)count);
}

ssize_t read(int fd, void* buf, size_t count) {
    return (ssize_t)syscall3(SYS_READ, fd, (long)buf, (long)count);
}

/* open принимает varargs (3-й аргумент mode иногда не нужен) —
   реализуем как фиксированный 3-argument, mode по умолчанию 0. */
int open(const char* path, int flags, ...) {
    return (int)syscall3(SYS_OPEN, (long)path, flags, 0);
}

int close(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

long lseek(int fd, long off, int whence) {
    return syscall3(SYS_LSEEK, fd, off, whence);
}

pid_t getpid(void) {
    return (pid_t)syscall0(SYS_GETPID);
}

int sched_yield(void) {
    return (int)syscall0(SYS_YIELD);
}

void _exit(int status) {
    syscall1(SYS_EXIT, status);
    for (;;) {}
}

void* sbrk(long inc) {
    long cur = syscall1(SYS_BRK, 0);
    if (cur < 0) return (void*)-1;
    long want = cur + inc;
    long got = syscall1(SYS_BRK, want);
    if (got != want) return (void*)-1;
    return (void*)cur;
}

unsigned int sleep(unsigned int seconds) {
    struct timespec_user req = { (long)seconds, 0 };
    syscall2(SYS_NANOSLEEP, (long)&req, 0);
    return 0;
}

int usleep(unsigned int usec) {
    struct timespec_user req = { (long)(usec / 1000000),
                                  (long)((usec % 1000000) * 1000) };
    syscall2(SYS_NANOSLEEP, (long)&req, 0);
    return 0;
}

#define SYS_FORK     57
#define SYS_EXECVE   59
#define SYS_WAIT4    61
#define SYS_PIPE     22
#define SYS_DUP      32
#define SYS_DUP2     33
#define SYS_GETUID  102
#define SYS_GETGID  104
#define SYS_GETPPID 110
#define SYS_ACCESS   21
#define SYS_UNLINK   87
#define SYS_RMDIR    84
#define SYS_CHDIR    80
#define SYS_GETCWD   79
#define SYS_ISATTY  213   /* нестандартный — добавим в ядро */
#define SYS_GETTID  186

/* ----- fork / wait / exec ----- */

pid_t fork(void) { return (pid_t)syscall1(SYS_FORK, 0); }

int execve(const char* path, char* const argv[], char* const envp[]) {
    return (int)syscall3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
}

int execv(const char* path, char* const argv[]) {
    extern char** environ;
    return execve(path, argv, environ);
}

int execvp(const char* file, char* const argv[]) {
    /* TODO: реальный PATH lookup. Пока — execve напрямую. */
    return execv(file, argv);
}

int execl(const char* path, const char* arg, ...) {
    /* Простейшая реализация: соберём argv из varargs. */
    char* argv[16];
    int n = 0;
    argv[n++] = (char*)arg;
    va_list ap;
    va_start(ap, arg);
    while (n < 15) {
        char* a = va_arg(ap, char*);
        argv[n++] = a;
        if (!a) break;
    }
    va_end(ap);
    return execv(path, argv);
}

int execlp(const char* file, const char* arg, ...) {
    char* argv[16];
    int n = 0;
    argv[n++] = (char*)arg;
    va_list ap;
    va_start(ap, arg);
    while (n < 15) {
        char* a = va_arg(ap, char*);
        argv[n++] = a;
        if (!a) break;
    }
    va_end(ap);
    return execvp(file, argv);
}

/* ----- pipe / dup ----- */

int pipe(int fds[2])   { return (int)syscall1(SYS_PIPE, (long)fds); }
int dup(int fd)        { return (int)syscall1(SYS_DUP, fd); }
int dup2(int o, int n) { return (int)syscall2(SYS_DUP2, o, n); }

/* ----- ids ----- */

pid_t getppid(void) { return (pid_t)syscall0(SYS_GETPPID); }
pid_t gettid(void)  { return (pid_t)syscall0(SYS_GETTID); }
uid_t getuid(void)  { return (uid_t)syscall0(SYS_GETUID); }
uid_t geteuid(void) { return (uid_t)syscall0(SYS_GETUID); }
gid_t getgid(void)  { return (gid_t)syscall0(SYS_GETGID); }
gid_t getegid(void) { return (gid_t)syscall0(SYS_GETGID); }

/* ----- fs misc ----- */

int access(const char* path, int mode) {
    return (int)syscall2(SYS_ACCESS, (long)path, mode);
}
int unlink(const char* path) {
    return (int)syscall1(SYS_UNLINK, (long)path);
}
int rmdir(const char* path) {
    return (int)syscall1(SYS_RMDIR, (long)path);
}
int chdir(const char* path) {
    return (int)syscall1(SYS_CHDIR, (long)path);
}
char* getcwd(char* buf, size_t size) {
    long r = syscall2(SYS_GETCWD, (long)buf, (long)size);
    return (r < 0) ? NULL : buf;
}

int isatty(int fd) {
    /* fd 0/1/2 → /dev/console это tty; всё остальное — нет. */
    if (fd >= 0 && fd <= 2) return 1;
    return 0;
}

/* wait/waitpid */
pid_t wait(int* status) {
    return (pid_t)syscall4(SYS_WAIT4, -1, (long)status, 0, 0);
}
pid_t waitpid(pid_t pid, int* status, int options) {
    return (pid_t)syscall4(SYS_WAIT4, pid, (long)status, options, 0);
}

#define SYS_MMAP      9
#define SYS_MUNMAP    11
#define SYS_MPROTECT  10

void* mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
    long r = syscall6(SYS_MMAP, (long)addr, (long)len, prot, flags, fd, (long)off);
    return (void*)r;
}

int munmap(void* addr, size_t len) {
    return (int)syscall2(SYS_MUNMAP, (long)addr, (long)len);
}

int mprotect(void* addr, size_t len, int prot) {
    return (int)syscall3(SYS_MPROTECT, (long)addr, (long)len, prot);
}

/* gettimeofday через clock_gettime (SYS_clock_gettime=228) */
struct __timeval { long tv_sec; long tv_usec; };
int gettimeofday(void* tv, void* tz) {
    (void)tz;
    if (!tv) return 0;
    struct { long sec; long nsec; } ts = {0,0};
    syscall2(228, 0 /*CLOCK_REALTIME*/, (long)&ts);
    struct __timeval* t = (struct __timeval*)tv;
    t->tv_sec = ts.sec;
    t->tv_usec = ts.nsec / 1000;
    return 0;
}

/* stat: syscall 4 заполняет промежуточный {size,type}, мы конвертируем
   в POSIX struct stat. type: 1=file(VNODE_FILE), 2=dir(VNODE_DIR). */
int stat(const char* path, void* statbuf) {
    struct { unsigned long size; int type; int _pad; } tmp = {0,0,0};
    long r = syscall2(4 /*SYS_stat*/, (long)path, (long)&tmp);
    if (r < 0) return -1;
    /* struct stat: st_mode на offset 16, st_size на offset 48 (POSIX layout) */
    unsigned char* sb = (unsigned char*)statbuf;
    /* Обнуляем всю структуру (144 байта с запасом) */
    for (int i = 0; i < 144; i++) sb[i] = 0;
    /* st_mode (mode_t, offset 16 в нашем layout: dev,ino=16 байт перед) */
    unsigned int mode = (tmp.type == 2) ? 0040755 : 0100644;  /* dir или reg */
    *(unsigned int*)(sb + 16) = mode;     /* st_mode */
    *(long*)(sb + 48) = (long)tmp.size;   /* st_size */
    return 0;
}

int fstat(int fd, void* statbuf) {
    (void)fd;
    /* Упрощённо: помечаем как regular file неизвестного размера */
    unsigned char* sb = (unsigned char*)statbuf;
    for (int i = 0; i < 144; i++) sb[i] = 0;
    *(unsigned int*)(sb + 16) = 0100644;
    return 0;
}

int lstat(const char* path, void* statbuf) { return stat(path, statbuf); }
