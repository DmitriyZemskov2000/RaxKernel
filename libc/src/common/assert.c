#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void __assert_fail(const char* expr, const char* file, int line, const char* func) {
    printf("\n*** ASSERTION FAILED ***\n");
    printf("    Expression: %s\n", expr);
    printf("    Location:   %s:%d (%s)\n", file, line, func);
    abort();
}
