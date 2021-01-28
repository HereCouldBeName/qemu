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

#include "qemu/osdep.h"
#include "hw/char/usart_avr.h"
#include "qemu/log.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"

static int avr_usart_can_receive(void *opaque)
{
    // AvrUsartState *s = opaque;
    // if (!(s->ucsra & UCSRA_RXC)) {
    //     return 1;
    // }
    // return 0;
    return 1;
}

static void avr_usart_receive(void *opaque, const uint8_t *buf, int size)
{
    AvrUsartState *s = opaque;
    if (!(s->ucsrb & UCSRB_RXEN)) {
        /* USART not enabled - drop the chars */
        return;
    }

    s->udr = *buf;
    s->ucsra |= UCSRA_RXC;

    if (s->ucsrb & UCSRB_TXCIE) {
        qemu_set_irq(s->irq, 1);
    }
    //printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
}

static void avr_usart_reset(DeviceState *dev)
{
    AvrUsartState *s = AVR_USART(dev);
	
    s->udr   = 0x0;
    s->ucsra = 0x20;
    s->ucsrb = 0x0;
    s->ucsrc = 0x86;
    s->ubrrh = 0x0;
    s->ubrrl = 0x0;
    s->switch_reg = false;

    qemu_set_irq(s->irq, 0);
}

uint64_t avr_usart_read(AvrUsartState *s, hwaddr addr,
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

void avr_usart_write(AvrUsartState *s, hwaddr addr,
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
        printf("UDR REG SEND IRQ\n");
        const uint8_t* buf = (const uint8_t*)("UDR REG SEND IRQ");
        vm_stop_irq(buf);

        //vm_stop(RUN_STATE_PAUSED);

        if (value < 0xF000) {
            ch = value;
            /* XXX this blocks entire thread. Rewrite to use
             * qemu_chr_fe_write and background I/O callbacks */
            qemu_chr_fe_write_all(&s->chr, &ch, size);
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

static const VMStateDescription vmstate_avr_usart = {
    .name = "avr-usart",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(udr, AvrUsartState),
        VMSTATE_UINT32(ucsra, AvrUsartState),
        VMSTATE_UINT32(ucsrc, AvrUsartState),
        VMSTATE_UINT32(ubrrh, AvrUsartState),
        VMSTATE_UINT32(ubrrl, AvrUsartState),

        VMSTATE_BOOL(switch_reg, AvrUsartState),

        VMSTATE_END_OF_LIST()
    }
};


static Property avr_usart_properties[] = {
    DEFINE_PROP_CHR("chardev", AvrUsartState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void avr_usart_realize(DeviceState *dev, Error **errp)
{
    AvrUsartState *s = AVR_USART(dev);

    qemu_chr_fe_set_handlers(&s->chr, avr_usart_can_receive,
                             avr_usart_receive, NULL, NULL,
                             s, NULL, true);
}

static void avr_usart_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->reset = avr_usart_reset;
    dc->vmsd = &vmstate_avr_usart;
    dc->props = avr_usart_properties;
    dc->realize = avr_usart_realize;
}

static const TypeInfo avr_usart_info = {
    .name          = TYPE_AVR_USART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AvrUsartState),
    .class_init    = avr_usart_class_init,
};

static void avr_usart_register_types(void)
{
    type_register_static(&avr_usart_info);
}

type_init(avr_usart_register_types)
