#include "rvcc_target.h"

static int add(int a, int b) {
    return a + b;
}

void main(void) {
    int x = add(3, 4);
    printdec(x);
    println("");
}
