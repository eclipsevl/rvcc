#include "rvcc_target.h"

static int square(int x) {
    return x * x;
}

static int abs_val(int x) {
    if (x < 0)
        return -x;
    return x;
}

static int max(int a, int b) {
    if (a > b)
        return a;
    return b;
}

static int min(int a, int b) {
    if (a < b)
        return a;
    return b;
}

static int clamp(int val, int lo, int hi) {
    return max(lo, min(val, hi));
}

static int sum_array(int *arr, int len) {
    int total = 0;
    for (int i = 0; i < len; i = i + 1) {
        total = total + arr[i];
    }
    return total;
}

void main(void) {
    printdec(square(7));          /* 49 */
    putc(' ');
    printdec(abs_val(-15));       /* 15 */
    putc(' ');
    printdec(max(3, 9));          /* 9 */
    putc(' ');
    printdec(min(3, 9));          /* 3 */
    putc(' ');
    printdec(clamp(50, 0, 10));   /* 10 */
    putc(' ');
    printdec(clamp(-5, 0, 10));   /* 0 */
    putc(' ');

    int data[] = {1, 2, 3, 4, 5};
    printdec(sum_array(data, 5)); /* 15 */
    println("");
}
