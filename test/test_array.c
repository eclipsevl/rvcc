#include "rvcc_target.h"

void main(void) {
    uint8_t buf[5];
    buf[0] = 0xDE;
    buf[1] = 0xAD;
    printhex(buf[0]);
    putc(' ');
    printhex(buf[1]);
    println("");
}
