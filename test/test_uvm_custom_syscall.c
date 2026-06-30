#include "uvm32_target.h"

void main(void) {
    if (0xAABBCCDD == syscall(0xDEADBEEF, 0xABCD1234, 0xDECAFBAD)) {
        print("ok");
    } else {
        print("fail");
    }
}
