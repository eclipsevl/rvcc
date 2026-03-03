#include "rvcc_target.h"

void main(void) {
    /* string literal */
    println("test");

    /* char array init */
    char buf[] = "ABCD";
    printdec(buf[0]);   /* 65 = 'A' */
    putc(' ');
    printdec(buf[3]);   /* 68 = 'D' */
    putc(' ');

    /* string length via index */
    char *s = "hello";
    int len = 0;
    char ch = s[0];
    int i = 0;
    while (ch != 0) {
        len = len + 1;
        i = i + 1;
        ch = s[i];
    }
    printdec(len);      /* 5 */
    putc(' ');

    /* escape sequences */
    char tab = '\t';
    char nl = '\n';
    printdec(tab);   /* 9 */
    putc(' ');
    printdec(nl);    /* 10 */
    putc(' ');

    /* hex escape */
    char hex = '\x41';
    printdec(hex);   /* 65 = 'A' */
    println("");
}
