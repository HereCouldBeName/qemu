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
#define DB5 (1<<5)
#define DB4 (1<<4)
#define DB3 (1<<3)
#define DB2 (1<<2)
#define DB1 (1<<1)
#define DB0 (1<<0)

#define LEN 5

typedef struct hd44780_state {
    I2CSlave parent_obj;

    bool rs;
    bool bit_mode_4;
    bool IsAll;
    uint8_t pos;
    uint8_t total_char;
    uint8_t poslast[2];
    uint8_t offset;
    uint8_t full_data;
    uint8_t counter_mode;
    uint8_t counter;
    uint8_t cursor_pos;
    uint8_t cursor_type; // 0 - no
    QemuConsole *con;
    int invalidate;
    unsigned char ddram[2][40];
    QEMUTimer* timerl_offset;
    QEMUTimer* timer_blink;
    uint8_t column;
    uint8_t line;
    uint16_t width;
    uint16_t height;

} hd44780_state;

typedef void (*drawchfn)(const uint8_t *, DisplaySurface *, int, int);
typedef void (*drawcursfn)(DisplaySurface *, int, int, int, bool);

unsigned char CGRAM[][7] = {['H'] = {0xe,0xe,0xe,0x0,0xe,0xe,0xe,},
                            ['e'] = {0x1f,0x1f,0x11,0xe,0x0,0xf,0x11,},
                            ['l'] = {0x13,0x1b,0x1b,0x1b,0x1b,0x1b,0x11,},
                            ['o'] = {0x1f,0x1f,0x11,0xe,0xe,0xe,0x11,},
                            [' '] = {0x1f,0x1f,0x1f,0x1f,0x1f,0x1f,0x1f,},
                            ['W'] = {0xe,0xe,0xe,0xa,0xa,0xa,0x15,},
                            ['r'] = {0x1f,0x1f,0x14,0x13,0x17,0x17,0x17,},
                            ['d'] = {0x1e,0x1e,0x10,0xe,0xe,0xe,0x10,},
                            ['!'] = {0x1b,0x1b,0x1b,0x1b,0x1f,0x1f,0x1b,},
                            ['S'] = {0x11,0xe,0xf,0x11,0x1e,0xe,0x11,},
                            ['t'] = {0x1b,0x1b,0x0,0x1b,0x1b,0x1b,0x1c,},
                            ['i'] = {0x1b,0x1f,0x13,0x1b,0x1b,0x1b,0x11,},
                            ['n'] = {0x1f,0x1f,0x9,0x6,0xe,0xe,0xe,},
                            ['g'] = {0x1f,0x1f,0x10,0xe,0x10,0x1e,0x11,},
                            ['"'] = {0x15,0x15,0x15,0x1f,0x1f,0x1f,0x1f,},
                            ['#'] = {0x15,0x15,0x0,0x15,0x0,0x15,0x15,},
                            ['$'] = {0x1b,0x10,0xB,0x11,0x1A,0x1,0x1b,},
                            ['%'] = {0x7,0x6,0x1d,0x1b,0x17,0xc,0x1c,},
                            ['&'] = {0x17,0xb,0xb,0x16,0xa,0xd,0x12,},
                            ['\''] = {0x13,0x1b,0x17,0x1f,0x1f,0x1f,0x1f,},
                            ['('] = {0x1d,0x1b,0x17,0x17,0x17,0x1b,0x1d,},
                            [')'] = {0x17,0x1b,0x1d,0x1d,0x1d,0x1b,0x17,},
                            ['*'] = {0x1b,0xa,0x11,0x1b,0x11,0xa,0x1b,},
                            ['+'] = {0x1f,0x1b,0x1b,0x0,0x1b,0x1b,0x1f,},
                            [','] = {0x1f,0x1f,0x1f,0x1f,0x1f,0x13,0x13,},
                            ['-'] = {0x1f,0x1f,0x1f,0x0,0x1f,0x1f,0x1f,},
                            ['.'] = {0x1f,0x1f,0x1f,0x1f,0x1f,0x13,0x13,},
                            ['/'] = {0x1f,0x1e,0x1d,0x1b,0x17,0xf,0x1f,},
                            ['0'] = {0x11,0xe,0xc,0xa,0x6,0xe,0x11,},
                            ['1'] = {0x1b,0x13,0x1b,0x1b,0x1b,0x1b,0x11,},
                            ['2'] = {0x11,0xe,0x1e,0x1d,0x1b,0x17,0x0,},
                            ['3'] = {0x0,0x1d,0x1b,0x1d,0x1e,0xe,0x11,},
                            ['4'] = {0x1d,0x19,0x15,0xd,0x0,0x1d,0x1d,},
                            ['@'] = {0x11,0xe,0x1e,0x12,0xa,0xa,0x11,},
                            ['A'] = {0x11,0xe,0xe,0xe,0x0,0xe,0xe,},
                            ['B'] = {0x1,0xe,0xe,0x1,0xe,0xe,0x1,},
                            ['C'] = {0x11,0xe,0xf,0xf,0xf,0xe,0x11,},
                            ['D'] = {0x3,0xd,0xe,0xe,0xe,0xd,0x3,},
                            ['E'] = {0x0,0xf,0xf,0x1,0xf,0xf,0x0,},
                            ['F'] = {0x0,0xf,0xf,0x1,0xf,0xf,0xf,},
                            ['G'] = {0x11,0xe,0xe,0x8,0xe,0xe,0x10,},
                            ['I'] = {0x11,0x1b,0x1b,0x1b,0x1b,0x1b,0x11,},
                            ['J'] = {0x18,0x1d,0x1d,0x1d,0x1d,0xd,0x13,},
                            ['K'] = {0xe,0xd,0xb,0x7,0xb,0xd,0xe,},
                            ['L'] = {0xf,0xf,0xf,0xf,0xf,0xf,0x0,},
                            ['M'] = {0xe,0x4,0xa,0xa,0xe,0xe,0xe,},
                            ['N'] = {0xe,0xe,0x6,0xa,0xc,0xe,0xe,},
                            ['O'] = {0x11,0xe,0xe,0xe,0xe,0xe,0x11,},
                            ['P'] = {0x1,0xe,0xe,0x1,0xf,0xf,0xf,},
                            ['Q'] = {0x11,0xe,0xe,0xe,0xa,0xd,0x12,},
                            ['R'] = {0x1,0xe,0xe,0x1,0xb,0xd,0xe,},
                            ['T'] = {0x0,0x1b,0x1b,0x1b,0x1b,0x1b,0x1b,},
                            ['U'] = {0xe,0xe,0xe,0xe,0xe,0xe,0x11,},
                            ['V'] = {0xe,0xe,0xe,0xe,0xe,0x15,0xb,},
                            ['X'] = {0xe,0xe,0x15,0x1b,0x15,0xe,0xe,},
                            ['Y'] = {0xe,0xe,0xe,0x15,0x1b,0x1b,0x1b,},
                            ['Z'] = {0x0,0x1e,0x1d,0x1b,0x17,0xf,0x0,},
                            ['['] = {0x11,0x17,0x17,0x17,0x17,0x17,0x11,},
                            [']'] = {0x11,0x1d,0x1d,0x1d,0x1d,0x1d,0x11,},
                            ['^'] = {0x1b,0x15,0xe,0x1f,0x1f,0x1f,0x1f,},
                            ['_'] = {0x1f,0x1f,0x1f,0x1f,0x1f,0x1f,0x0,},
                            ['`'] = {0x17,0x1b,0x1f,0x1f,0x1f,0x1f,0x1f,},
                            ['a'] = {0x1f,0x1f,0x11,0x1e,0x10,0xe,0x10,},
                            ['b'] = {0xf,0xf,0x9,0x6,0xe,0xe,0x1,},
                            ['c'] = {0x1f,0x1f,0x11,0xf,0xf,0xe,0x11,},
                            ['f'] = {0x19,0x16,0x17,0x3,0x17,0x17,0x17,},
                            ['h'] = {0xf,0xf,0x9,0x6,0xe,0xe,0xe,},
                            ['j'] = {0x1d,0x1f,0x19,0x1d,0x1d,0xd,0x13,},
                            ['k'] = {0xf,0xf,0xd,0xb,0x7,0xb,0xd,},
                            ['m'] = {0x1f,0x1f,0x5,0xa,0xa,0xe,0xe,},
                            ['p'] = {0x1f,0x1f,0x1,0xe,0x1,0xf,0xf,},
                            ['q'] = {0x1f,0x1f,0x10,0xe,0x10,0x1e,0x1e,},
                            ['s'] = {0x1f,0x1f,0x11,0xf,0x11,0x1e,0x1,},
                            ['u'] = {0x1f,0x1f,0xe,0xe,0xe,0xc,0x12,},
                            ['v'] = {0x1f,0x1f,0xe,0xe,0xe,0x15,0x1b,},
                            ['w'] = {0x1f,0x1f,0xe,0xe,0xa,0xa,0x15,},
                            ['x'] = {0x1f,0x1f,0xe,0x15,0x1b,0x15,0xe,},
                            ['y'] = {0x1f,0x1f,0xe,0xe,0x10,0x1e,0x11,},
                            ['z'] = {0x1f,0x1f,0x0,0x1d,0x1b,0x17,0x0,},};
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
        }
        else {
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

static void timer_function(void *opaque)
{
    hd44780_state *s = opaque;
//    if(s->offset < (s->poslast[0] > s->poslast[1]?s->poslast[0]:s->poslast[1])) {
//        timer_mod(s->timerl_offset,qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)+800);
//    }
    if(s->total_char > 0) {
        timer_mod(s->timerl_offset,qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)+800);
        s->total_char--;
    }
    s->cursor_pos--;
    if(s->offset < 0x27) {
        s->offset++;
    } else {
        s->offset = 0;
        s->cursor_pos = s->pos;
    }
    printf("s->poslast[0] = %u\n",s->poslast[0]);
    printf("s->poslast[0] = %u\n",s->poslast[1]);
    printf("s->offset = %u\n",s->offset);
}

static void timer_blink_function(void *opaque)
{
    hd44780_state *s = opaque;

    timer_mod(s->timer_blink,qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)+500);
    if (s->cursor_type == 3) {
        s->cursor_type = 2;
    } else if (s->cursor_type == 1) {
        s->cursor_type = 0;
    } else if (s->cursor_type == 2) {
        s->cursor_type = 3;
    } else if (s->cursor_type == 0) {
        s->cursor_type = 1;
    }
}

static void clear_display(hd44780_state *s) {
    s->pos          = 0;
    s->offset       = 0;
    s->cursor_pos   = 0;
    s->total_char   = 0;
    memset(s->poslast, 0, sizeof (s->poslast));
    memset(s->ddram,0,sizeof(s->ddram[0][0])*2*40);
}

static void write_data(hd44780_state *s, uint8_t data) {
    //printf("get: %x \n",data);
    data &= 0xF0;
    if(s->IsAll) {
        s->IsAll = false;
        data >>= 4;
        s->full_data |= data;
        //printf("%c \n",s->full_data);
        if(s->pos >= 0x40) {
            s->ddram[1][s->pos-0x40] = s->full_data;
            s->poslast[1] = s->pos-0x40;
            //s->cursor_pos = s->pos-0x40 + 0x1;
        } else if(s->pos <= 0x27){
            s->ddram[0][s->pos] = s->full_data;
            s->poslast[0] = s->pos;
            //s->cursor_pos = s->pos + 0x1;
        }
        s->total_char++;
        s->pos++;
        s->cursor_pos++;
        s->full_data = 0;
    } else {
        s->IsAll = true;
        s->full_data |= data;
    }
}

static void write_command(hd44780_state *s, uint8_t data) {
    data &= 0xF0;
    if(s->IsAll) {
        s->IsAll = false;
        data >>= 4;
        s->full_data |= data;
        printf("DATA is: %x\n", s->full_data);
        if (s->full_data & DB7) {
            s->pos = s->full_data&~DB7;
            s->cursor_pos = s->pos;
        } else if(s->full_data & DB5) {
            if(s->full_data & DB4) {
                printf("ERROR: expected unenabled DL\n");
            } else {
                if(s->full_data & DB3) {
                    printf("number of lines of the display is 2 \n");
                } else {
                    printf("number of lines of the display is 1 \n");
                }
                if(s->full_data & DB2) {
                    printf("font size is is 5x10 \n");
                } else {
                    printf("font size is is 5x7 \n");
                }
            }
        } else if(s->full_data & DB4) {
            printf("offset: %x\n", s->offset);
            if(s->full_data & DB3) {
                printf("SDVIG USER DISPLEY\n");
                s->cursor_pos--;
                if(s->offset < 0x27) {
                    s->offset++;
                } else {
                    s->offset = 0;
                    s->cursor_pos = s->pos;
                }
            } else {
                printf("SDVIG USER CURSOR\n");
            }
            if(s->full_data & DB2) {
                printf("SDVIG USER RIGHT\n");
            } else {
                printf("SDVIG USER LEFT\n");
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
                if(s->cursor_type == 2) {
                    s->cursor_type = 3;
                } else {
                    s->cursor_type = 1;
                }
                s->timer_blink = timer_new_ms(QEMU_CLOCK_VIRTUAL, timer_blink_function, s);
                timer_mod(s->timer_blink,qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)+500);
            } else {
                printf("Cursor blink off \n");
                if(s->cursor_type == 2) {
                    s->cursor_type = 2;
                } else {
                    s->cursor_type = 0;
                }
            }
        } else if (s->full_data & DB2) {
            if (s->full_data & DB1) {
                printf("move cursor right \n");
            } else {
                printf("move cursor left \n");
            }
            if (s->full_data & DB0) {
                s->timerl_offset = timer_new_ms(QEMU_CLOCK_VIRTUAL, timer_function, s);
                timer_mod(s->timerl_offset,qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL)+800);

                printf("move display on \n");
            } else {
                printf("move display off \n");
            }
        } else if (s->full_data & DB1) {
            s->pos          = 0;
            s->offset       = 0;
            s->cursor_pos   = 0;
            s->total_char   = 0;
            memset(s->poslast, 0, sizeof (s->poslast));
        } else if (s->full_data & DB0) {
            clear_display(s);
            printf("clear display \n");
        }
        s->full_data = 0;
    } else {
        s->IsAll = true;
        s->full_data |= data;
    }
}

static void receive_byte(hd44780_state *s, uint8_t data) {
    printf("counter is %u\n", s->counter);
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

static uint16_t set_height(uint8_t line) {
    return SCALE * 9 * line + 2 * (SCALE * 2);
}
static uint16_t set_width(uint8_t column) {
    return SCALE * (LEN + 1) * column + 2 * (SCALE * 2);
}

static void hd44780_reset(DeviceState *dev)
{
    hd44780_state *s = HD44780(dev);

    s->rs           = false;
    s->bit_mode_4   = false;
    s->IsAll        = false;
    s->full_data    = 0;
    s->counter_mode = 0;
    s->counter      = 0;
    s->pos          = 0;
    s->offset       = 0;
    s->cursor_pos   = 0;
    s->total_char   = 0;
//    s->height       = set_height(1);
//    s->width        = set_width(8);
    memset(s->poslast, 0, sizeof (s->poslast));
    memset(s->ddram,0,sizeof(s->ddram[0][0])*2*40);
}

static void hd44780_led_invalidate_display(void *opaque)
{
    hd44780_state *s = opaque;
    s->invalidate = 1;
}


static void hd44780_led_clear_display(void *opaque)
{
    hd44780_state *s = opaque;
    DisplaySurface *surface = qemu_console_surface(s->con);

    if (!surface_bits_per_pixel(surface)) {
        return;
    }

    drawcursfn draw_cursor = draw_cursor_table[surface_bits_per_pixel(surface)];

    if(s->line == 1) {
        for(int i=0; i<s->column; i++) {
            draw_cursor(surface,i,0,0,false);
            draw_cursor(surface,i,0,0,true);
        }
    } else if(s->line == 2) {
        for(int i=0; i<s->column; i++) {
            for(int j=0; j<2; j++) {
                draw_cursor(surface,i, j,0,false);
                draw_cursor(surface,i, j,0,true);
            }
        }
    } else if(s->line == 4) {
        for(int i=0; i<s->column; i++) {
            for(int j=0; j<4; j++) {
                draw_cursor(surface,i, j,0,false);
                draw_cursor(surface,i, j,0,true);
            }
        }
    }

    dpy_gfx_update(s->con, 0, 0,  s->width, s->height);
    s->invalidate = 0;
}
/*
 * begin - position in ggram to start
 * end - position in ggram to end
 * line - line in ggram
 * column_disp - column in display
 * line_disp - line in display
*/
static void hd44780_led_printf_str(uint8_t **src, DisplaySurface **surface, uint8_t begin, uint8_t end, uint8_t line, uint8_t column_disp, uint8_t line_disp, hd44780_state *s) {
    drawchfn draw_char = draw_char_table[surface_bits_per_pixel(*surface)];
    for(int i=begin + s->offset; i< s->offset + end && i < 0x28; i++, column_disp++) {
        if(s->ddram[line][i] == 0x0) {
            continue;
        }
        *src = CGRAM[s->ddram[line][i]];
        draw_char(*src,*surface,column_disp,line_disp);
    }
}

static void hd44780_led_update_display(void *opaque)
{
    hd44780_state *s = opaque;
    printf("column: %u\n", s->column);
    printf("line: %u\n", s->line);
    DisplaySurface *surface = qemu_console_surface(s->con);
    uint8_t *src;
    if (!surface_bits_per_pixel(surface)) {
        return;
    }

    hd44780_led_clear_display(s);

    for(int j=0; j<2; j++) {
        for(int i=s->offset; i<0x27; i++) {
            printf("%c ",s->ddram[j][i]);
        }
        printf("\n");
    }

    drawcursfn draw_cursor = draw_cursor_table[surface_bits_per_pixel(surface)];

    /*for 8x2 16x2 20x2*/
    if(s->line == 1) {
        hd44780_led_printf_str(&src,&surface,0,s->column/2,0,0,0,s);
        hd44780_led_printf_str(&src,&surface,0,s->column/2,1,s->column/2,0,s);
    } else if(s->line == 2) {
        hd44780_led_printf_str(&src,&surface,0,s->column,0,0,0,s);
        hd44780_led_printf_str(&src,&surface,0,s->column,1,0,1,s);
    } else if(s->line == 4) {
        for (int i=0;i<2;i++) {
            hd44780_led_printf_str(&src,&surface,0,s->column,i,0,i,s);
            hd44780_led_printf_str(&src,&surface,s->column,s->column*2,i,0,2+i,s);
        }
    }

    if(s->line == 1) {
        if(s->cursor_pos >= 0x40 && s->cursor_pos < 0x40 + s->column/2) {
            draw_cursor(surface,s->cursor_pos - 0x40  + s->column/2 ,0,0xff,false);
            if(s->cursor_pos == 0x40) {
                s->cursor_pos = s->poslast[0]-s->offset+1;
            }
        } else if(s->cursor_pos <= (s->column / 2)) {
            draw_cursor(surface,s->cursor_pos,0,0xff,false);
            printf("s->cursor_pos = %u\n",s->cursor_pos);
        }
    } else if(s->line == 2){
        if(s->cursor_pos >= 0x40 && s->cursor_pos < 0x40 + s->column) {
            if (s->cursor_type == 3 || s->cursor_type == 2) {
                draw_cursor(surface,s->cursor_pos - 0x40,1,0xff,false);
            }
            if (s->cursor_type == 3 || s->cursor_type == 1) {
                draw_cursor(surface,s->cursor_pos - 0x40,1,0xff,true);
            }
        } else if(s->cursor_pos <= s->column) {
            if (s->cursor_type == 3 || s->cursor_type == 2) {
                draw_cursor(surface,s->cursor_pos,0,0xff,false);
            }
            if (s->cursor_type == 3 || s->cursor_type == 1) {
                draw_cursor(surface,s->cursor_pos,0,0xff,true);
            }
        }
    } else if(s->line == 4){
        printf("s->cursor_pos = 0x%x\n",s->cursor_pos);
        if(s->cursor_pos >= 0x40 && s->cursor_pos < 0x40 + s->column) { // line 2
            draw_cursor(surface,s->cursor_pos - 0x40,1,0xff,false);
        } else if(s->cursor_pos >= 0x40 + s->column && s->cursor_pos < 0x40 + s->column * 2) { // line 4
            draw_cursor(surface,s->cursor_pos - 0x40 - s->column,3,0xff,false);
        } else if(s->cursor_pos <= s->column) {  //line 1
            draw_cursor(surface,s->cursor_pos,0,0xff,false);
        } else if(s->cursor_pos <= s->column * 2) {  //line 3
            draw_cursor(surface,s->cursor_pos - s->column,2,0xff,false);
        }
    }
//    if(last_column < 0x10)
//        draw_cursor(surface,last_column,last_line, 0xff);
    printf("s->width = %u , s->height = %u", s->width, s->height);
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
    s->column = 8;
    s->line   = 1;
    s->width  = set_width(8);
    s->height = set_height(1);
    s->con = graphic_console_init(dev, 0, &hd44780_led_ops, s);
}

static void hd44780_set_column(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    hd44780_state *s = HD44780(obj);
    Error *local_err = NULL;
    uint8_t column;

    visit_type_uint8(v, name, &column, &local_err);
//    if (local_err) {
//        error_propagate(errp, local_err);
//        return;
//    }
    s->width = set_width(column);
    s->column = column;
    qemu_console_resize(s->con, s->width, s->height);
}
static void hd44780_set_line(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    hd44780_state *s = HD44780(obj);
    Error *local_err = NULL;
    uint8_t line;

    visit_type_uint8(v, name, &line, &local_err);
//    if (local_err) {
//        error_propagate(errp, local_err);
//        return;
//    }
    s->height = set_height(line);
    s->line = line;
    qemu_console_resize(s->con, s->width, s->height);
}

static void hd44780_initfn(Object *obj)
{
    object_property_add(obj, "column", "uint8_t",
                        NULL, hd44780_set_column,
                        NULL, NULL, NULL);
    object_property_add(obj, "line", "uint8_t",
                        NULL, hd44780_set_line,
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
