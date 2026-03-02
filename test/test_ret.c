#include "rvcc_target.h"

static int get_five(void) {
    return 5;
}

void main(void) {
    int x = get_five();
    printdec(x);
    println("");
}
