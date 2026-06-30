#include "uvm32_target.h"
#include "../shared.h"

void main(void) {
    switch(syscall(SYSCALL_PICKTEST, 0, 0)) {
        case TEST1:
            asm("auipc t0, 0"); // copy pc into t0
            asm("auipc t1, 0"); // copy pc into t1
        break;
        case TEST2:
            asm("li  t0, 2");
            asm("li  t1, 1");
            label2:
            println("loop");
            asm goto("blt t0, t1, %l0" : : : : label2);
        break;

    }
}
