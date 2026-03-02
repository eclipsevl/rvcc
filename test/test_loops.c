#include "rvcc_target.h"

void main(void) {
    /* while loop */
    int i = 0;
    int sum = 0;
    while (i < 5) {
        sum = sum + i;
        i = i + 1;
    }
    printdec(sum);     /* 0+1+2+3+4 = 10 */
    putc(' ');

    /* for loop */
    sum = 0;
    for (int j = 1; j <= 4; j = j + 1) {
        sum = sum + j;
    }
    printdec(sum);     /* 1+2+3+4 = 10 */
    putc(' ');

    /* break */
    sum = 0;
    for (int k = 0; k < 100; k = k + 1) {
        if (k == 5)
            break;
        sum = sum + 1;
    }
    printdec(sum);     /* 5 iterations: 0,1,2,3,4 */
    putc(' ');

    /* continue */
    sum = 0;
    for (int m = 0; m < 6; m = m + 1) {
        if (m == 3)
            continue;
        sum = sum + 1;
    }
    printdec(sum);     /* 6 iterations minus skip at 3 = 5 */
    println("");
}
