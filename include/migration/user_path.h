/*
 * QEMU device register information
 *
 * Copyright (c) 2019 HereCouldBeName
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#ifndef QEMU_USER_PATH_H
#define QEMU_USER_PATH_H

#include "qemu/osdep.h"
#include "migration/migration.h"

typedef struct CurrPosDebug {
    char* name;
    const VMStateDescription * vmsd;
    VMStateField* field;
    void* opaque;
    struct CurrPosDebug* last;
    struct CurrPosDebug* next;
    bool is_array;
    bool is_qlist;
} CurrPosDebug;

CurrPosDebug* create_next_cpd(CurrPosDebug* cpd, const VMStateDescription *vmsd,
                            VMStateField *field, void* opaque, const char* name);

CurrPosDebug* create_next_cpd_array(CurrPosDebug* cpd, const VMStateDescription *vmsd,
                            VMStateField *field, void* opaque, const char* name);

CurrPosDebug* create_next_cpd_qlist(CurrPosDebug* cpd, const VMStateDescription *vmsd,
                            VMStateField *field, void* opaque, const char* name);

#endif
