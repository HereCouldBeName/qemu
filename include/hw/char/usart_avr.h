/*
 * ATMEGA8A USART
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

#ifndef HW_Atmega8a_USART_H
#define HW_Atmega8a_USART_H

#include "hw/sysbus.h"
#include "chardev/char-fe.h"
#include "hw/hw.h"

#define UDR 0x0C
#define UCSRA 0x0B
#define UCSRB 0x0A
#define UCSRC 0x20
#define UBRRL 0x09

#define UCSRA_RXC   (1 << 7)
#define UCSRA_UDRE  (1 << 5)
#define UCSRB_RXCIE (1 << 7)
#define UCSRB_TXCIE (1 << 6)
#define UCSRB_RXEN  (1 << 4)

#define URSEL       (1 << 7)

#define TYPE_Atmega8_USART "atmega8-usart"
#define Atmega8_USART(obj) \
    OBJECT_CHECK(Atmega8UsartState, (obj), TYPE_Atmega8_USART)
typedef struct {
    /* <private> */
    SysBusDevice parent_obj;

    /* <public> */
    MemoryRegion mmio;

    uint32_t udr;
    uint32_t ucsra;
    uint32_t ucsrb;
    uint32_t ucsrc;
    uint32_t ubrrh;
    uint32_t ubrrl;

    bool switch_reg;

    CharBackend chr;
    qemu_irq irq;
} Atmega8UsartState;

uint64_t atmega8_usart_read(Atmega8UsartState *s, hwaddr addr,
                                       unsigned int size);
void atmega8_usart_write(Atmega8UsartState *s, hwaddr addr,
                                  uint64_t val64, unsigned int size);

#endif /* HW_Atmega8a_USART_H */
