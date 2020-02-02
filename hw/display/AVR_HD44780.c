/*
 * QEMU JAZZ LED emulator.
 *
 * Copyright (c) 2007-2012 Herve Poussineau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/bcd.h"
#include "hw/i2c/i2c.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "qemu/timer.h"
#include "qapi/visitor.h"

#define TYPE_HD44780 "avr_hd44780"
#define HD44780(obj) OBJECT_CHECK(hd44780_state, (obj), TYPE_HD44780)

#define E   0x04
#define RS  0x01
#define DB7 (1<<7)
#define DB6 (1<<6)
#define DB5 (1<<5)
#define DB4 (1<<4)
#define DB3 (1<<3)
#define DB2 (1<<2)
#define DB1 (1<<1)
#define DB0 (1<<0)

#define LEN 5

#define COLUMNS_DDRAM 40
#define ROWS_DDRAM 2

#define SIZE_CGROM 128
#define HEIGHT_CHAR 8

#define SET_HEIGHT_SCREEN(rows) s->height = SCALE * 10 * rows + 2 * (SCALE * 2)
#define SET_WIDTH_SCREEN(columns) s->width = SCALE * (LEN + 1) * columns + 2 * (SCALE * 2)

typedef struct hd44780_state {
    I2CSlave parent_obj;

    bool rs;
    bool bit_mode_4;
    bool full_bit;
    bool active_autoscroll;

    /*
     *I/D(id) - Address Counter Offset Mode
     *I/D(id) = 1: Increment
     *I/D(id) = 0: Decrement
    */
    bool id;

    bool receive_custom_char;
    uint8_t custom_pos;
    int8_t pos;
    uint8_t total_char;
    int8_t offset;
    uint8_t full_data;
    uint8_t counter_mode;
    uint8_t counter;
    uint8_t cursor_pos;
    /*
     * cursor_type = 0 : Cursor off
     * cursor_type = 1 : Blinking cursor
     * cursor_type = 2 : Underline cursor
     * cursor_type = 3 : Blinking underlined cursor
    */
    uint8_t cursor_type; // 0 - no
    QemuConsole *con;
    uint8_t invalidate;
    unsigned char ddram[ROWS_DDRAM][COLUMNS_DDRAM];
    QEMUTimer* timer_blink;
    uint8_t columns;
    uint8_t rows;
    uint16_t width;
    uint16_t height;

    unsigned char CGRAM[8][8];
    unsigned char CGROM[SIZE_CGROM][HEIGHT_CHAR];
} hd44780_state;

const unsigned char Symbols[SIZE_CGROM][HEIGHT_CHAR] = {
    ['!'] = {0x1b,0x1b,0x1b,0x1b,0x1f,0x1f,0x1b,0x1f,},
    ['"'] = {0x15,0x15,0x15,0x1f,0x1f,0x1f,0x1f,0x1f,},
    ['#'] = {0x15,0x15,0x0,0x15,0x0,0x15,0x15,0x1f,},
    ['$'] = {0x1b,0x10,0xB,0x11,0x1A,0x1,0x1b,0x1f,},
    ['%'] = {0x7,0x6,0x1d,0x1b,0x17,0xc,0x1c,0x1f,},
    ['&'] = {0x17,0xb,0xb,0x16,0xa,0xd,0x12,0x1f,},
    ['\''] = {0x13,0x1b,0x17,0x1f,0x1f,0x1f,0x1f,0x1f,},
    ['('] = {0x1d,0x1b,0x17,0x17,0x17,0x1b,0x1d,0x1f,},
    [')'] = {0x17,0x1b,0x1d,0x1d,0x1d,0x1b,0x17,0x1f,},
    ['*'] = {0x1b,0xa,0x11,0x1b,0x11,0xa,0x1b,0x1f,},
    ['+'] = {0x1f,0x1b,0x1b,0x0,0x1b,0x1b,0x1f,0x1f,},
    [','] = {0x1f,0x1f,0x1f,0x1f,0x1f,0x13,0x13,0x1f,},
    ['-'] = {0x1f,0x1f,0x1f,0x0,0x1f,0x1f,0x1f,0x1f,},
    ['.'] = {0x1f,0x1f,0x1f,0x1f,0x1f,0x13,0x13,0x1f,},
    ['/'] = {0x1f,0x1e,0x1d,0x1b,0x17,0xf,0x1f,0x1f,},
    ['0'] = {0x11,0xe,0xc,0xa,0x6,0xe,0x11,0x1f,},
    ['1'] = {0x1b,0x13,0x1b,0x1b,0x1b,0x1b,0x11,0x1f,},
    ['2'] = {0x11,0xe,0x1e,0x1d,0x1b,0x17,0x0,0x1f,},
    ['3'] = {0x0,0x1d,0x1b,0x1d,0x1e,0xe,0x11,0x1f,},
    ['4'] = {0x1d,0x19,0x15,0xd,0x0,0x1d,0x1d,0x1f,},
    ['5'] = {0x0,0xf,0x1,0x1E,0x1E,0xE,0x11,0x1f,},
    ['6'] = {0x19,0x17,0xF,0x1,0xE,0xE,0x11,0x1f,},
    ['7'] = {0x00,0x1E,0x1D,0x1B,0x17,0x17,0x17,0x1F,},
    ['8'] = {0x11,0x0E,0x0E,0x11,0x0E,0x0E,0x11,0x1F,},
    ['9'] = {0x11,0x0E,0x0E,0x10,0x1E,0x1D,0x13,0x1F,},
    [':'] = {0x1F,0x13,0x13,0x1F,0x13,0x13,0x1F,0x1F,},
    [';'] = {0x1F,0x13,0x13,0x1F,0x13,0x1B,0x17,0x1F,},
    ['<'] = {0x1D,0x1B,0x17,0x0F,0x17,0x1B,0x1D,0x1F,},
    ['='] = {0x1F,0x1F,0x00,0x1F,0x00,0x1F,0x1F,0x1F,},
    ['>'] = {0x17,0x1B,0x1D,0x1E,0x1D,0x1B,0x17,0x1F,},
    ['?'] = {0x11,0x0E,0x1E,0x1D,0x1B,0x1F,0x1B,0x1F,},
    ['@'] = {0x11,0xe,0x1e,0x12,0xa,0xa,0x11,0x1f,},
    ['A'] = {0x11,0xe,0xe,0xe,0x0,0xe,0xe,0x1f,},
    ['B'] = {0x1,0xe,0xe,0x1,0xe,0xe,0x1,0x1f,},
    ['C'] = {0x11,0xe,0xf,0xf,0xf,0xe,0x11,0x1f,},
    ['D'] = {0x3,0xd,0xe,0xe,0xe,0xd,0x3,0x1f,},
    ['E'] = {0x0,0xf,0xf,0x1,0xf,0xf,0x0,0x1f,},
    ['F'] = {0x0,0xf,0xf,0x1,0xf,0xf,0xf,0x1f,},
    ['G'] = {0x11,0xe,0xf,0x8,0xe,0xe,0x11,0x1f,},
    ['H'] = {0xe,0xe,0xe,0x0,0xe,0xe,0xe,0x1f,},
    ['I'] = {0x11,0x1b,0x1b,0x1b,0x1b,0x1b,0x11,0x1f,},
    ['J'] = {0x18,0x1d,0x1d,0x1d,0x1d,0xd,0x13,0x1f,},
    ['K'] = {0xe,0xd,0xb,0x7,0xb,0xd,0xe,0x1f,},
    ['L'] = {0xf,0xf,0xf,0xf,0xf,0xf,0x0,0x1f,},
    ['M'] = {0xe,0x4,0xa,0xa,0xe,0xe,0xe,0x1f,},
    ['N'] = {0xe,0xe,0x6,0xa,0xc,0xe,0xe,0x1f,},
    ['O'] = {0x11,0xe,0xe,0xe,0xe,0xe,0x11,0x1f,},
    ['P'] = {0x1,0xe,0xe,0x1,0xf,0xf,0xf,0x1f,},
    ['Q'] = {0x11,0xe,0xe,0xe,0xa,0xd,0x12,0x1f,},
    ['R'] = {0x1,0xe,0xe,0x1,0xb,0xd,0xe,0x1f,},
    ['S'] = {0x11,0xe,0xf,0x11,0x1e,0xe,0x11,0x1f,},
    ['T'] = {0x0,0x1b,0x1b,0x1b,0x1b,0x1b,0x1b,0x1f,},
    ['U'] = {0xe,0xe,0xe,0xe,0xe,0xe,0x11,0x1f,},
    ['V'] = {0xe,0xe,0xe,0xe,0xe,0x15,0xb,0x1f,},
    ['W'] = {0xe,0xe,0xe,0xa,0xa,0xa,0x15,0x1f,},
    ['X'] = {0xe,0xe,0x15,0x1b,0x15,0xe,0xe,0x1f,},
    ['Y'] = {0xe,0xe,0xe,0x15,0x1b,0x1b,0x1b,0x1f,},
    ['Z'] = {0x0,0x1e,0x1d,0x1b,0x17,0xf,0x0,0x1f,},
    ['['] = {0x11,0x17,0x17,0x17,0x17,0x17,0x11,0x1f,},
    [']'] = {0x11,0x1d,0x1d,0x1d,0x1d,0x1d,0x11,0x1f,},
    ['^'] = {0x1b,0x15,0xe,0x1f,0x1f,0x1f,0x1f,0x1f,},
    ['_'] = {0x1f,0x1f,0x1f,0x1f,0x1f,0x1f,0x0,0x1f,},
    ['`'] = {0x17,0x1b,0x1f,0x1f,0x1f,0x1f,0x1f,0x1f,},
    ['a'] = {0x1f,0x1f,0x11,0x1e,0x10,0xe,0x10,0x1f,},
    ['b'] = {0xf,0xf,0x9,0x6,0xe,0xe,0x1,0x1f,},
    ['c'] = {0x1f,0x1f,0x11,0xf,0xf,0xe,0x11,0x1f,},
    ['d'] = {0x1e,0x1e,0x10,0xe,0xe,0xe,0x10,0x1f,},
    ['e'] = {0x1f,0x1f,0x11,0xe,0x0,0xf,0x11,0x1f,},
    ['f'] = {0x19,0x16,0x17,0x3,0x17,0x17,0x17,0x1f,},
    ['g'] = {0x1f,0x1f,0x10,0xe,0x10,0x1e,0x11,0x1f,},
    ['h'] = {0xf,0xf,0x9,0x6,0xe,0xe,0xe,0x1f,},
    ['i'] = {0x1b,0x1f,0x13,0x1b,0x1b,0x1b,0x11,0x1f,},
    ['j'] = {0x1d,0x1f,0x19,0x1d,0x1d,0xd,0x13,0x1f,},
    ['k'] = {0xf,0xf,0xd,0xb,0x7,0xb,0xd,0x1f,},
    ['l'] = {0x13,0x1b,0x1b,0x1b,0x1b,0x1b,0x11,0x1f,},
    ['m'] = {0x1f,0x1f,0x5,0xa,0xa,0xe,0xe,0x1f,},
    ['n'] = {0x1f,0x1f,0x9,0x6,0xe,0xe,0xe,0x1f,},
    ['o'] = {0x1f,0x1f,0x11,0xe,0xe,0xe,0x11,0x1f,},
    ['p'] = {0x1f,0x1f,0x1,0xe,0x1,0xf,0xf,0x1f,},
    ['q'] = {0x1f,0x1f,0x10,0xe,0x10,0x1e,0x1e,0x1f,},
    ['r'] = {0x1f,0x1f,0x14,0x13,0x17,0x17,0x17,0x1f,},
    ['s'] = {0x1f,0x1f,0x11,0xf,0x11,0x1e,0x1,0x1f,},
    ['t'] = {0x1b,0x1b,0x0,0x1b,0x1b,0x1b,0x1c,0x1f,},
    ['u'] = {0x1f,0x1f,0xe,0xe,0xe,0xc,0x12,0x1f,},
    ['v'] = {0x1f,0x1f,0xe,0xe,0xe,0x15,0x1b,0x1f,},
    ['w'] = {0x1f,0x1f,0xe,0xe,0xa,0xa,0x15,0x1f,},
    ['x'] = {0x1f,0x1f,0xe,0x15,0x1b,0x15,0xe,0x1f,},
    ['y'] = {0x1f,0x1f,0xe,0xe,0x10,0x1e,0x11,0x1f,},
    ['z'] = {0x1f,0x1f,0x0,0x1d,0x1b,0x17,0x0,0x1f,},
    ['{'] = {0x1D,0x1B,0x1B,0x17,0x1B,0x1B,0x1D,0x1F,},
    ['|'] = {0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,0x1B,},
    ['}'] = {0x17,0x1B,0x1B,0x1D,0x1B,0x1B,0x17,0x1F,},
    [' '] = {0x1f,0x1f,0x1f,0x1f,0x1f,0x1f,0x1f,0x1f,},};









typedef void (*drawchfn)(const uint8_t *, DisplaySurface *, int, int);
typedef void (*drawcursfn)(DisplaySurface *, int, int, int, bool);

#define DEPTH 8
#include "HD44780_template.h"
#define DEPTH 15
#include "HD44780_template.h"
#define DEPTH 16
#include "HD44780_template.h"
#define DEPTH 32
#include "HD44780_template.h"

static drawchfn draw_char_table[33] = {
    [0 ... 32]	= NULL,
    [8]		= draw_char_8,
    [15]	= draw_char_15,
    [16]	= draw_char_16,
    [32]	= draw_char_32,
};

static drawcursfn draw_cursor_table[33] = {
    [0 ... 32]	= NULL,
    [8]		= draw_cursor_8,
    [15]	= draw_cursor_15,
    [16]	= draw_cursor_16,
    [32]	= draw_cursor_32,
};

static void initialization_mode(hd44780_state *s, uint8_t data){
    switch (s->counter % 3) {
    case 0:
        if (!(data & E)) {
             printf("ERROR: expected enabled E\n");
        }
        break;
    case 1:
        data &= 0xF0;
        if (data == 0x30 && s->counter_mode < 3) {
            s->counter_mode++;
        } else if (s->counter_mode == 3 && data == 0x20) {
            s->counter_mode++;
        } else {
            printf("ERROR: Failed initialization \n");
        }
        break;
    case 2:
        if (data & E) {
            printf("ERROR: expected unenabled E\n");
        }
        if (s->counter_mode == 4) {
            s->bit_mode_4 = true;
            s->counter_mode = 0;
            s->counter = -1;
            printf("all inicial\n");
        }
        break;
    }
}

static void timer_blink_function(void *opaque)
{
    hd44780_state *s = opaque;

    timer_mod(s->timer_blink,qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)+500);
    s->cursor_type ^= 1;
}

static void clear_display(hd44780_state *s) {
    s->pos          = 0;
    s->offset       = 0;
    s->total_char   = 0;
    memset(s->ddram,0x80,sizeof(s->ddram[0][0])*ROWS_DDRAM*COLUMNS_DDRAM);
}

static void write_data(hd44780_state *s, uint8_t data) {
    data &= 0xF0;
    if (s->full_bit) {
        s->full_bit = false;
        data >>= 4;
        s->full_data |= data;
        if(s->receive_custom_char) {
            uint8_t columns = s->custom_pos % 8;
            uint8_t rows    = s->custom_pos / 8;
            if(rows > 7) {
                return;
            }
            /*
             * invert bits, because for display 1 - set bit, but for the emulator, vice versa
            */
            s->CGRAM[rows][columns] =~ s->full_data;
            s->custom_pos++;
        } else {
            if (s->pos >= 0x40) {
                s->ddram[1][s->pos-0x40] = s->full_data;
            } else if (s->pos <= 0x27){
                s->ddram[0][s->pos] = s->full_data;
            }
            if (s->id) {
                s->pos++;
                /*
                 * The gap is due to the fact that addressing uses seven-bit addressing,
                 * and the high bit indicates which line of memory is involved.
                */
                if (s->pos > 0x27 && s->pos < 0x40) {
                    s->pos = 0x40;
                } else if (s->pos > 0x67) {
                    s->pos = 0x0;
                }
            } else {
                s->pos--;
                if (s->pos < 0x0) {
                    s->pos = 0x67;
                } else if (s->pos > 0x27 && s->pos < 0x40) {
                    s->pos = 0x27;
                }
            }

            s->total_char++;

            if (s->active_autoscroll) {
                if (s->id) {
                    if (s->offset < 0x27) {
                        s->offset++;
                    } else {
                        s->offset = 0;
                    }
                } else {
                    if (s->offset > -0x27) {
                        s->offset--;
                    } else {
                        s->offset = 0;
                    }
                }
            }
        }

        s->full_data = 0;

    } else {
        s->full_bit = true;
        s->full_data |= data;
    }
}

static void write_command(hd44780_state *s, uint8_t data) {
    data &= 0xF0;
    if (s->full_bit) {
        s->full_bit = false;
        data >>= 4;
        s->full_data |= data;
        printf("DATA is: %x\n", s->full_data);
        if (s->full_data & DB7) {
            s->pos = s->full_data&~DB7;
            s->receive_custom_char = false;
        } else if (s->full_data & DB6) {
            s->custom_pos = s->full_data &~ DB6;
            s->receive_custom_char = true;
            printf("CGRAM address\n");
        } else if (s->full_data & DB5) {
            if (s->full_data & DB4) {
                printf("ERROR: expected unenabled DL\n");
            } else {
                if (s->full_data & DB3) {
                    printf("number of lines of the display is 2 \n");
                } else {
                    printf("number of lines of the display is 1 \n");
                }
                if (s->full_data & DB2) {
                    printf("font size is is 5x10 \n");
                } else {
                    printf("font size is is 5x7 \n");
                }
            }
        } else if (s->full_data & DB4) {
            printf("offset: %x\n", s->offset);
            if (s->full_data & DB3) {
                printf("SDVIG USER DISPLEY\n");
                if (s->full_data & DB2) {
                    printf("SDVIG USER RIGHT\n");
                    if (s->offset > -0x27) {
                        s->offset--;

                    } else {
                        s->offset = 0;
                    }
                } else {
                    printf("SDVIG USER LEFT\n");
                    if (s->offset < 0x27) {
                        s->offset++;

                    } else {
                        s->offset = 0;
                    }
                }
            } else {
                printf("SDVIG USER CURSOR\n");
                if (s->full_data & DB2) {
                    printf("SDVIG USER RIGHT\n");
                    if (s->pos < 0x67) {
                        s->pos++;
                    } else {
                        s->pos = 0;
                    }
                } else {
                    printf("SDVIG USER LEFT\n");
                    if (s->pos > 0) {
                        s->pos--;
                    } else {
                        s->pos = 0x67;
                    }
                }
            }
        } else if (s->full_data & DB3) {
            if (s->full_data & DB2) {
                printf("Display on \n");
            } else {
                printf("Display off \n");
            }
            if (s->full_data & DB1) {
                printf("Cursor on \n");
                s->cursor_type = 2;
            } else {
                printf("Cursor off \n");
                s->cursor_type = 0;
            }
            if (s->full_data & DB0) {
                printf("Cursor blink on \n");
                if (s->cursor_type == 2) {
                    s->cursor_type = 3;
                } else {
                    s->cursor_type = 1;
                }
                s->timer_blink = timer_new_ms(QEMU_CLOCK_VIRTUAL, timer_blink_function, s);
                timer_mod(s->timer_blink,qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)+500);
            } else {
                printf("Cursor blink off \n");
                if (s->cursor_type == 2) {
                    s->cursor_type = 2;
                } else {
                    s->cursor_type = 0;
                }
                timer_del(s->timer_blink);
            }
        } else if (s->full_data & DB2) {
            if (s->full_data & DB1) {
                s->id = true;
                printf("    move cursor right \n");
            } else {
                s->id = false;
                printf("move cursor left \n");
            }
            if (s->full_data & DB0) {
                s->active_autoscroll = true;
                printf("move display on \n");
            } else {
                printf("move display off \n");
            }
        } else if (s->full_data & DB1) {
            s->pos          = 0;
            s->offset       = 0;
            s->total_char   = 0;
        } else if (s->full_data & DB0) {
            clear_display(s);
            printf("clear display \n");
        }
        s->full_data = 0;
    } else {
        s->full_bit = true;
        s->full_data |= data;
    }
}

static void receive_byte(hd44780_state *s, uint8_t data) {
    switch (s->counter % 7) {
    case 0:
        if(data & RS){
            s->rs = true;
        } else {
            s->rs = false;
        }
        break;
    case 1:
        if (!(data & E)) {
            printf("ERROR: expected enabled E\n");
            s->counter = -1;
        }
        break;
    case 2:
        if (s->rs) {
            write_data(s,data);
        } else {
            write_command(s, data);
        }
        break;
    case 3:
        if (data & E) {
            printf("ERROR: expected unenabled E\n");
        }
        break;
    case 4:
        if (!(data & E)) {
            printf("ERROR: expected enabled E\n");
        }
        break;
    case 5:
        if (s->rs) {
            write_data(s,data);
        } else {
            write_command(s, data);
        }
        break;
    case 6:
        if (data & E) {
            printf("ERROR: expected unenabled E\n");
        }
        s->counter = -1;
        break;
    }
}
/*static uint64_t jazz_led_read(void *opaque, hwaddr addr,
                              unsigned int size)
{
    hd44780_state *s = opaque;
    uint8_t val;

    val = s->segments;
    trace_jazz_led_read(addr, val);

    return val;
}*/


static int hd44780_send(I2CSlave *i2c, uint8_t data)
{
    hd44780_state *s = HD44780(i2c);
    if (!s->bit_mode_4){
        initialization_mode(s,data);
    } else {
        receive_byte(s,data);
    }
    s->counter++;
    return 0;
}

static void hd44780_reset(DeviceState *dev)
{
    hd44780_state *s = HD44780(dev);

    s->rs                  = false;
    s->bit_mode_4          = false;
    s->full_bit            = false;
    s->active_autoscroll   = false;
    s->receive_custom_char = false;
    s->id                  = false;
    s->full_data           = 0;
    s->counter_mode        = 0;
    s->counter             = 0;
    s->pos                 = 0;
    s->offset              = 0;
    s->total_char          = 0;
    s->custom_pos          = 0;

    memset(s->ddram,0x80,sizeof(s->ddram[0][0])*ROWS_DDRAM*COLUMNS_DDRAM);
}

static void hd44780_led_invalidate_display(void *opaque)
{
    hd44780_state *s = opaque;
    s->invalidate = 1;
}

/*
 * begin - position in ggram to start
 * num_char - number of characters to output at a time
 * rows - rows in ggram
 * columns_disp - columns in display
 * line_disp - line in display
*/
static void hd44780_led_printf_str(uint8_t **src, DisplaySurface **surface,
                                   uint8_t begin, uint8_t num_char,
                                   uint8_t rows, uint8_t columns_disp,
                                   uint8_t rows_disp, hd44780_state *s) {
    drawchfn draw_char = draw_char_table[surface_bits_per_pixel(*surface)];
    drawcursfn draw_cursor = draw_cursor_table[surface_bits_per_pixel(*surface)];

    int left_border = begin + s->offset;
    int i = left_border;

    while(num_char > 0x0) {
        if (i > 0x27) {
            i -= 0x28;
            left_border -= 0x28;
        }
        else if (i < 0) {
            i += 0x28;
            left_border += 0x28;
        }

        /*
         * draw character
        */

        *src = s->CGROM[' '];
        if (s->ddram[rows][i] != 0x80) {
            if(s->ddram[rows][i] <= 7) {
                *src = s->CGRAM[s->ddram[rows][i]];
            } else {
                *src = s->CGROM[s->ddram[rows][i]];
            }
        }

        draw_char(*src,*surface,columns_disp,rows_disp);

        /*
         * draw cursor
        */
        int cursor_pos = s->pos - 0x40 * rows;
        //printf("cursor_pos : %x, i = %x, left_border = %x \n", cursor_pos, i, left_border);
        if (i == cursor_pos) {
            cursor_pos = i-left_border;
            if (s->rows == 1 && rows == 1) {
                cursor_pos += s->columns/2;
            }
            if (s->cursor_type == 3 || s->cursor_type == 2) {
                draw_cursor(*surface,cursor_pos,rows_disp,0xff,false);
            }
            if (s->cursor_type == 3 || s->cursor_type == 1) {
                draw_cursor(*surface,cursor_pos,rows_disp,0xff,true);
            }
        }
        i++;
        columns_disp++;
        num_char--;
    }
}

static void hd44780_led_update_display(void *opaque)
{
    hd44780_state *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint8_t *src;
    if (!surface_bits_per_pixel(surface)) {
        return;
    }

    for(int j=0; j<2; j++) {
        for(int i=0; i<=0x27; i++) {
            printf("%i",s->ddram[j][i]);
        }
        printf("\n");
    }
    for(int i=0; i<8; i++) {
        for(int j=0; j<8; j++) {
            printf("0x%x ", s->CGRAM[i][j]);
        }
        printf("\n");
    }


    if (s->rows == 1) {
        hd44780_led_printf_str(&src,&surface,0,s->columns/2,0,0,0,s);
        hd44780_led_printf_str(&src,&surface,0,s->columns/2,1,s->columns/2,0,s);
    } else if (s->rows == 2) {
        hd44780_led_printf_str(&src,&surface,0,s->columns,0,0,0,s);
        hd44780_led_printf_str(&src,&surface,0,s->columns,1,0,1,s);
    } else if (s->rows == 4) {
        for (int i=0;i<2;i++) {
            hd44780_led_printf_str(&src,&surface,0,s->columns,i,0,i,s);
            hd44780_led_printf_str(&src,&surface,s->columns,s->columns,i,0,2+i,s);
        }
    }

    printf("s->width = %u , s->height = %u, s->offset = %u", s->width, s->height, s->offset);
    dpy_gfx_update(s->con, 0, 0, s->width, s->height);
    s->invalidate = 0;
}

static const GraphicHwOps hd44780_led_ops = {
    .invalidate  = hd44780_led_invalidate_display,
    .gfx_update  = hd44780_led_update_display,
};

static void hd44780_realize(DeviceState *dev, Error **errp)
{
    hd44780_state *s = HD44780(dev);
    s->columns = 8;
    s->rows   = 1;
    SET_WIDTH_SCREEN(8);
    SET_HEIGHT_SCREEN(1);
    s->con = graphic_console_init(dev, 0, &hd44780_led_ops, s);
    memcpy(s->CGROM, Symbols, SIZE_CGROM * HEIGHT_CHAR);
}

static void hd44780_set_columns(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    hd44780_state *s = HD44780(obj);
    Error *local_err = NULL;
    uint8_t columns;

    visit_type_uint8(v, name, &columns, &local_err);
//    if (local_err) {
//        error_propagate(errp, local_err);
//        return;
//    }
    SET_WIDTH_SCREEN(columns);
    s->columns = columns;
    qemu_console_resize(s->con, s->width, s->height);
}
static void hd44780_set_rows(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    hd44780_state *s = HD44780(obj);
    Error *local_err = NULL;
    uint8_t rows;

    visit_type_uint8(v, name, &rows, &local_err);
//    if (local_err) {
//        error_propagate(errp, local_err);
//        return;
//    }
    SET_HEIGHT_SCREEN(rows);
    s->rows = rows;
    qemu_console_resize(s->con, s->width, s->height);
}

static void hd44780_initfn(Object *obj)
{
    object_property_add(obj, "columns", "uint8_t",
                        NULL, hd44780_set_columns,
                        NULL, NULL, NULL);
    object_property_add(obj, "rows", "uint8_t",
                        NULL, hd44780_set_rows,
                        NULL, NULL, NULL);
}

static void hd44780_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);

    sc->send    = hd44780_send;
    //sc->event   = hd44780_event;
    dc->realize = hd44780_realize;
    dc->reset   = hd44780_reset;
    //dc->props   = hd44780_properties;
}

static const TypeInfo hd44780_info = {
    .name          = TYPE_HD44780,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(hd44780_state),
    .class_init    = hd44780_class_init,
    .instance_init = hd44780_initfn,
};

static void hd44780_register(void)
{
    type_register_static(&hd44780_info);
}

type_init(hd44780_register);