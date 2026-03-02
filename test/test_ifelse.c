#include "rvcc_target.h"

void main(void) {
    int x = 10;

    /* basic if */
    if (x == 10)
        println("x is 10");

    /* if-else */
    if (x > 20)
        println("FAIL");
    else
        println("x <= 20");

    /* chained if-else-if */
    if (x == 1)
        println("FAIL");
    else if (x == 5)
        println("FAIL");
    else if (x == 10)
        println("x is 10 again");
    else
        println("FAIL");

    /* nested if */
    if (x > 0) {
        if (x < 100) {
            println("0 < x < 100");
        }
    }

    /* if with logical operators */
    if (x > 5 && x < 15)
        println("5 < x < 15");

    if (x == 1 || x == 10)
        println("x is 1 or 10");
}
