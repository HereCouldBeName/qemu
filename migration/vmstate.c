/*
 * VMState interpreter
 *
 * Copyright (c) 2009-2017 Red Hat Inc
 *
 * Authors:
 *  Juan Quintela <quintela@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "migration.h"
#include "migration/vmstate.h"
#include "savevm.h"
#include "qemu-file.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "trace.h"
#include "qjson.h"

#include "migration/per_reg_data.h"

static int vmstate_subsection_save(QEMUFile *f, const VMStateDescription *vmsd,
                                   void *opaque, QJSON *vmdesc);
static int vmstate_subsection_load(QEMUFile *f, const VMStateDescription *vmsd,
                                   void *opaque);

static int vmstate_n_elems(void *opaque, VMStateField *field)
{
    int n_elems = 1;

    if (field->flags & VMS_ARRAY) {
        n_elems = field->num;
    } else if (field->flags & VMS_VARRAY_INT32) {
        n_elems = *(int32_t *)(opaque+field->num_offset);
    } else if (field->flags & VMS_VARRAY_UINT32) {
        n_elems = *(uint32_t *)(opaque+field->num_offset);
    } else if (field->flags & VMS_VARRAY_UINT16) {
        n_elems = *(uint16_t *)(opaque+field->num_offset);
    } else if (field->flags & VMS_VARRAY_UINT8) {
        n_elems = *(uint8_t *)(opaque+field->num_offset);
    }

    if (field->flags & VMS_MULTIPLY_ELEMENTS) {
        n_elems *= field->num;
    }

    trace_vmstate_n_elems(field->name, n_elems);
    return n_elems;
}

static int vmstate_size(void *opaque, VMStateField *field)
{
    int size = field->size;

    if (field->flags & VMS_VBUFFER) {
        size = *(int32_t *)(opaque+field->size_offset);
        if (field->flags & VMS_MULTIPLY) {
            size *= field->size;
        }
    }

    return size;
}

static void vmstate_handle_alloc(void *ptr, VMStateField *field, void *opaque)
{
    if (field->flags & VMS_POINTER && field->flags & VMS_ALLOC) {
        gsize size = vmstate_size(opaque, field);
        size *= vmstate_n_elems(opaque, field);
        if (size) {
            *(void **)ptr = g_malloc(size);
        }
    }
}

int vmstate_load_state(QEMUFile *f, const VMStateDescription *vmsd,
                       void *opaque, int version_id)
{
    VMStateField *field = vmsd->fields;
    int ret = 0;

    trace_vmstate_load_state(vmsd->name, version_id);
    if (version_id > vmsd->version_id) {
        error_report("%s: incoming version_id %d is too new "
                     "for local version_id %d",
                     vmsd->name, version_id, vmsd->version_id);
        trace_vmstate_load_state_end(vmsd->name, "too new", -EINVAL);
        return -EINVAL;
    }
    if  (version_id < vmsd->minimum_version_id) {
        if (vmsd->load_state_old &&
            version_id >= vmsd->minimum_version_id_old) {
            ret = vmsd->load_state_old(f, opaque, version_id);
            trace_vmstate_load_state_end(vmsd->name, "old path", ret);
            return ret;
        }
        error_report("%s: incoming version_id %d is too old "
                     "for local minimum version_id  %d",
                     vmsd->name, version_id, vmsd->minimum_version_id);
        trace_vmstate_load_state_end(vmsd->name, "too old", -EINVAL);
        return -EINVAL;
    }
    if (vmsd->pre_load) {
        int ret = vmsd->pre_load(opaque);
        if (ret) {
            return ret;
        }
    }
    while (field->name) {
        trace_vmstate_load_state_field(vmsd->name, field->name);
        if ((field->field_exists &&
             field->field_exists(opaque, version_id)) ||
            (!field->field_exists &&
             field->version_id <= version_id)) {
            void *first_elem = opaque + field->offset;
            int i, n_elems = vmstate_n_elems(opaque, field);
            int size = vmstate_size(opaque, field);

            vmstate_handle_alloc(first_elem, field, opaque);
            if (field->flags & VMS_POINTER) {
                first_elem = *(void **)first_elem;
                assert(first_elem || !n_elems || !size);
            }
            for (i = 0; i < n_elems; i++) {
                void *curr_elem = first_elem + size * i;

                if (field->flags & VMS_ARRAY_OF_POINTER) {
                    curr_elem = *(void **)curr_elem;
                }
                if (!curr_elem && size) {
                    /* if null pointer check placeholder and do not follow */
                    assert(field->flags & VMS_ARRAY_OF_POINTER);
                    ret = vmstate_info_nullptr.get(f, curr_elem, size, NULL);
                } else if (field->flags & VMS_STRUCT) {
                    ret = vmstate_load_state(f, field->vmsd, curr_elem,
                                             field->vmsd->version_id);
                } else if (field->flags & VMS_VSTRUCT) {
                    ret = vmstate_load_state(f, field->vmsd, curr_elem,
                                             field->struct_version_id);
                } else {
                    ret = field->info->get(f, curr_elem, size, field);
                }
                if (ret >= 0) {
                    ret = qemu_file_get_error(f);
                }
                if (ret < 0) {
                    qemu_file_set_error(f, ret);
                    error_report("Failed to load %s:%s", vmsd->name,
                                 field->name);
                    trace_vmstate_load_field_error(field->name, ret);
                    return ret;
                }
            }
        } else if (field->flags & VMS_MUST_EXIST) {
            error_report("Input validation failed: %s/%s",
                         vmsd->name, field->name);
            return -1;
        }
        field++;
    }
    ret = vmstate_subsection_load(f, vmsd, opaque);
    if (ret != 0) {
        return ret;
    }
    if (vmsd->post_load) {
        ret = vmsd->post_load(opaque, version_id);
    }
    trace_vmstate_load_state_end(vmsd->name, "end", ret);
    return ret;
}


#define BITS_TO_U64S(nr) DIV_ROUND_UP(nr, 64)

static void show_help_msg(fprintf_function func_fprintf, void *f, const char* name, int size)
{
    func_fprintf(f, "\nIf you want to see a concret element"
                 " you need to entered %s[i], where i = {0...%i}\n",name, size-1);
}

#define INDERROR -1
#define INDEMPTY -2

static int per_get_ind_name(const char** name)
{
    int size = strlen(*name);
    int lcopy = 0;
    int ind = INDEMPTY;

    for (int i = 0; i < size; i++) {
        if ((*name)[i] == '[') {
            lcopy = i;

            if ((*name)[size-1] != ']') {
                return INDERROR;
            }

            ind = 0;
            int b_ind = i+1;
            int len = size-2;
            for (int j = len; j >= i+1; j--) {
                ind *= 10;
                if (!isdigit((*name)[b_ind + len - j])) {
                    return INDERROR;
                }
                ind += ((*name)[b_ind + len - j] - '0');
            }

            char* name_f = (char*)malloc(lcopy + 1);
            strncpy(name_f, *name, lcopy);
            name_f[lcopy] = '\0';
            *name = name_f;

            break;
        }
    }
    return ind;
}

/*struct*/

static void per_printf_data_struct(fprintf_function func_fprintf, void *f,
                                   VMStateField *field, void *opaque,
                                   const char * name, bool hex)
{
    if (!strcmp(field->name, name)) {
        get_name(&name);
    }
    const VMStateDescription *vmsd = field->vmsd;
    field = vmsd->fields;
    vmsd_data(func_fprintf, f, name, vmsd, opaque, hex);
    return;
}

static void per_printf_struct(fprintf_function func_fprintf, void *f, VMStateField *field,
                              void *opaque, const char * name, int n_elems, bool hex)
{
    const char* type;
    if(field->flags & VMS_STRUCT) {
        type = "Struct";
    } else {
        type = "VStruct";
    }
    if (name) {
        if (n_elems > 1) {
            for (int i = 0; i < n_elems; i++) {
                func_fprintf(f, "- <%s el> %s[%i]\n", type, field->name, i);
            }
        } else {
            per_printf_data_struct(func_fprintf, f, field, opaque, name, hex);
        }
    } else {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array %s> %s\n", type, field->name);
        } else {
            func_fprintf(f, "- <%s> %s\n", type, field->name);
        }
    }
    return;                                
}

/*VMS POINTER*/

static void* per_printf_data_pointer(void* opaque) 
{
    opaque = *(void **)opaque;
    assert(opaque);
    return opaque;
}

static void* per_printf_pointer(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field,
                            const char* name) 
{
    if (name) {
        return per_printf_data_pointer(opaque);
    } else if (!field->info) {
        func_fprintf(f, "- <VMS POINTER> %s\n", field->name);
    }
    return opaque;
}

/*VMS array of pointer*/

static void* per_printf_arr_pointer(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field,
                            const char* name, int n_elems)
{
    if (name) {
        if (n_elems > 1 && !field->info) {
            for (int i = 0; i < n_elems; i++) {
                func_fprintf(f, "- <VMS array of pointer el> %s[%i]\n", field->name, i);
            }
        } else {
            return per_printf_data_pointer(opaque);
        }
    } else if (!field->info) {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array VMS array of pointer> %s\n", field->name);
        } else {
            func_fprintf(f, "- <VMS array of pointer> %s\n", field->name);
        }
    }
    return opaque;
}

/*int, float, str*/

static char* per_printf_data_value(void* opaque, VMStateField *field,
                                  bool hex, bool sign)
{
    int64_t val = 0;
    uint64_t uval = 0;

    char* buff;

    if (sign && !strcmp(field->info->name, "int8")) { 
        val = (int64_t)*(int8_t *)opaque;
    } else if (!sign && !strcmp(field->info->name, "bool")) {
        uval = (uint64_t)*(bool *)opaque;
    } else if (sign && !strcmp(field->info->name, "int16")) {
        val = (int64_t)*(int16_t *)opaque;
    } else if (sign && ((!strcmp(field->info->name, "int32")) ||
               (!strcmp(field->info->name, "int32 le")) ||
               (!strcmp(field->info->name, "int32 equal")))) {
        val = (int64_t)*(int32_t *)opaque;
    } else if (sign && !strcmp(field->info->name, "int64")) {
        val = *(int64_t *)opaque;
    } else if (!sign && ((!strcmp(field->info->name, "uint8")) ||
               (!strcmp(field->info->name, "uint8 equal")))) {
        uval = (uint64_t)*(uint8_t *)opaque;          
    } else if (!sign && ((!strcmp(field->info->name, "uint16")) ||
               (!strcmp(field->info->name, "uint16 equal")))) {
        uval = (uint64_t)*(uint16_t *)opaque;              
    } else if (!sign && ((!strcmp(field->info->name, "uint32")) ||
               (!strcmp(field->info->name, "uint32 equal")))) {
        uval = (uint64_t)*(uint32_t *)opaque;                  
    } else if (!sign && ((!strcmp(field->info->name, "uint64")) ||
               (!strcmp(field->info->name, "uint64 equal")))) {
        uval = *(uint64_t *)opaque;          
    } else if (!strcmp(field->info->name, "float64")) {
        if (!asprintf(&buff, "%li", *(float64 *)opaque)) {
            /*ERROR*/
            return NULL;
        }
        return buff;
    } else if (!strcmp(field->info->name, "str")) {
        if (!asprintf(&buff, "%s", (char *)opaque)) {
            /*ERROR*/
            return NULL;
        }
        return buff;
    }

    if (sign) {
        if (hex) {
            if (!asprintf(&buff, "%#lx", val)) {
                /*ERROR*/
                return NULL;
            }
        } else {
            if (!asprintf(&buff, "%li", val)) {
                /*ERROR*/
                return NULL;
            }
        }
    } else {
        if (hex) {
            if (!asprintf(&buff, "%#lx", uval)) {
                /*ERROR*/
                return NULL;
            }
        } else {
            if (!asprintf(&buff, "%li", uval)) {
                /*ERROR*/
                return NULL;
            }
        }
    }
    return buff;
}



static void per_printf_data_basic(fprintf_function func_fprintf, void *f,
                                  void* opaque, VMStateField *field,
                                  bool hex, bool sign)
{
    char* buff = per_printf_data_value(opaque, field, hex, sign);
    if (buff) {
        func_fprintf(f, "- <%s> %s %s\n", field->info->name, field->name, buff);
    } else {
        /*printf error*/
    }
    free(buff);
}


static void per_printf_basic(fprintf_function func_fprintf, void *f, void* opaque,
                             VMStateField *field, const char* name, int n_elems,
                             bool hex, bool sign)
{
    if (n_elems > 1) {
        if (name) {
            int size = vmstate_size(opaque, field);
            for (int i=0; i < n_elems; i++) {        
                char* buff = per_printf_data_value(opaque, field, hex, sign);
                if (buff) {
                    func_fprintf(f, "- <Array %s el> %s[%i] %s\n", field->info->name, field->name, i, buff);
                }
                else {
                    /*printf error*/
                }
                free(buff);
                opaque += size;
            }
        } else {
           func_fprintf(f, "- <Array %s> %s\n", field->info->name, field->name); 
        }
    } else {
        per_printf_data_basic(func_fprintf, f, opaque, field, hex, sign);
    }
    return;
}

static void per_printf_int_equal(fprintf_function func_fprintf, void *f, void* opaque,
                            VMStateField *field, const char* name, int n_elems, bool hex, bool sign)
{   
    if (field->err_hint) {
        func_fprintf(f, "- <%s> %s <ERROR> %s\n", field->info->name, field->name, field->err_hint);
    } else {
        per_printf_basic(func_fprintf, f, opaque, field, name, n_elems, hex, sign);
    }
    return;
}


/*CPU_Double_U, timer*/

static void per_printf_data_CPUDouble_timer(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, bool hex)
{
    if (!strcmp(field->info->name, "CPU_Double_U")) {
        CPU_DoubleU elem = *(CPU_DoubleU *)opaque;
        if (hex) {
            func_fprintf(f, "- <CPU_DoubleU> ld: %ld, lower: %#x, upper: %#x, ll: %#lx\n",
                         elem.d, elem.l.lower,elem.l.upper,elem.ll) ;
        } else {
            func_fprintf(f, "- <CPU_DoubleU> ld: %ld, lower: %i, upper: %i, ll: %li\n",
                         elem.d, elem.l.lower,elem.l.upper,elem.ll) ;
        }

    } else if (!strcmp(field->info->name, "timer")) {
        QEMUTimer elem = *(QEMUTimer *)opaque;
        if (hex) {
            func_fprintf(f, "- <QEMUTimer> expire_time: %#lx, opaque: %p, scale: %#x\n",
                         elem.expire_time, elem.opaque, elem.scale); 
        } else {
            func_fprintf(f, "- <QEMUTimer> expire_time: %li, opaque: %p, scale: %i\n",
                         elem.expire_time, elem.opaque, elem.scale);
        }
                
    }
}

static void per_printf_CPUDouble_timer(fprintf_function func_fprintf, void *f, void* opaque,
                                VMStateField *field, const char* name, int n_elems, bool hex)
{
    if (name) {
        if (n_elems > 1) {
            for (int i = 0; i < n_elems; i++) {
                func_fprintf(f, "- <Array %s el> %s[%i]\n", field->info->name, field->name, i);
            }
        } else {
            per_printf_data_CPUDouble_timer(func_fprintf, f, opaque, field, hex);
        }
    } else {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array %s> %s\n", field->info->name, field->name); 
        } else {
            func_fprintf(f, "- <%s> %s\n", field->info->name, field->name); 
        }
    }
    return;
}

static void per_printf_data_arr_buffer_bitmap(fprintf_function func_fprintf, void *f,
                    void* opaque, VMStateField *field, int size, bool hex)
{
    if (!strcmp(field->info->name, "buffer") ||
        !strcmp(field->info->name, "unused_buffer")) {
        uint8_t *buf = (uint8_t *)opaque;
        if (hex) {
            for (long i = 0; i < size; i++) {
                func_fprintf(f, "%#x ", buf[i]);
            } 
        } else {
            for (long i = 0; i < size; i++) {
                func_fprintf(f, "%i ", buf[i]);
            } 
        }
    } else if(!strcmp(field->info->name,"bitmap")) {
        unsigned long *bmp = (unsigned long *)opaque;
        for(long i = 0; i < size; i++) {
            func_fprintf(f, "%i ", test_bit(i,bmp));
        }
    }

    func_fprintf(f,"\n");
    
    show_help_msg(func_fprintf, f, field->name, size);

    return;
}

static bool check_size(fprintf_function func_fprintf, void *f,
                    int size, int ind, VMStateField *field)
{
    if (ind >= size) {
        func_fprintf(f, "Invalid field index received\n");
        return false;
    }
    if (size < 1) {
        func_fprintf(f, "- <%s> %s is empty\n", field->info->name, field->name);
        return false;
    }
    return true;
}

static void per_printf_data_buffer_bitmap(fprintf_function func_fprintf, void *f,
                                          void* opaque, VMStateField *field,
                                          int ind, bool hex)
{
    if (!strcmp(field->info->name, "buffer") ||
        !strcmp(field->info->name, "unused_buffer")) {
        uint8_t *buf = (uint8_t *)opaque;
        if (hex) {
            func_fprintf(f, "<uint8_t buffer> %s[%i]: %#x\n", field->name, ind, buf[ind]);
        } else {
            func_fprintf(f, "<uint8_t buffer> %s[%i]: %i\n", field->name, ind, buf[ind]);
        }
    } else if(!strcmp(field->info->name,"bitmap")) {
        unsigned long *bmp = (unsigned long *)opaque;
        func_fprintf(f, "<bitmap> %s: %i\n", field->name, test_bit(ind,bmp));
    }
    return;
}


static void per_printf_buffer_bitmap(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, const char* name,
                            int n_elems, int size, bool hex)
{    
    if (name) {
        if (n_elems > 1) {
            for (int i = 0; i < n_elems; i++) {
                func_fprintf(f, "- <Array uint8_t buffer el> %s[%i]\n", field->name, i);
            }
        } else {
            per_printf_data_arr_buffer_bitmap(func_fprintf, f, opaque, field, size, hex);
        }
    } else {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array uint8_t buffer> %s\n", field->name); 
        } else {
            func_fprintf(f, "- <uint8_t buffer> %s\n", field->name); 
        }
    }
    return;
}


static uint8_t get_qtailq_size(void* opaque, size_t entry_offset)
{
    uint16_t size = 0;
    void *elm = QTAILQ_RAW_FIRST(opaque);
    while(elm) {
        size++;
        elm = QTAILQ_RAW_NEXT(elm, entry_offset);
    }
    return size;
}

static void per_printf_qtail_c(fprintf_function func_fprintf, void *f,
                    void* opaque, VMStateField *field, int size)
{
    for (int i = 0; i < size; i++) {
        func_fprintf(f, "- <qtailq el> %s[%i]\n", field->name, i);
    }
}

static void per_printf_qtailq(fprintf_function func_fprintf, void *f,
                              void *opaque, VMStateField *field,
                              const char * name, int n_elems, int size)
{    
    if (name) {
        if (n_elems > 1) {
            for (int i = 0; i < n_elems; i++) {
                func_fprintf(f, "- <qtailq array el> %s[%i]\n", field->name, i);
            }
        } else {
            per_printf_qtail_c(func_fprintf, f, opaque, field, size);
        } 
    } else {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array qtailq> %s\n", field->name);
        } else {
            func_fprintf(f, "- <qtailq> %s\n", field->name);
        }
    }
    return;                                
}


static void Print_information_qtail_el(fprintf_function func_fprintf, void *f,
                                    void *opaque, VMStateField *field,
                                    int ind, const char* path, bool hex)
{
    if (field->flags & VMS_POINTER || field->flags & VMS_ARRAY_OF_POINTER) {
        opaque = per_printf_data_pointer(opaque);
    }

    size_t entry_offset = field->start;
    
    int size = get_qtailq_size(opaque, entry_offset);
    
    if (!check_size(func_fprintf, f, size, ind, field)) {
        return;
    }
    void *elm = QTAILQ_RAW_FIRST(opaque);
    uint8_t i = 0;
    while (i < ind) {
        elm = QTAILQ_RAW_NEXT(elm, entry_offset);
        i++;
    }
    per_printf_data_struct(func_fprintf, f, field, elm, path, hex);
}

static void Print_information_buff_bitmap_el(fprintf_function func_fprintf, void *f,
                                             void *opaque, VMStateField *field,
                                             int ind, bool hex)
{    
    int size = vmstate_size(opaque, field);

    if (!check_size(func_fprintf, f, size, ind, field)) {
        return;    
    }

    if (field->flags & VMS_POINTER || field->flags & VMS_ARRAY_OF_POINTER) {
        opaque = per_printf_data_pointer(opaque);
    }
    per_printf_data_buffer_bitmap(func_fprintf, f, opaque, field, ind, hex);

}

static int get_size(void *opaque, VMStateField *field)
{
    if (field->info && !strcmp(field->info->name,"qtailq")) {
        return get_qtailq_size(opaque, field->start);
    } else {
        return vmstate_size(opaque, field);
    }
}

static void Print_information_find_field(fprintf_function func_fprintf, void *f,
                                    void *opaque, VMStateField *field,
                                    const char * name, bool hex)
{
    /*name and n_elems = 1*/
    int size = get_size(opaque, field);

    if (field->flags & VMS_POINTER || field->flags & VMS_ARRAY_OF_POINTER) {
        opaque = per_printf_data_pointer(opaque);
    }
    if ((field->flags & VMS_STRUCT) || (field->flags & VMS_VSTRUCT)) { 
        per_printf_data_struct(func_fprintf, f, field, opaque, name, hex);
    } else if ((!strcmp(field->info->name, "str")) ||
               (!strcmp(field->info->name, "int8")) || 
               (!strcmp(field->info->name, "int16")) ||
               (!strcmp(field->info->name, "int32")) ||
               (!strcmp(field->info->name, "int64")) ||
               (!strcmp(field->info->name, "float64")) ||
               (!strcmp(field->info->name, "int32 le")) ||
               (!strcmp(field->info->name, "int32 equal"))) {
        per_printf_data_basic(func_fprintf, f, opaque, field, hex, true);
    } else if ((!strcmp(field->info->name, "bool")) ||
               (!strcmp(field->info->name, "uint8")) ||
               (!strcmp(field->info->name, "uint16")) ||
               (!strcmp(field->info->name, "uint32")) ||
               (!strcmp(field->info->name, "uint64")) ||
               (!strcmp(field->info->name, "uint8 equal")) ||
               (!strcmp(field->info->name, "uint16 equal")) ||
               (!strcmp(field->info->name, "uint32 equal")) ||
               (!strcmp(field->info->name, "uint64 equal"))) {
        per_printf_data_basic(func_fprintf, f, opaque, field, hex, false);
    } else if ((!strcmp(field->info->name, "CPU_Double_U")) ||
               (!strcmp(field->info->name, "timer"))) {
       per_printf_data_CPUDouble_timer(func_fprintf, f, opaque, field, hex);
    } else if ((!strcmp(field->info->name,"buffer")) ||
                (!strcmp(field->info->name,"unused_buffer")) ||
                (!strcmp(field->info->name,"bitmap"))) {
        per_printf_data_arr_buffer_bitmap(func_fprintf, f, opaque, field, size, hex);
    } else if (!strcmp(field->info->name,"qtailq")) {
        per_printf_qtail_c(func_fprintf, f, opaque, field, size);
    }
    return;
}

static void Print_information_fields(fprintf_function func_fprintf, void *f,
                                     void *opaque, VMStateField *field,
                                     const char * name, bool hex)
{
    int n_elems = vmstate_n_elems(opaque, field);
    int size = get_size(opaque, field);


    if (field->flags & VMS_POINTER) {
        opaque = per_printf_pointer(func_fprintf, f, opaque, field, name);
    } 
    if (field->flags & VMS_ARRAY_OF_POINTER) {
        opaque = per_printf_arr_pointer(func_fprintf, f, opaque, field, name, n_elems);
    } 
    if ((field->flags & VMS_STRUCT) ||  (field->flags & VMS_VSTRUCT)) {
        per_printf_struct(func_fprintf, f, field, opaque, name, n_elems, hex);
    } else {
        if ((!strcmp(field->info->name, "str")) ||
            (!strcmp(field->info->name, "int8")) ||
            (!strcmp(field->info->name, "int16")) ||
            (!strcmp(field->info->name, "int32")) ||
            (!strcmp(field->info->name, "int64")) ||
            (!strcmp(field->info->name, "float64")) ||
            (!strcmp(field->info->name, "int32 le"))) {
            per_printf_basic(func_fprintf, f, opaque, field, name, n_elems, hex, true);
        } else if ((!strcmp(field->info->name, "uint8")) ||
                   (!strcmp(field->info->name, "uint16")) ||
                   (!strcmp(field->info->name, "uint32")) ||
                   (!strcmp(field->info->name, "uint64"))) {
            per_printf_basic(func_fprintf, f, opaque, field, name, n_elems, hex, false);
        } else if (!strcmp(field->info->name, "int32 equal")) {
            per_printf_basic(func_fprintf, f, opaque, field, name, n_elems, hex, true);
        } else if ((!strcmp(field->info->name, "uint8 equal")) ||
                   (!strcmp(field->info->name, "uint16 equal")) ||
                   (!strcmp(field->info->name, "uint32 equal")) ||
                   (!strcmp(field->info->name, "uint64 equal"))) {
            per_printf_int_equal(func_fprintf, f, opaque, field, name, n_elems, hex, false);
        } else if ((!strcmp(field->info->name, "CPU_Double_U")) ||
                   (!strcmp(field->info->name, "timer"))) {
            per_printf_CPUDouble_timer(func_fprintf, f, opaque, field, name, n_elems, hex);
        } else if ((!strcmp(field->info->name,"buffer")) ||
                  (!strcmp(field->info->name,"unused_buffer")) ||
                  (!strcmp(field->info->name,"bitmap"))) {
           per_printf_buffer_bitmap(func_fprintf, f, opaque, field, name, n_elems, size, hex);
        } else if (!strcmp(field->info->name,"qtailq")) {
            per_printf_qtailq(func_fprintf, f, opaque, field, name, n_elems, size);
        }
    }
    return;
}


void vmsd_data(fprintf_function func_fprintf, void *f, const char* path,
               const VMStateDescription *vmsd, void *opaque, bool hex)
{
    if (!path) {
        VMStateField *field = vmsd->fields;
        while (field->name != NULL) {
            void* curr_elem = opaque + field->offset;
            Print_information_fields(func_fprintf, f, curr_elem, field, NULL, hex);
            field++;
        }

        return;
    }
    
    const char* name = get_name(&path);
    int ind = per_get_ind_name(&name);
    
    if (ind == INDERROR) {
        func_fprintf(f, "Invalid field index received\n");
        return;
    }

    if (!path && name) {
        path = name;
    }

    VMStateField *field = vmsd->fields;
    while (field->name != NULL) {
        if (!strcmp(field->name, name)) {
            void *curr_elem = opaque + field->offset;
            if (ind != INDEMPTY) {
                int n_elems = vmstate_n_elems(curr_elem, field);
                if (n_elems > 1) {
                    /*array*/
                    if (ind >= n_elems) {
                        func_fprintf(f, "Invalid field index received\n");
                        return;
                    }
                    int size = vmstate_size(curr_elem, field);
                    curr_elem += size * ind;
                    Print_information_find_field(func_fprintf, f, curr_elem,
                                                 field, path, hex);
                } else if ((!strcmp(field->info->name,"buffer")) ||
                        (!strcmp(field->info->name,"unused_buffer")) ||
                        (!strcmp(field->info->name,"bitmap"))) {
                    /*if we enter bitmap or buffer [i]*/
                    Print_information_buff_bitmap_el(func_fprintf, f, curr_elem, field, ind, hex);
                } else if (!strcmp(field->info->name,"qtailq")) {
                    /*if we enter qtailq[i]*/
                    Print_information_qtail_el(func_fprintf, f, curr_elem, field, ind, path, hex);
                } else {
                    func_fprintf(f, "this field cannot be accessed by index\n");
                }
            } else {
                Print_information_fields(func_fprintf, f, curr_elem, field, path, hex);
            }
            return;
        }
        field++;
    }

    func_fprintf(f, "Current field hasn't child field with name = \"%s\"\n", name);
    return;
}


static int vmfield_name_num(VMStateField *start, VMStateField *search)
{
    VMStateField *field;
    int found = 0;

    for (field = start; field->name; field++) {
        if (!strcmp(field->name, search->name)) {
            if (field == search) {
                return found;
            }
            found++;
        }
    }

    return -1;
}

static bool vmfield_name_is_unique(VMStateField *start, VMStateField *search)
{
    VMStateField *field;
    int found = 0;

    for (field = start; field->name; field++) {
        if (!strcmp(field->name, search->name)) {
            found++;
            /* name found more than once, so it's not unique */
            if (found > 1) {
                return false;
            }
        }
    }

    return true;
}

static const char *vmfield_get_type_name(VMStateField *field)
{
    const char *type = "unknown";

    if (field->flags & VMS_STRUCT) {
        type = "struct";
    } else if (field->flags & VMS_VSTRUCT) {
        type = "vstruct";
    } else if (field->info->name) {
        type = field->info->name;
    }

    return type;
}

static bool vmsd_can_compress(VMStateField *field)
{
    if (field->field_exists) {
        /* Dynamically existing fields mess up compression */
        return false;
    }

    if (field->flags & VMS_STRUCT) {
        VMStateField *sfield = field->vmsd->fields;
        while (sfield->name) {
            if (!vmsd_can_compress(sfield)) {
                /* Child elements can't compress, so can't we */
                return false;
            }
            sfield++;
        }

        if (field->vmsd->subsections) {
            /* Subsections may come and go, better don't compress */
            return false;
        }
    }

    return true;
}

static void vmsd_desc_field_start(const VMStateDescription *vmsd, QJSON *vmdesc,
                                  VMStateField *field, int i, int max)
{
    char *name, *old_name;
    bool is_array = max > 1;
    bool can_compress = vmsd_can_compress(field);

    if (!vmdesc) {
        return;
    }

    name = g_strdup(field->name);

    /* Field name is not unique, need to make it unique */
    if (!vmfield_name_is_unique(vmsd->fields, field)) {
        int num = vmfield_name_num(vmsd->fields, field);
        old_name = name;
        name = g_strdup_printf("%s[%d]", name, num);
        g_free(old_name);
    }

    json_start_object(vmdesc, NULL);
    json_prop_str(vmdesc, "name", name);
    if (is_array) {
        if (can_compress) {
            json_prop_int(vmdesc, "array_len", max);
        } else {
            json_prop_int(vmdesc, "index", i);
        }
    }
    json_prop_str(vmdesc, "type", vmfield_get_type_name(field));

    if (field->flags & VMS_STRUCT) {
        json_start_object(vmdesc, "struct");
    }

    g_free(name);
}

static void vmsd_desc_field_end(const VMStateDescription *vmsd, QJSON *vmdesc,
                                VMStateField *field, size_t size, int i)
{
    if (!vmdesc) {
        return;
    }

    if (field->flags & VMS_STRUCT) {
        /* We printed a struct in between, close its child object */
        json_end_object(vmdesc);
    }

    json_prop_int(vmdesc, "size", size);
    json_end_object(vmdesc);
}


bool vmstate_save_needed(const VMStateDescription *vmsd, void *opaque)
{
    if (vmsd->needed && !vmsd->needed(opaque)) {
        /* optional section not needed */
        return false;
    }
    return true;
}


int vmstate_save_state(QEMUFile *f, const VMStateDescription *vmsd,
                       void *opaque, QJSON *vmdesc_id)
{
    return vmstate_save_state_v(f, vmsd, opaque, vmdesc_id, vmsd->version_id);
}

int vmstate_save_state_v(QEMUFile *f, const VMStateDescription *vmsd,
                         void *opaque, QJSON *vmdesc, int version_id)
{
    int ret = 0;
    VMStateField *field = vmsd->fields;

    trace_vmstate_save_state_top(vmsd->name);

    if (vmsd->pre_save) {
        ret = vmsd->pre_save(opaque);
        trace_vmstate_save_state_pre_save_res(vmsd->name, ret);
        if (ret) {
            error_report("pre-save failed: %s", vmsd->name);
            return ret;
        }
    }

    if (vmdesc) {
        json_prop_str(vmdesc, "vmsd_name", vmsd->name);
        json_prop_int(vmdesc, "version", version_id);
        json_start_array(vmdesc, "fields");
    }

    while (field->name) {
        if ((field->field_exists &&
             field->field_exists(opaque, version_id)) ||
            (!field->field_exists &&
             field->version_id <= version_id)) {
            void *first_elem = opaque + field->offset;
            int i, n_elems = vmstate_n_elems(opaque, field);
            int size = vmstate_size(opaque, field);
            int64_t old_offset, written_bytes;
            QJSON *vmdesc_loop = vmdesc;

            trace_vmstate_save_state_loop(vmsd->name, field->name, n_elems);
            if (field->flags & VMS_POINTER) {
                first_elem = *(void **)first_elem;
                assert(first_elem || !n_elems || !size);
            }
            for (i = 0; i < n_elems; i++) {
                void *curr_elem = first_elem + size * i;
                ret = 0;

                vmsd_desc_field_start(vmsd, vmdesc_loop, field, i, n_elems);
                old_offset = qemu_ftell_fast(f);
                if (field->flags & VMS_ARRAY_OF_POINTER) {
                    assert(curr_elem);
                    curr_elem = *(void **)curr_elem;
                }
                if (!curr_elem && size) {
                    /* if null pointer write placeholder and do not follow */
                    assert(field->flags & VMS_ARRAY_OF_POINTER);
                    ret = vmstate_info_nullptr.put(f, curr_elem, size, NULL,
                                                   NULL);
                } else if (field->flags & VMS_STRUCT) {
                    ret = vmstate_save_state(f, field->vmsd, curr_elem,
                                             vmdesc_loop);
                } else if (field->flags & VMS_VSTRUCT) {
                    ret = vmstate_save_state_v(f, field->vmsd, curr_elem,
                                               vmdesc_loop,
                                               field->struct_version_id);
                } else {
                    ret = field->info->put(f, curr_elem, size, field,
                                     vmdesc_loop);
                }
                if (ret) {
                    error_report("Save of field %s/%s failed",
                                 vmsd->name, field->name);
                    return ret;
                }

                written_bytes = qemu_ftell_fast(f) - old_offset;
                vmsd_desc_field_end(vmsd, vmdesc_loop, field, written_bytes, i);

                /* Compressed arrays only care about the first element */
                if (vmdesc_loop && vmsd_can_compress(field)) {
                    vmdesc_loop = NULL;
                }
            }
        } else {
            if (field->flags & VMS_MUST_EXIST) {
                error_report("Output state validation failed: %s/%s",
                        vmsd->name, field->name);
                assert(!(field->flags & VMS_MUST_EXIST));
            }
        }
        field++;
    }

    if (vmdesc) {
        json_end_array(vmdesc);
    }

    return vmstate_subsection_save(f, vmsd, opaque, vmdesc);
}

static const VMStateDescription *
vmstate_get_subsection(const VMStateDescription **sub, char *idstr)
{
    while (sub && *sub) {
        if (strcmp(idstr, (*sub)->name) == 0) {
            return *sub;
        }
        sub++;
    }
    return NULL;
}

static int vmstate_subsection_load(QEMUFile *f, const VMStateDescription *vmsd,
                                   void *opaque)
{
    trace_vmstate_subsection_load(vmsd->name);

    while (qemu_peek_byte(f, 0) == QEMU_VM_SUBSECTION) {
        char idstr[256], *idstr_ret;
        int ret;
        uint8_t version_id, len, size;
        const VMStateDescription *sub_vmsd;

        len = qemu_peek_byte(f, 1);
        if (len < strlen(vmsd->name) + 1) {
            /* subsection name has be be "section_name/a" */
            trace_vmstate_subsection_load_bad(vmsd->name, "(short)", "");
            return 0;
        }
        size = qemu_peek_buffer(f, (uint8_t **)&idstr_ret, len, 2);
        if (size != len) {
            trace_vmstate_subsection_load_bad(vmsd->name, "(peek fail)", "");
            return 0;
        }
        memcpy(idstr, idstr_ret, size);
        idstr[size] = 0;

        if (strncmp(vmsd->name, idstr, strlen(vmsd->name)) != 0) {
            trace_vmstate_subsection_load_bad(vmsd->name, idstr, "(prefix)");
            /* it doesn't have a valid subsection name */
            return 0;
        }
        sub_vmsd = vmstate_get_subsection(vmsd->subsections, idstr);
        if (sub_vmsd == NULL) {
            trace_vmstate_subsection_load_bad(vmsd->name, idstr, "(lookup)");
            return -ENOENT;
        }
        qemu_file_skip(f, 1); /* subsection */
        qemu_file_skip(f, 1); /* len */
        qemu_file_skip(f, len); /* idstr */
        version_id = qemu_get_be32(f);

        ret = vmstate_load_state(f, sub_vmsd, opaque, version_id);
        if (ret) {
            trace_vmstate_subsection_load_bad(vmsd->name, idstr, "(child)");
            return ret;
        }
    }

    trace_vmstate_subsection_load_good(vmsd->name);
    return 0;
}

static int vmstate_subsection_save(QEMUFile *f, const VMStateDescription *vmsd,
                                   void *opaque, QJSON *vmdesc)
{
    const VMStateDescription **sub = vmsd->subsections;
    bool subsection_found = false;
    int ret = 0;

    trace_vmstate_subsection_save_top(vmsd->name);
    while (sub && *sub) {
        if (vmstate_save_needed(*sub, opaque)) {
            const VMStateDescription *vmsdsub = *sub;
            uint8_t len;

            trace_vmstate_subsection_save_loop(vmsd->name, vmsdsub->name);
            if (vmdesc) {
                /* Only create subsection array when we have any */
                if (!subsection_found) {
                    json_start_array(vmdesc, "subsections");
                    subsection_found = true;
                }

                json_start_object(vmdesc, NULL);
            }

            qemu_put_byte(f, QEMU_VM_SUBSECTION);
            len = strlen(vmsdsub->name);
            qemu_put_byte(f, len);
            qemu_put_buffer(f, (uint8_t *)vmsdsub->name, len);
            qemu_put_be32(f, vmsdsub->version_id);
            ret = vmstate_save_state(f, vmsdsub, opaque, vmdesc);
            if (ret) {
                return ret;
            }

            if (vmdesc) {
                json_end_object(vmdesc);
            }
        }
        sub++;
    }

    if (vmdesc && subsection_found) {
        json_end_array(vmdesc);
    }

    return ret;
}
