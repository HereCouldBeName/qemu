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

typedef struct {
    int ind;
        /*
        test value:
        500 - steps max
        30 - max len one field
    */
    char* steps[500];
} UserPath;


typedef struct CurrPosDebug {
    const VMStateDescription * vmsd;
    VMStateField* field;
    void* opaque;
    struct CurrPosDebug* prev;
} CurrPosDebug;



#endif
