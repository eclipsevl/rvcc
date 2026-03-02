#include "rvcc_target.h"

void main(void) {
    /* basic switch with break */
    int x = 2;
    switch (x) {
    case 1:
        println("FAIL");
        break;
    case 2:
        println("x is 2");
        break;
    case 3:
        println("FAIL");
        break;
    }

    /* switch with default */
    int y = 99;
    switch (y) {
    case 1:
        println("FAIL");
        break;
    case 2:
        println("FAIL");
        break;
    default:
        println("y is default");
        break;
    }

    /* switch fall-through */
    int z = 1;
    switch (z) {
    case 1:
    case 2:
        println("z is 1 or 2");
        break;
    case 3:
        println("FAIL");
        break;
    }

    /* switch with expressions */
    int w = 10;
    switch (w + 5) {
    case 15:
        println("w+5 is 15");
        break;
    default:
        println("FAIL");
        break;
    }
}
