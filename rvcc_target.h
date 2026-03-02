#ifndef RVCC_TARGET_H
#define RVCC_TARGET_H

#include "target-stdint.h"

#define NULL ((void*)0)
#define true 1
#define false 0
typedef unsigned char bool;

/* Syscall IDs — must match the host (vm_config.h / uvm32_common_custom.h) */
#define UVM32_SYSCALL_PUTC        0x00000000
#define UVM32_SYSCALL_GETC        0x00000001
#define UVM32_SYSCALL_PRINT       0x00000002
#define UVM32_SYSCALL_PRINTLN     0x00000003
#define UVM32_SYSCALL_PRINTDEC    0x00000004
#define UVM32_SYSCALL_PRINTHEX    0x00000005
#define UVM32_SYSCALL_MILLIS      0x00000006
#define UVM32_SYSCALL_I2C_WRITE       0x00000010
#define UVM32_SYSCALL_I2C_READ        0x00000011
#define UVM32_SYSCALL_YIELD       0x01000001
#define UVM32_SYSCALL_HALT        0x01000000

/*
 * syscall via compiler built-in __syscall(id, p1, p2)
 * The rvcc compiler emits: a7=id, a0=p1, a1=p2, ecall, result=a2
 */
#define syscall(id, p1, p2) __syscall((id), (p1), (p2))
#define syscall_cast(id, p1, p2) __syscall((uint32_t)(id), (uint32_t)(p1), (uint32_t)(p2))

#define println(x)      syscall_cast(UVM32_SYSCALL_PRINTLN, x, 0)
#define print(x)        syscall_cast(UVM32_SYSCALL_PRINT, x, 0)
#define printdec(x)     syscall_cast(UVM32_SYSCALL_PRINTDEC, x, 0)
#define printhex(x)     syscall_cast(UVM32_SYSCALL_PRINTHEX, x, 0)
#define putc(x)         syscall_cast(UVM32_SYSCALL_PUTC, x, 0)
#define getc()          syscall_cast(UVM32_SYSCALL_GETC, 0, 0)
#define yield()         syscall_cast(UVM32_SYSCALL_YIELD, 0, 0)

/* HAL: I2C. Returns 0 on ACK, 1 on NACK/error. */
static bool i2c_write(uint8_t dev_addr, uint8_t *buf, uint8_t len) {
    return (bool)syscall_cast(UVM32_SYSCALL_I2C_WRITE, buf, (dev_addr << 8) | len);
}

/* Write wlen bytes from buf, then read rlen bytes into buf+wlen (repeated start). */
static bool i2c_read(uint8_t dev_addr, uint8_t *buf, uint8_t wlen, uint8_t rlen) {
    return (bool)syscall_cast(UVM32_SYSCALL_I2C_READ, buf,
        ((uint32_t)dev_addr << 16) | ((uint32_t)wlen << 8) | rlen);
}

#endif
