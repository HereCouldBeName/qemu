/*
 * ATMEGA8A SoC
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

#ifndef HW_AVR_Atmega8_SOC_H
#define HW_AVR_Atmega8_SOC_H

#include "hw/char/usart_avr.h"
#include "hw/i2c/atmega8_twi.h"

#define TYPE_Atmega8_SOC "atmega8-soc"
#define Atmega8_SOC(obj) \
    OBJECT_CHECK(Atmega8State, (obj), TYPE_Atmega8_SOC)

typedef struct Atmega8State {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    char *cpu_type;

    MemoryRegion *ram;
    MemoryRegion *flash;
    MemoryRegion *io;

    AvrUsartState usart;
    Atmega8TWIState twi;

 } Atmega8State;

#endif
