#include "rvcc_target.h"

static int factorial(int n) {
    if (n <= 1)
        return 1;
    return n * factorial(n - 1);
}

static int fib(int n) {
    if (n <= 0) return 0;
    if (n == 1) return 1;
    return fib(n - 1) + fib(n - 2);
}

static int sum_to(int n) {
    if (n == 0)
        return 0;
    return n + sum_to(n - 1);
}

void main(void) {
    printdec(factorial(5));   /* 120 */
    putc(' ');
    printdec(fib(7));         /* 13 */
    putc(' ');
    printdec(sum_to(10));     /* 55 */
    println("");
}
