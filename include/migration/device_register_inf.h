/*
 * QEMU device register information
 *
 * Copyright (c) 2019 HereCouldBeName
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_DEVICE_REGISTER_INFO_H
#define QEMU_DEVICE_REGISTER_INFO_H

#include "migration/user_path.h"

void find_device(fprintf_function func_fprintf, void *f);
CurrPosDebug* show_per_reg(fprintf_function func_fprintf, void *f, const char* name, CurrPosDebug* cpd);
void per_find_device(fprintf_function func_fprintf, void *f, const char* path);
#endif
