#include "rvcc_target.h"

void main(void) {
    /* int to char truncation */
    int big = 0x141;
    char c = (char)big;
    printdec(c);
    putc(' ');

    /* char to int (sign extension) */
    char neg = -5;
    int wide = (int)neg;
    printdec(wide);
    putc(' ');

    /* unsigned char stays positive */
    unsigned char uc = 200;
    int uwide = (int)uc;
    printdec(uwide);
    putc(' ');

    /* short truncation */
    int big2 = 0x10042;
    short s = (short)big2;
    printdec(s);
    putc(' ');

    /* pointer to int and back */
    int val = 77;
    int *p = &val;
    int addr = (int)p;
    int *p2 = (int *)addr;
    printdec(*p2);
    println("");
}
