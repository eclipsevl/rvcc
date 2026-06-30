#include "uvm32_target.h"

#define SSD1306_ADDR 0x3C

/* Send one SSD1306 command byte */
int ssd1306_cmd(uint8_t cmd)
{
    uint8_t buf[2];
    buf[0] = 0x00;   /* control byte: command */
    buf[1] = cmd;
    println("cmd");
    return i2c_write(SSD1306_ADDR, buf, 2);
}

/* Send N data bytes to SSD1306 */
int ssd1306_data(uint8_t *data, uint8_t len)
{
    uint8_t buf[17];
    uint8_t i;

    if (len > 16)
        len = 16;

    buf[0] = 0x40;   /* control byte: data */
    for (i = 0; i < len; i++) {
        buf[i + 1] = data[i];
    }

    return i2c_write(SSD1306_ADDR, buf, len + 1);
}

/* Basic SSD1306 init for 128x64 panel */
int ssd1306_init(void)
{
    uint8_t init_seq[] = {
        0xAE,       /* display off */
        0xD5, 0x80,
        0xA8, 0x3F,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14,
        0x20, 0x02, /* page addressing mode */
        0xA1,
        0xC8,
        0xDA, 0x12,
        0x81, 0x7F,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0x2E,
        0xAF
    };

    uint8_t i;
    for (i = 0; i < 26; i++) {
        if (ssd1306_cmd(init_seq[i]))
            return 1;
    }
    return 0;
}

/* Set cursor in page addressing mode */
int ssd1306_setpos(uint8_t page, uint8_t col)
{
    if (ssd1306_cmd(0xB0 | (page & 0x07))) return 1;
    if (ssd1306_cmd(0x00 | (col & 0x0F))) return 1;
    if (ssd1306_cmd(0x10 | ((col >> 4) & 0x0F))) return 1;
    return 0;
}

/* Clear whole screen */
int ssd1306_clear(void)
{
    uint8_t page, i;
    uint8_t zeros[16];

    for (i = 0; i < 16; i++)
        zeros[i] = 0x00;

    for (page = 0; page < 8; page++) {
        if (ssd1306_setpos(page, 0))
            return 1;

        for (i = 0; i < 8; i++) {
            if (ssd1306_data(zeros, 16))
                return 1;
        }
    }
    return 0;
}

/* Minimal 5x7 font for only needed characters */
uint8_t font_space[5] = {0x00, 0x00, 0x00, 0x00, 0x00};
uint8_t font_H[5]     = {0x7F, 0x08, 0x08, 0x08, 0x7F};
uint8_t font_Y[5]     = {0x03, 0x04, 0x78, 0x04, 0x03};
uint8_t font_e[5]     = {0x38, 0x54, 0x54, 0x54, 0x18};
uint8_t font_f[5]     = {0x08, 0x7E, 0x09, 0x01, 0x02};
uint8_t font_k[5]     = {0x7F, 0x10, 0x28, 0x44, 0x00};
uint8_t font_l[5]     = {0x00, 0x41, 0x7F, 0x40, 0x00};
uint8_t font_m[5]     = {0x7C, 0x04, 0x18, 0x04, 0x78};
uint8_t font_o[5]     = {0x38, 0x44, 0x44, 0x44, 0x38};
uint8_t font_r[5]     = {0x7C, 0x08, 0x04, 0x04, 0x08};

uint8_t *glyph(char c)
{
    switch (c) {
    case 'H': return font_H;
    case 'Y': return font_Y;
    case 'e': return font_e;
    case 'f': return font_f;
    case 'k': return font_k;
    case 'l': return font_l;
    case 'm': return font_m;
    case 'o': return font_o;
    case 'r': return font_r;
    case ' ': return font_space;
    default:  return font_space;
    }
}

/* Draw one character as 5 columns + 1 blank column */
int ssd1306_putc(char c)
{
    uint8_t out[6];
    uint8_t *g;
    uint8_t i;

    g = glyph(c);

    for (i = 0; i < 5; i++)
        out[i] = g[i];
    out[5] = 0x00;

    return ssd1306_data(out, 6);
}

/* Draw a zero-terminated string */
int ssd1306_print(char *s)
{
    while (*s) {
        if (ssd1306_putc(*s))
            return 1;
        s++;
    }
    return 0;
}

void main(void)
{
    char *msg = "Hello from York";

    println("SSD1306 init");
    if (ssd1306_init()) {
        println("SSD1306 NACK during init!");
        return;
    }

    println("SSD1306 clear");
    if (ssd1306_clear()) {
        println("SSD1306 NACK during clear!");
        return;
    }

    if (ssd1306_setpos(2, 8)) {
        println("SSD1306 setpos failed!");
        return;
    }

    println("Writing label");
    if (ssd1306_print(msg)) {
        println("SSD1306 write failed!");
        return;
    }

    println("Done");
}
