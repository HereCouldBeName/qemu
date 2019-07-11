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

#define PHYS_BASE_REGS 0x10000000
#define AVR_CPU_REGS_BASE 0x0000
#define AVR_CPU_REGS 0x0020
#define AVR_CPU_IO_REGS_BASE (AVR_CPU_REGS_BASE + AVR_CPU_REGS)

#include "qemu/osdep.h"
#include "hw/char/usart_avr.h"
#include "qemu/log.h"
#include "exec/address-spaces.h"

static int atmega8_usart_can_receive(void *opaque)
{
    Atmega8UsartState *s = opaque;
    if (!(s->ucsra & UCSRA_RXC)) {
        return 1;
    }
    return 0;
}

static void atmega8_usart_receive(void *opaque, const uint8_t *buf, int size)
{
    Atmega8UsartState *s = opaque;
    if (!(s->ucsrb & UCSRB_RXEN)) {
        /* USART not enabled - drop the chars */
        return;
    }

    s->udr = *buf;
    s->ucsra |= UCSRA_RXC;

    if (s->ucsrb & UCSRB_TXCIE) {
        qemu_set_irq(s->irq, 1);
    }
}

static void atmega8_usart_reset(DeviceState *dev)
{
    Atmega8UsartState *s = Atmega8_USART(dev);
	
    s->udr   = 0x0;
    s->ucsra = 0x20;
    s->ucsrb = 0x0;
    s->ucsrc = 0x86;
    s->ubrrh = 0x0;
    s->ubrrl = 0x0;
    s->switch_reg = false;

    qemu_set_irq(s->irq, 0);
}

uint64_t atmega8_usart_read(Atmega8UsartState *s, hwaddr addr,
                                       unsigned int size)
{
    switch (addr) {
    case UCSRA:
        qemu_chr_fe_accept_input(&s->chr);
        s->switch_reg = false;
        return s->ucsra;
    case UDR:
        s->ucsra &= ~UCSRA_RXC;
        qemu_chr_fe_accept_input(&s->chr);
        qemu_set_irq(s->irq, 0);
        s->switch_reg = false;
        return s->udr & 0x3FF;
    case UCSRB:
        s->switch_reg = false;
        return s->ucsrb;
    case UCSRC:
        if (s->switch_reg) {
            return s->ucsrc;
        } else {
            s->switch_reg = true;
            return s->ubrrh;
        };
    case UBRRL:
        s->switch_reg = false;
        return s->ubrrl;
    }

    return 0;
}

void atmega8_usart_write(Atmega8UsartState *s, hwaddr addr,
                                  uint64_t val64, unsigned int size)
{
    uint32_t value = val64;
    unsigned char ch;
    //DB_PRINT("Write 0x%" PRIx32 ", 0x%"HWADDR_PRIx"\n", value, addr);
    switch (addr) {
    case UCSRA:
        if (value <= 0x3FF) {
            /* I/O being synchronous, TXE is always set. In addition, it may
               only be set by hardware, so keep it set here. */
            s->ucsra = value | UCSRA_UDRE;
        } else {
            s->ucsra &= value;
        }
        if (!(s->ucsra & UCSRA_RXC)) {
            qemu_set_irq(s->irq, 0);
        }
        return;
    case UDR:
        if (value < 0xF000) {
            ch = value;
            /* XXX this blocks entire thread. Rewrite to use
             * qemu_chr_fe_write and background I/O callbacks */
            qemu_chr_fe_write_all(&s->chr, &ch, 1);
            s->ucsra |= UCSRA_RXC;
        }
        return;
    case UCSRB:
        s->ucsrb = value;
        if(s->ucsra & UCSRB_RXCIE && s->ucsra & UCSRA_RXC) {
            qemu_set_irq(s->irq, 1);
        }
        return;
    case UCSRC:
        if (val64 & URSEL) {
            s->ucsrc = value;
        } else {
            s->ubrrh = value;
        }
        return;
    case UBRRL:
        s->ubrrl = value;
        return;
    }
}

static Property atmega8_usart_properties[] = {
    DEFINE_PROP_CHR("chardev", Atmega8UsartState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void atmega8_usart_realize(DeviceState *dev, Error **errp)
{
    Atmega8UsartState *s = Atmega8_USART(dev);

    qemu_chr_fe_set_handlers(&s->chr, atmega8_usart_can_receive,
                             atmega8_usart_receive, NULL, NULL,
                             s, NULL, true);
}

static void atmega8_usart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = atmega8_usart_reset;
    dc->props = atmega8_usart_properties;
    dc->realize = atmega8_usart_realize;
}

static const TypeInfo atmega8_usart_info = {
    .name          = TYPE_Atmega8_USART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Atmega8UsartState),
    .class_init    = atmega8_usart_class_init,
};

static void atmega8_usart_register_types(void)
{
    type_register_static(&atmega8_usart_info);
}

type_init(atmega8_usart_register_types)
