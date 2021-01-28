/*
 * ATMEGA8A TWI controller emulation
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
#include "hw/i2c/atmega8_twi.h"


#define TWI_ERR_DEBUG 0

#define DB_PRINT(...) do { \
        if (TWI_ERR_DEBUG) { \
            printf(__VA_ARGS__); \
        } \
    } while (0)


/*static inline void exynos4210_i2c_raise_interrupt(Exynos4210I2CState *s)
{
    if (s->i2ccon & I2CCON_INTRS_EN) {
        s->i2ccon |= I2CCON_INT_PEND;
        qemu_irq_raise(s->irq);
    }
}*/

/*static void atmega8_twi_data_send(Atmega8TWIState *s)
{

}*/

uint64_t atmega8_twi_read(Atmega8TWIState *s, hwaddr addr,
                                 unsigned size)
{
    switch (addr) {
    case TWBR:
        return s->twbr;
    case TWCR:
        return s->twcr;
    case TWDR:
        return s->twdr;
        s->scl_free = true;
        /* not need for display */
        break;
    default:
        printf("ERROR: Bad read offset 0x%x\n", (unsigned int)addr);
        break;
    }
    return 0;
}

void atmega8_twi_write(Atmega8TWIState *s, hwaddr addr,
                              uint64_t value, unsigned size)
{
    //uint8_t v = value & 0xff;
    switch (addr) {
    case TWBR:
        s->twbr = value;
        break;
    case TWCR:
        s->twcr = value;
        if (s->twcr & TWEN) { /* TWI block is on */
            if (s->twcr & TWINT) {
                if(s->twcr & TWSTA) {
                    s->start = true;
                    //qemu_log("START\n");
                }
                else if(s->twcr & TWSTO) {
                    s->start = false;
                    i2c_end_transfer(s->bus);
                    s->sending = false;
                    DB_PRINT("STOP\n");
                }
            }
        }
        break;
    case TWDR:
        s->twdr = value;
        if(s->start) {
            if(i2c_start_transfer(s->bus, (s->twdr)>>1, (s->twdr) & 0x1)) {
                DB_PRINT("ERROR start transfer\n");
            }
            else {
                s->sending = true;
            }
            s->start = false;
        }
        else if(s->sending) {
            i2c_send(s->bus, s->twdr);
            //printf("SEND DATE TWI %x\n", s->twdr);
        }
        //qemu_log("VALUE WRITE !!! %lx\n TWCR IS: ", value);
        break;
    default:
        printf("ERROR: Bad write offset 0x%x\n", (unsigned int)addr);
        break;
    }
}

static const VMStateDescription atmega8_twi_vmstate = {
    .name = "avr_twi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT8(twbr, Atmega8TWIState),
        VMSTATE_UINT8(twcr, Atmega8TWIState),
        VMSTATE_UINT8(twdr, Atmega8TWIState),

        VMSTATE_BOOL(scl_free, Atmega8TWIState),
        VMSTATE_BOOL(start, Atmega8TWIState),
        VMSTATE_BOOL(sending, Atmega8TWIState),
        VMSTATE_END_OF_LIST()
    }
};

static void atmega8_twi_reset(DeviceState *d)
{
    Atmega8TWIState *s = ATMEGA8_TWI(d);

    s->twbr     = 0x00;
    s->twcr     = 0x00;
    s->twdr     = 0xFF;
    s->start    = false;
    s->scl_free = true;
    s->sending  = false;
}

static void atmega8_twi_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    Atmega8TWIState *s = ATMEGA8_TWI(obj);
    s->bus = i2c_init_bus(dev, "twi");
}

static void atmega8_twi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &atmega8_twi_vmstate;
    dc->reset = atmega8_twi_reset;
}

static const TypeInfo atmega8_twi_type_info = {
    .name = TYPE_ATMEGA8_TWI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Atmega8TWIState),
    .instance_init = atmega8_twi_init,
    .class_init = atmega8_twi_class_init,
};

static void atmega8_twi_register_types(void)
{
    type_register_static(&atmega8_twi_type_info);
}

type_init(atmega8_twi_register_types)
