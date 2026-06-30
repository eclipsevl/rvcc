#include "uvm32_target.h"

int foo(int a) {
    yield(0);
    printdec(a);
    return foo(a+1);
}

void main(void) {
    foo(1);
}
