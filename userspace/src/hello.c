/* RaxOS init — первая userspace-программа. */
#include <stdio.h>
#include <unistd.h>
int main(void) {
    printf("\n");
    printf("=====================================\n");
    printf(" RaxOS — hobby kernel x86_64\n");
    printf("=====================================\n");
    printf("kernel: alive\n");
    printf("init  : pid=%d\n", (int)getpid());
    printf("\n");
    printf("System ready. Idle loop.\n");
    for (;;) {
        for (volatile long i = 0; i < 200000000; i++) ;
    }
    return 0;
}
