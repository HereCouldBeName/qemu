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

#include "qemu/osdep.h"
#include "hw/boards.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "exec/address-spaces.h"
#include "hw/avr/atmega8_soc.h"
#include "cpu.h"
#include "sysemu/sysemu.h"

#define VIRT_BASE_FLASH 0x00000000
#define VIRT_BASE_ISRAM 0x00000100
#define VIRT_BASE_EXMEM 0x00001100
#define VIRT_BASE_EEPROM 0x00000000

#define SIZE_FLASH 0x00020000
#define SIZE_ISRAM 0x00001000
#define SIZE_EXMEM 0x00010000
#define SIZE_EEPROM 0x00001000
#define SIZE_IOREG SIZE_REGS

#define PHYS_BASE_FLASH (PHYS_BASE_CODE)

#define PHYS_BASE_ISRAM (PHYS_BASE_DATA)
#define PHYS_BASE_EXMEM (PHYS_BASE_ISRAM + SIZE_ISRAM)
#define PHYS_BASE_EEPROM (PHYS_BASE_EXMEM + SIZE_EXMEM)

#define PHYS_BASE_IOREG (PHYS_BASE_REGS + 0x20)

#define FLASH_BASE_ADDRESS 0x08000000
#define FLASH_SIZE (1024 * 1024)
#define SRAM_BASE_ADDRESS 0x20000000
#define SRAM_SIZE (128 * 1024)

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

DeviceState *add_device_to_bus(DeviceState *dev, const char *name, uint8_t addr)
{
    Atmega8State *s = Atmega8_SOC(dev);
    return i2c_create_slave(s->twi.bus, "avr_hd44780", 0x27);
}

static uint64_t atmega8_ioreg_read(void *opaque, hwaddr addr,
                                       unsigned int size)
{
    //qemu_log("READ addres need is 0x%lx\n", addr);
    Atmega8State *s = opaque;
    switch (addr) {
    case UCSRA:
    case UDR:
    case UCSRB:
    case UCSRC:
    case UBRRL:
        return avr_usart_read(&s->usart, addr, size);
    case TWBR:
    case TWCR:
    case TWDR:
        s->usart.switch_reg = false;
        return atmega8_twi_read(&s->twi, addr, size);
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        s->usart.switch_reg = false;
        return 0;
    }
}

static void atmega8_ioreg_write(void *opaque, hwaddr addr,
                                  uint64_t val64, unsigned int size)
{
    //qemu_log("WRITE addres need is 0x%lx\n", addr);
    Atmega8State *s = opaque;
    s->usart.switch_reg = false;
    switch (addr) {
    case UCSRA:
    case UDR:
    case UCSRB:
    case UCSRC:
    case UBRRL:
        avr_usart_write(&s->usart, addr, val64, size);
        break;
    case TWBR:
    case TWCR:
    case TWDR:
        atmega8_twi_write(&s->twi, addr, val64, size);
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: Bad offset 0x%"HWADDR_PRIx"\n", __func__, addr);
        break;
    }
}


static const MemoryRegionOps atmega8_ioregs_ops = {
    .read = atmega8_ioreg_read,
    .write = atmega8_ioreg_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void atmega8_soc_initfn(Object *obj)
{
    Atmega8State *s = Atmega8_SOC(obj);
    object_initialize(&s->usart, sizeof(s->usart),
                          TYPE_AVR_USART);
    object_initialize(&s->twi, sizeof(s->twi),
                          TYPE_ATMEGA8_TWI);
    qdev_set_parent_bus(DEVICE(&s->usart), sysbus_get_default());
    qdev_set_parent_bus(DEVICE(&s->twi), sysbus_get_default());

}

static void atmega8_soc_realize(DeviceState *dev_soc, Error **errp)
{
    Atmega8State *s = Atmega8_SOC(dev_soc);
    DeviceState *dev;
    Error *err = NULL;

    MemoryRegion *address_space_mem;
    unsigned ram_size = SIZE_ISRAM + SIZE_EXMEM;

    address_space_mem = get_system_memory();
    s->ram = g_new(MemoryRegion, 1);
    s->flash = g_new(MemoryRegion, 1);
    s->io = g_new(MemoryRegion, 1);
    
    AVR_CPU(cpu_create(AVR_CPU_TYPE_NAME("avr4")));

    memory_region_allocate_system_memory(s->ram, NULL, "avr.ram", ram_size);
    memory_region_add_subregion(address_space_mem, PHYS_BASE_ISRAM, s->ram);

    memory_region_init_rom(s->flash, NULL, "avr.flash", SIZE_FLASH, &error_fatal);
    memory_region_add_subregion(address_space_mem, PHYS_BASE_FLASH, s->flash);
    
    memory_region_init_io(s->io, NULL, &atmega8_ioregs_ops, s,
                          "atmega8-ioregs", 0x400);
    memory_region_add_subregion(address_space_mem, PHYS_BASE_REGS + AVR_CPU_IO_REGS_BASE, s->io);
   
    /* Attach UART (uses USART registers) and USART controllers */
    dev = DEVICE(&(s->usart));
    qdev_prop_set_chr(dev, "chardev", serial_hd(0));
    object_property_set_bool(OBJECT(&s->usart), true, "realized", &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }
    object_property_set_bool(OBJECT(&s->twi), true, "realized", &err);
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }
}

static Property atmega8_soc_properties[] = {
    DEFINE_PROP_STRING("cpu-type", Atmega8State, cpu_type),
    DEFINE_PROP_END_OF_LIST(),
};

static void atmega8_soc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = atmega8_soc_realize;
    dc->props = atmega8_soc_properties;
}

static const TypeInfo atmega8_soc_info = {
    .name          = TYPE_Atmega8_SOC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Atmega8State),
    .instance_init = atmega8_soc_initfn,
    .class_init    = atmega8_soc_class_init,
};

static void atmega8_soc_types(void)
{
    type_register_static(&atmega8_soc_info);
}

type_init(atmega8_soc_types)
