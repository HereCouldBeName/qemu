/*
 * QEMU device register information
 *
 * Copyright (c) 2019 HereCouldBeName
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_PER_REG_DATA_H
#define QEMU_PER_REG_DATA_H


void vmsd_data(fprintf_function func_fprintf, void *f, const char* path, const VMStateDescription *vmsd, void *opaque);

const char* get_name(const char** path);

#endif
