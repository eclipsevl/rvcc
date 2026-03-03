#include "rvcc_target.h"

int counter = 0;
int total = 0;

static void increment(void) {
    counter = counter + 1;
}

static void add_to_total(int val) {
    total = total + val;
}

void main(void) {
    /* global variable modification */
    printdec(counter);
    putc(' ');

    increment();
    increment();
    increment();
    printdec(counter);
    putc(' ');

    /* accumulator pattern */
    for (int i = 1; i <= 4; i = i + 1) {
        add_to_total(i);
    }
    printdec(total);
    putc(' ');

    /* global array */
    int vals[] = {100, 200, 300};
    total = 0;
    for (int i = 0; i < 3; i = i + 1) {
        total = total + vals[i];
    }
    printdec(total);
    println("");
}
