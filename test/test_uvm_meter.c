#include "uvm32_target.h"

void stall(void) {
    uint32_t i;
    for (i=0;i<10;i++) {
        yield();
    }
}

void main(void) {
    for (int i=0;i<10;i++) {
        //stall();
        printdec(i);
    }
}
