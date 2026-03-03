#include "rvcc_target.h"

void main(void) {
    /* compound assignment operators */
    int x = 10;
    x += 5;
    printdec(x);    /* 15 */
    putc(' ');

    x -= 3;
    printdec(x);    /* 12 */
    putc(' ');

    x *= 2;
    printdec(x);    /* 24 */
    putc(' ');

    x /= 6;
    printdec(x);    /* 4 */
    putc(' ');

    x %= 3;
    printdec(x);    /* 1 */
    putc(' ');

    /* bitwise compound */
    int y = 0xFF;
    y &= 0x0F;
    printdec(y);    /* 15 */
    putc(' ');

    y |= 0x30;
    printdec(y);    /* 63 */
    putc(' ');

    y ^= 0x0F;
    printdec(y);    /* 48 */
    putc(' ');

    /* shift compound */
    int z = 1;
    z <<= 5;
    printdec(z);    /* 32 */
    putc(' ');

    z >>= 2;
    printdec(z);    /* 8 */
    putc(' ');

    /* pre-increment / pre-decrement */
    int a = 10;
    printdec(++a);  /* 11 */
    putc(' ');
    printdec(--a);  /* 10 */
    println("");
}
