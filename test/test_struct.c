#include "rvcc_target.h"

struct point {
    int x;
    int y;
};

static int sum_point(struct point *p) {
    return p->x + p->y;
}

void main(void) {
    /* struct on stack, init via array cast */
    int mem[] = {3, 4};
    struct point *p = (struct point *)mem;

    /* read via arrow */
    printdec(p->x);
    putc(' ');
    printdec(p->y);
    putc(' ');
    printdec(sum_point(p));
    putc(' ');

    /* second struct */
    int mem2[] = {10, 20};
    struct point *q = (struct point *)mem2;
    printdec(q->x + q->y);
    println("");
}
