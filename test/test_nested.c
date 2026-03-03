#include "rvcc_target.h"

void main(void) {
    /* nested loops */
    int sum = 0;
    for (int i = 0; i < 3; i = i + 1) {
        for (int j = 0; j < 3; j = j + 1) {
            sum = sum + 1;
        }
    }
    printdec(sum);     /* 9 */
    putc(' ');

    /* loop with inner conditional */
    int count = 0;
    for (int i = 0; i < 10; i = i + 1) {
        if (i % 2 == 0)
            count = count + 1;
    }
    printdec(count);   /* 5 (0,2,4,6,8) */
    putc(' ');

    /* while inside for */
    int result = 1;
    for (int i = 0; i < 3; i = i + 1) {
        int j = 0;
        while (j < 2) {
            result = result * 2;
            j = j + 1;
        }
    }
    printdec(result);  /* 2^6 = 64 */
    putc(' ');

    /* break out of nested loop */
    int found = 0;
    for (int i = 0; i < 5; i = i + 1) {
        if (i == 3) {
            found = i;
            break;
        }
    }
    printdec(found);   /* 3 */
    putc(' ');

    /* switch inside loop */
    int total = 0;
    for (int i = 0; i < 4; i = i + 1) {
        switch (i) {
        case 0: total += 1; break;
        case 1: total += 10; break;
        case 2: total += 100; break;
        default: total += 1000; break;
        }
    }
    printdec(total);   /* 1+10+100+1000 = 1111 */
    println("");
}
