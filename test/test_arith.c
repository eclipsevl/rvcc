#include "rvcc_target.h"

void main(void) {
    /* multiply */
    printdec(7 * 8);
    putc(' ');

    /* divide */
    printdec(100 / 7);
    putc(' ');

    /* modulo */
    printdec(17 % 5);
    putc(' ');

    /* bitwise AND */
    printdec(0xFF & 0x0F);
    putc(' ');

    /* bitwise OR */
    printdec(0xA0 | 0x05);
    putc(' ');

    /* bitwise XOR */
    printdec(0xFF ^ 0x0F);
    putc(' ');

    /* left shift */
    printdec(1 << 4);
    putc(' ');

    /* right shift */
    printdec(256 >> 3);
    putc(' ');

    /* bitwise NOT (lower 8 bits) */
    printdec(~0 & 0xFF);
    putc(' ');

    /* negate */
    int x = 42;
    printdec(-x);
    println("");
}
