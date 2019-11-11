/*
 * controller emulation TWI controller emulation
 *
 * Copyright (c) 2019 HereCouldBeName <>
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

#ifndef Atmega8_TWI_H
#define Atmega8_TWI_H

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/sysbus.h"
#include "hw/i2c/i2c.h"
#include "qemu/log.h"

#define TYPE_ATMEGA8_TWI                  "atmega8.twi"          //TYPE_EXYNOS4_I2C
#define ATMEGA8_TWI(obj)                  \
    OBJECT_CHECK(Atmega8TWIState, (obj), TYPE_ATMEGA8_TWI)

#define TWBR 0x00
#define TWCR 0x36
#define TWDR 0x03

/*TWCR Register*/
#define TWINT                      (1 << 7) /* Interrupt Flag */
#define TWEA                       (1 << 6) /* Enable Acknowledge Bit */
#define TWSTA                      (1 << 5) /* START Bit */
#define TWSTO                      (1 << 4) /* STOP Bit */
#define TWEN                       (1 << 2) /* Enable Bit */

#define TWI_START_CONDITION(reg)   (reg & TWCR) && (reg & TWINT) && (reg & TWSTA) && (reg & TWEN)

typedef struct Atmega8TWIState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    I2CBus *bus;
    //qemu_irq irq;

    uint8_t twbr;
    uint8_t twcr;
    uint8_t twdr;
    bool start;
    bool sending;
    bool scl_free;
} Atmega8TWIState;

uint64_t atmega8_twi_read(Atmega8TWIState *s, hwaddr addr,
                                 unsigned size);
void atmega8_twi_write(Atmega8TWIState *s, hwaddr addr,
                              uint64_t value, unsigned size);
#endif
