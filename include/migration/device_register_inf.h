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
void show_reg_peref(fprintf_function func_fprintf, void *f, const char* name);
//void show_per_reg_by_name(fprintf_function func_fprintf, void *f, UserPath *up);
CurrPosDebug* test_reg(fprintf_function func_fprintf, void *f, const char* name, CurrPosDebug* cpd);
#endif
