/*
 * unistd.h — POSIX-системные вызовы из userspace.
 */
#ifndef _UNISTD_H
#define _UNISTD_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

ssize_t read(int fd, void* buf, size_t count);
ssize_t write(int fd, const void* buf, size_t count);
int     close(int fd);
off_t   lseek(int fd, off_t off, int whence);
pid_t   getpid(void);
pid_t   getppid(void);
pid_t   gettid(void);
uid_t   getuid(void);
uid_t   geteuid(void);
gid_t   getgid(void);
gid_t   getegid(void);
int     sched_yield(void);
void   _exit(int status) __attribute__((noreturn));

/* fork/exec/wait — критично для GCC (он fork'ает cpp/cc1/as/ld). */
pid_t   fork(void);
int     execve(const char* path, char* const argv[], char* const envp[]);
int     execv(const char* path, char* const argv[]);
int     execvp(const char* file, char* const argv[]);
int     execl(const char* path, const char* arg, ...);
int     execlp(const char* file, const char* arg, ...);

/* pipe — критично для shell-pipelines (cpp | cc1 | as | ld). */
int     pipe(int fds[2]);
int     dup(int fd);
int     dup2(int oldfd, int newfd);

void*   sbrk(long inc);

unsigned int sleep(unsigned int seconds);
int     usleep(unsigned int usec);

int     access(const char* path, int mode);
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

int     unlink(const char* path);
int     rmdir(const char* path);
int     chdir(const char* path);
char*   getcwd(char* buf, size_t size);

int     isatty(int fd);

#ifdef __cplusplus
}
#endif

#endif
