#include "uvm32_target.h"

#define EEPROM_ADDR 0x50

void main(void)
{
    uint8_t buf[5];

    /* Write 4 bytes to EEPROM at memory address 0x00 */
    buf[0] = 0x00;  /* EEPROM memory address */
    buf[1] = 0xDE;
    buf[2] = 0xAD;
    buf[3] = 0xBE;
    buf[4] = 0xEF;

    println("EEPROM write 4 bytes at 0x00");
    if (i2c_write(EEPROM_ADDR, buf, 5)) {
        println("Write NACK!");
        return;
    }
    println("Write OK");

    /* Wait for EEPROM internal write cycle (~5ms).
     * Poll with a dummy write until ACK. */
    uint8_t dummy = 0x00;
    while (i2c_write(EEPROM_ADDR, &dummy, 1)) {
        yield();
    }

    /* Read back: write address byte, then read 4 bytes */
    buf[0] = 0x00;  /* EEPROM memory address */
    println("EEPROM read 4 bytes from 0x00");
    if (i2c_read(EEPROM_ADDR, buf, 1, 4)) {
        println("Read NACK!");
        return;
    }

    /* buf[1..4] now contain the read data */
    print("Read: ");
    printhex(buf[1]);
    putc(' ');
    printhex(buf[2]);
    putc(' ');
    printhex(buf[3]);
    putc(' ');
    printhex(buf[4]);
    println("");
}
