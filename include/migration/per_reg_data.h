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

#include "migration/user_path.h"

CurrPosDebug* vmsd_data(fprintf_function func_fprintf, void *f, const char* name, CurrPosDebug* cpd);
void vmsd_test(fprintf_function func_fprintf, void *f, const char* name, CurrPosDebug* cpd);

#endif
