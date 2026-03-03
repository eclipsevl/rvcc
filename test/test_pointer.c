#include "rvcc_target.h"

void main(void) {
    /* address-of and dereference */
    int x = 42;
    int *p = &x;
    printdec(*p);
    putc(' ');

    /* modify through pointer */
    *p = 99;
    printdec(x);
    putc(' ');

    /* pointer to array, arithmetic */
    int arr[] = {10, 20, 30};
    int *q = arr;
    printdec(*q);
    putc(' ');
    printdec(*(q + 2));
    putc(' ');

    /* char pointer and string indexing */
    char *s = "AB";
    printdec(s[0]);
    putc(' ');
    printdec(s[1]);
    println("");
}
