/*
 * QEMU device register information
 *
 * Copyright (c) 2019 HereCouldBeName
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "migration/user_path.h"

/*
    *cpd - pointer to current cpd
    *vmsd - vmsd to future cpd
    *field - field to future cpd
    *opaque - opaque to future cpd
    *name - name to future cpd. Needed to output the path

    *Function returns pointer to the new cpd
*/

CurrPosDebug* create_next_cpd(CurrPosDebug* cpd, const VMStateDescription *vmsd, VMStateField *field,
                            void* opaque, const char* name) {
    
    if(cpd->next && !strcmp(cpd->next->name, name)) {
        return cpd->next;
    }

    CurrPosDebug* tmp;
    tmp = malloc(sizeof(CurrPosDebug));
    tmp->field = field;
    tmp->opaque = opaque;
    tmp->vmsd = vmsd;
    tmp->last = cpd;
    tmp->next = NULL;

    size_t len = strlen(name);
    tmp->name = malloc((len + 1) * sizeof(const char));
    memcpy(tmp->name,name,len * sizeof(const char));
    tmp->name[len] = '\0';

    cpd->next = tmp;

    return tmp;
}
