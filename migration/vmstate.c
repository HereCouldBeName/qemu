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
void vmsd_data(fprintf_function func_fprintf, void *f, const VMStateDescription *vmsd, void *opaque)
{
    VMStateField *field = vmsd->fields;
    while (field->name != NULL) {
        void *first_elem = opaque + field->offset;
        int n_elems = vmstate_n_elems(opaque, field);
        int size = vmstate_size(opaque, field);

        if (field->flags & VMS_POINTER) {
            first_elem = *(void **)first_elem;
            assert(first_elem || !n_elems || !size);
        }
        for (int i = 0; i < n_elems; i++) {
            void *curr_elem = first_elem + size * i;

            func_fprintf(f,"%s\n",field->name);

            if (field->flags & VMS_ARRAY_OF_POINTER) {
                curr_elem = *(void **)curr_elem;
            }
            if (field->flags & VMS_STRUCT) {
                func_fprintf(f,"СТРУКТУРА!!\n");
                vmsd_data(func_fprintf,f, field->vmsd, curr_elem);
                func_fprintf(f,"КОНЕЦ!!\n");
            }
            else if (field->flags & VMS_VSTRUCT) {
                func_fprintf(f,"СТРУКТУРА!!\n");
                vmsd_data(func_fprintf,f, field->vmsd, curr_elem);
                func_fprintf(f,"КОНЕЦ!!\n");
            }
            else {
                func_fprintf(f,"TYPE: %s\n",field->info->name);
                if (!strcmp(field->info->name,"int8")) {
                    func_fprintf(f,"%i\n",*(int8_t *)curr_elem);
                } else if (!strcmp(field->info->name,"bool")) {
                    func_fprintf(f,"%i\n",*(bool *)curr_elem);
                } else if (!strcmp(field->info->name,"int16")) {
                    func_fprintf(f,"%i\n",*(int16_t *)curr_elem);
                } else if (!strcmp(field->info->name,"int32") ||
                        !strcmp(field->info->name,"int32 le")) {
                    func_fprintf(f,"%i\n",*(int32_t *)curr_elem);
                } else if (!strcmp(field->info->name,"int32 equal")) {
                    if (field->err_hint) {
                        func_fprintf(f,"%s\n", field->err_hint);
                    } else {
                        func_fprintf(f,"%i\n",*(int32_t *)curr_elem);
                    }
                }
                 else if (!strcmp(field->info->name,"int64")) {
                    func_fprintf(f,"%li\n",*(int64_t *)curr_elem);
                } else if (!strcmp(field->info->name,"uint8")) {
                    func_fprintf(f,"%i\n",*(uint8_t *)curr_elem);
                } else if (!strcmp(field->info->name,"uint8 equal")) {
                    if (field->err_hint) {
                        func_fprintf(f,"%s\n", field->err_hint);
                    } else {
                        func_fprintf(f,"%i\n",*(uint8_t *)curr_elem);
                    }
                } else if (!strcmp(field->info->name,"uint16")) {
                    func_fprintf(f,"%i\n",*(uint16_t *)curr_elem);
                } else if (!strcmp(field->info->name,"uint16 equal")) {
                    if (field->err_hint) {
                        func_fprintf(f,"%s\n", field->err_hint);
                    } else {
                        func_fprintf(f,"%i\n",*(uint16_t *)curr_elem);
                    }
                } else if (!strcmp(field->info->name,"uint32")) {
                    func_fprintf(f,"%i\n",*(uint32_t *)curr_elem);
                } else if (!strcmp(field->info->name,"uint32 equal")) {
                    if (field->err_hint) {
                        func_fprintf(f,"%s\n", field->err_hint);
                    } else {
                        func_fprintf(f,"%i\n",*(uint32_t *)curr_elem);
                    }
                } else if (!strcmp(field->info->name,"uint64")) {
                    func_fprintf(f,"%li\n",*(uint64_t *)curr_elem);
                } else if (!strcmp(field->info->name,"uint64 equal")) {
                    if (field->err_hint) {
                        func_fprintf(f,"%s\n", field->err_hint);
                    } else {
                        func_fprintf(f,"%li\n",*(uint64_t *)curr_elem);
                    }
                } else if (!strcmp(field->info->name,"float64")) {
                    func_fprintf(f,"%li\n",*(float64 *)curr_elem);
                } else if (!strcmp(field->info->name,"CPU_Double_U")) {
                    CPU_DoubleU elem = *(CPU_DoubleU *)curr_elem;
                    func_fprintf(f,"ld: %ld, lower: %i, upper: %i, ll: %li\n",
                                elem.d, elem.l.lower,elem.l.upper,elem.ll) ;
                } else if (!strcmp(field->info->name,"timer")) {
                    QEMUTimer elem = *(QEMUTimer *)curr_elem;
                    func_fprintf(f,"expire_time: %li, opaque: %p, scale: %i\n",
                                elem.expire_time, elem.opaque, elem.scale);
                } else if (!strcmp(field->info->name,"buffer") ||
                        !strcmp(field->info->name,"unused_buffer")) {
                    uint8_t *buf = (uint8_t *)curr_elem;
                    for (int i=0; i < field->size; i++) {
                        func_fprintf(f,"%i ",buf[i]);
                    }
                    func_fprintf(f,"\n");
                } else if (!strcmp(field->info->name,"bitmap")) {
                    unsigned long *bmp = (unsigned long *)curr_elem;
                    int idx = 0;
                    for (int i = 0; i < BITS_TO_U64S(size); i++) {
                        uint64_t w = bmp[idx++];
                        if (sizeof(unsigned long) == 4 && idx < BITS_TO_LONGS(size)) {
                            w |= ((uint64_t)bmp[idx++]) << 32;
                        }
                        func_fprintf(f,"%li ",w);
                    }
                    func_fprintf(f,"\n");
                } else if (!strcmp(field->info->name,"qtailq")) {
                    func_fprintf(f,"-------------qtailq start--------------\n");
                    size_t entry_offset = field->start;
                    void *elm;
                    QTAILQ_RAW_FOREACH(elm, curr_elem, entry_offset) {
                        vmsd_data(func_fprintf,f, field->vmsd, elm);
                    }
                    func_fprintf(f,"-------------qtailq finish--------------\n");
                } else if (!strcmp(field->info->name,"str")) {
                    func_fprintf(f,"%s\n",(char *)curr_elem);
                }
            }
        }
        field++;
    }
}


void vmsd_test(fprintf_function func_fprintf, void *f, const char* name, CurrPosDebug* cpd){
    func_fprintf(f,"%s %s",name,cpd->field->name);
}

static void print_path(CurrPosDebug* cpd, fprintf_function func_fprintf, void *f, const char* name){
    if (!cpd->last) {
        func_fprintf(f, "path: ");
        return;
    }
    print_path(cpd->last, func_fprintf, f, name);
    func_fprintf(f, "%s/",cpd->name);
    return;
}

/*
    Function search index in the entered string
    name - user input string
    parent_name - name field
*/

static int per_get_index(const char* name, const char* parent_name) {
    int ind = -1;
    int size = strlen(name);
    char feild_name[size];
    memset(feild_name,0x0,size);

    for (int i=0; i < size; i++) {
        if (name[i] != '[') {
            feild_name[i] += name[i];
        } else {
            if (strlen(parent_name) != i || strcmp(parent_name,feild_name)
             || name[size-1] != ']') {
                return -1;
            }
            ind = 0;
            int b_ind = i+1;
            int len = size-2;
            for (int j =len; j>=i+1; j--) {
                ind *= 10;
                ind += (name[b_ind + len - j] - '0');
            }
            break;
        }
    }
    return ind;
}

static CurrPosDebug* per_printf_struct(const char* txt, fprintf_function func_fprintf,
                                    void *f, VMStateField *field, void *opaque, CurrPosDebug* cpd,
                                    const char * name)  {
    const VMStateDescription *vmsd = cpd->vmsd;
    int n_elems = cpd->is_array ? 1 : vmstate_n_elems(opaque, field);


    /*Go to struct of array element*/
    // if (cpd->is_array) {
    //     n_elems = 1;
    // }

    if (name) {
        if (n_elems > 1) {
            for (int i = 0; i < n_elems; i++) {
                func_fprintf(f, "- <%s el> %s[%i]\n", txt, field->name, i);
            }
            cpd = create_next_cpd_array(cpd, vmsd, field, opaque, name);
        } else {
            vmsd = field->vmsd;
            field = vmsd->fields;
            cpd = create_next_cpd(cpd, vmsd, field, opaque, name);
            cpd = vmsd_data_1(func_fprintf, f, NULL, cpd);
        }
    } else {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array %s> %s\n", txt, field->name);
        } else {
            func_fprintf(f, "- <%s> %s\n", txt, field->name);
        }
    }
    return cpd;                                
}

static void* per_printf_pointer(bool is_name, fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field) {
    if (is_name) {
        opaque = *(void **)opaque;
        assert(opaque);
    } else {
        func_fprintf(f, "- <VMS POINTER> %s\n", field->name);
    }
    return opaque;
}

static void* per_printf_arr_pointer(bool is_name, fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field) {
    
    int n_elems = vmstate_n_elems(opaque, field);

    if (is_name) {
        if (n_elems > 1) {
            for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <VMS array of pointer el> %s[%i]\n", field->name, i);
            }
        } else {
            opaque = *(void**)opaque;
        }
    } else {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array VMS array of pointer> %s\n", field->name);
        } else {
            func_fprintf(f, "- <VMS array of pointer> %s\n", field->name);
        }
    }
    return opaque;
}

static CurrPosDebug* per_printf_int8(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd, 
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (n_elems > 1) {
        if (name) {
           for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <Array int8_t el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name); 
        } else {
           func_fprintf(f, "- <Array int8_t> %s\n", field->name); 
        }
    } else {
        func_fprintf(f, "- <int8_t> %s %i\n", field->name, *(int8_t *)opaque);
    }
    return cpd;
}

static CurrPosDebug* per_printf_bool(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (n_elems > 1) {
        if (name) {
           for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <Array bool el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name); 
        } else {
           func_fprintf(f, "- <Array bool> %s\n", field->name); 
        }
    } else {
        func_fprintf(f, "- <bool> %s %i\n", field->name, *(bool *)opaque);
    }
    return cpd;
}

static CurrPosDebug* per_printf_int16(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (n_elems > 1) {
        if (name) {
           for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <Array int16_t el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name);  
        } else {
           func_fprintf(f, "- <Array int16_t> %s\n", field->name); 
        }
    } else {
        func_fprintf(f, "- <int16_t> %s %i\n", field->name, *(int16_t *)opaque);
    }
    return cpd;
}

static CurrPosDebug* per_printf_int32(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (n_elems > 1) {
        if (name) {
           for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <Array int32_t el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name);  
        } else {
           func_fprintf(f, "- <Array int32_t> %s\n", field->name); 
        }
    } else {
        func_fprintf(f, "- <int32_t> %s %i\n", field->name, *(int32_t *)opaque);
    }
    return cpd;
}

static CurrPosDebug* per_printf_int32_equal(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd, 
                            const char* name) {   
    if (field->err_hint) {
        func_fprintf(f, "- <int32_t> %s <ERROR> %s\n", field->name, field->err_hint);
    } else {
        cpd = per_printf_int32(func_fprintf, f, opaque, field, cpd, name);
    }
    return cpd;
}

static CurrPosDebug* per_printf_int64(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd, 
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (n_elems > 1) {
        if (name) {
           for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <Array int64_t el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name);
        } else {
           func_fprintf(f, "- <Array int64_t> %s\n", field->name); 
        }
    } else {
        func_fprintf(f, "- <int64_t> %s %li\n", field->name, *(int64_t *)opaque);
    }
    return cpd;
}

static CurrPosDebug* per_printf_uint8(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);

    if (n_elems > 1 && !cpd->is_array) {
        if (name) {
           for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <Array uint8_t el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name); 
        } else {
           func_fprintf(f, "- <Array uint8_t> %s\n", field->name); 
        }
    } else {
        func_fprintf(f, "- <uint8_t> %s %i\n", field->name, *(uint8_t *)opaque);
    }
    return cpd;
}

static CurrPosDebug* per_printf_uint8_equal(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {    
    if (field->err_hint) {
        func_fprintf(f, "- <uint8_t> %s <ERROR> %s\n", field->name, field->err_hint);
    } else {
        cpd = per_printf_uint8(func_fprintf, f, opaque, field, cpd, name);
    }
    return cpd;
}

static CurrPosDebug* per_printf_uint16(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (n_elems > 1) {
        if (name) {
           for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <Array uint16_t el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name); 
        } else {
           func_fprintf(f, "- <Array uint16_t> %s\n", field->name); 
        }
    } else {
        func_fprintf(f, "- <uint16_t> %s %i\n", field->name, *(uint16_t *)opaque);
    }
    return cpd;
}

static CurrPosDebug* per_printf_uint16_equal(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {    
    if (field->err_hint) {
        func_fprintf(f, "- <uint16_t> %s <ERROR> %s\n", field->name, field->err_hint);
    } else {
        cpd = per_printf_uint16(func_fprintf, f, opaque, field, cpd, name);
    }
    return cpd;
}

static CurrPosDebug* per_printf_uint32(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (n_elems > 1) {
        if (name) {
           for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <Array uint32_t el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name);
        } else {
           func_fprintf(f, "- <Array uint32_t> %s\n", field->name); 
        }
    } else {
        func_fprintf(f, "- <uint32_t> %s %i\n", field->name, *(uint32_t *)opaque);
    }
    return cpd;
}

static CurrPosDebug* per_printf_uint32_equal(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {    
    if (field->err_hint) {
        func_fprintf(f, "- <uint32_t> %s <ERROR> %s\n", field->name, field->err_hint);
    } else {
        cpd = per_printf_uint32(func_fprintf, f, opaque, field, cpd, name);
    }
    return cpd;
}

static CurrPosDebug* per_printf_uint64(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (n_elems > 1) {
        if (name) {
           for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <Array uint64_t el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name); 
        } else {
           func_fprintf(f, "- <Array uint64_t> %s\n", field->name); 
        }
    } else {
        func_fprintf(f, "- <uint64_t> %s %li\n", field->name, *(uint64_t *)opaque);
    }
    return cpd;
}

static CurrPosDebug* per_printf_uint64_equal(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {    
    if (field->err_hint) {
        func_fprintf(f, "- <uint64_t> %s <ERROR> %s\n", field->name, field->err_hint);
    } else {
        cpd = per_printf_uint64(func_fprintf, f, opaque, field, cpd, name);
    }
    return cpd;
}

static CurrPosDebug* per_printf_float64(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd, 
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (n_elems > 1) {
        if (name) {
           for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <Array float64 el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name);  
        } else {
           func_fprintf(f, "- <Array float64> %s\n", field->name); 
        }
    } else {
        func_fprintf(f, "- <float64> %s %li\n", field->name, *(float64 *)opaque);
    }
    return cpd;
}

static CurrPosDebug* per_printf_CPU_Double_U(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (name) {
        if (n_elems > 1) {
            for (int i = 0; i < n_elems; i++) {
                func_fprintf(f, "- <Array CPU_DoubleU el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name);  
        } else {
            CPU_DoubleU elem = *(CPU_DoubleU *)opaque;
            func_fprintf(f,"- <CPU_DoubleU> ld: %ld, lower: %i, upper: %i, ll: %li\n",
                        elem.d, elem.l.lower,elem.l.upper,elem.ll) ;
        }
    } else {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array CPU_DoubleU> %s\n", field->name); 
        } else {
            func_fprintf(f, "- <CPU_DoubleU> %s\n", field->name); 
        }
    }
    return cpd;
}

static CurrPosDebug* per_printf_timer(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (name) {
        if (n_elems > 1) {
            for (int i = 0; i < n_elems; i++) {
                func_fprintf(f, "- <Array QEMUTimer el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name);  
        } else {
            QEMUTimer elem = *(QEMUTimer *)opaque;
            func_fprintf(f, "- <QEMUTimer> expire_time: %li, opaque: %p, scale: %i\n",
                        elem.expire_time, elem.opaque, elem.scale);
        }
    } else {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array QEMUTimer> %s\n", field->name); 
        } else {
            func_fprintf(f, "- <QEMUTimer> %s\n", field->name); 
        }
    }
    return cpd;
}

static CurrPosDebug* per_printf_buffer(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (name) {
        if (n_elems > 1) {
            for (int i = 0; i < n_elems; i++) {
                func_fprintf(f, "- <Array uint8_t buffer el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name);
        } else {
            uint8_t *buf = (uint8_t *)opaque;
            if (field->size < 1) {
                func_fprintf(f, "- <uint8_t buffer> %s is empty\n", field->name);
                return cpd;    
            } 
            func_fprintf(f, "- <uint8_t buffer> %s: \n", field->name);
            for (int i = 0; i < field->size; i++) {
                func_fprintf(f, "%i " ,buf[i]);
            }
            func_fprintf(f,"\n");
        }
    } else {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array uint8_t buffer> %s\n", field->name); 
        } else {
            func_fprintf(f, "- <uint8_t buffer> %s\n", field->name); 
        }
    }
    return cpd;
}


static CurrPosDebug* per_printf_bitmap(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd,
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (name) {
        if (n_elems > 1) {
            for (int i = 0; i < n_elems; i++) {
                func_fprintf(f, "- <Array bitmap el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name);
        } else {
            int size = vmstate_size(cpd->opaque, field);
            unsigned long *bmp = (unsigned long *)opaque;
            int i, idx = 0;
            func_fprintf(f, "<bitmap> %s: \n", name);
            func_fprintf(f,"SIZE : %i not size: %i\n", BITS_TO_U64S(size), size);
            for (i = 0; i < BITS_TO_U64S(size); i++) {
                idx++;
                if (sizeof(unsigned long) == 4 && idx < BITS_TO_LONGS(size)) {
                    bmp[idx] = bmp[idx] >> 32;
                    idx++;
                }
            }
            for(long i=0; i < size ; i++) {
                func_fprintf(f,"%i ", test_bit(i,bmp));
            }
            func_fprintf(f,"\n");
        }
    } else {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array bitmap> %s\n", field->name); 
        } else {
            func_fprintf(f, "- <bitmap> %s\n", field->name); 
        }
    }
    return cpd;
}


static CurrPosDebug* per_printf_str(fprintf_function func_fprintf, void *f,
                            void* opaque, VMStateField *field, CurrPosDebug* cpd, 
                            const char* name) {
    
    int n_elems = vmstate_n_elems(opaque, field);
    
    if (n_elems > 1) {
        if (name) {
           for (int i=0; i < n_elems; i++) {
                func_fprintf(f, "- <Array str el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, cpd->vmsd, field, opaque, name);  
        } else {
           func_fprintf(f, "- <Array str> %s\n", field->name); 
        }
    } else {
        func_fprintf(f, "- <str> %s %s\n", field->name, (char *)opaque);
    }
    return cpd;
}


static uint8_t get_qlist_size(void* opaque, size_t entry_offset) {
    uint16_t size = 0;
    void *elm = QTAILQ_RAW_FIRST(opaque);
    while(elm) {
        size++;
        elm = QTAILQ_RAW_NEXT(elm, entry_offset);
    }
    return size;
}


static CurrPosDebug* per_printf_qtailq(fprintf_function func_fprintf, void *f,
                                    void *opaque, VMStateField *field, CurrPosDebug* cpd,
                                    const char * name)  {
    const VMStateDescription *vmsd = cpd->vmsd;
    int n_elems = cpd->is_array ? 1 : vmstate_n_elems(opaque, field);

    if (name) {
        if (n_elems > 1) {
            for (int i = 0; i < n_elems; i++) {
                func_fprintf(f, "- <qtailq array el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_array(cpd, vmsd, field, opaque, name);
        } else if (!cpd->is_qlist) {
            int size = get_qlist_size(opaque, field->start);
            for (int i = 0; i < size; i++) {
                func_fprintf(f, "- <qtailq el> %s[%i]\n", field->name, i);
            }
            cpd = create_next_cpd_qlist(cpd, vmsd, field, opaque, name);
        } else {
            vmsd = field->vmsd;
            field = vmsd->fields;
            cpd = create_next_cpd(cpd, vmsd, field, opaque, name);
            cpd = vmsd_data_1(func_fprintf, f, NULL, cpd);
        }
    } else {
        if (n_elems > 1) {
            func_fprintf(f, "- <Array qtailq> %s\n", field->name);
        } else {
            func_fprintf(f, "- <qtailq> %s\n", field->name);
        }
    }
    return cpd;                                
}


static CurrPosDebug* Print_information_fields(fprintf_function func_fprintf, void *f,
                                    void *opaque, VMStateField *field, CurrPosDebug* cpd,
                                    const char * name) {
    
    bool is_name = name ? true : false;

    if (field->flags & VMS_POINTER) {
        opaque = per_printf_pointer(is_name, func_fprintf, f, opaque, field);
        if (!is_name) {
            return cpd;
        }
    } 
    if (field->flags & VMS_ARRAY_OF_POINTER) {
        opaque = per_printf_arr_pointer(is_name, func_fprintf, f, opaque, field);
        int n_elems = vmstate_n_elems(opaque, field);
        if (!(is_name && n_elems == 1)) {
            return cpd;
        }
    } 
    if (field->flags & VMS_STRUCT) {
        cpd = per_printf_struct("Struct", func_fprintf,
                                f, field, opaque, cpd, name);
    } else if (field->flags & VMS_VSTRUCT) {
        cpd = per_printf_struct("VStruct", func_fprintf,
                                f, field, opaque, cpd, name);
    } else {
        if (!strcmp(field->info->name,"int8")) {
            cpd = per_printf_int8(func_fprintf, f, opaque, field,
                                  cpd, name);
        } else if (!strcmp(field->info->name,"bool")) {
            cpd = per_printf_bool(func_fprintf, f, opaque, field,
                                  cpd, name);
        } else if (!strcmp(field->info->name,"int16")) {
            cpd = per_printf_int16(func_fprintf, f, opaque, field,
                                   cpd, name);
        } else if (!strcmp(field->info->name,"int32") ||
                   !strcmp(field->info->name,"int32 le")) {
            cpd = per_printf_int32(func_fprintf, f, opaque, field,
                                   cpd, name);
        } else if (!strcmp(field->info->name,"int32 equal")) {
            cpd = per_printf_int32_equal(func_fprintf, f, opaque, field, 
                                         cpd, name);
        } else if (!strcmp(field->info->name,"int64")) {
            cpd = per_printf_int64(func_fprintf, f, opaque, field,
                                   cpd, name);
        } else if (!strcmp(field->info->name,"uint8")) {
            cpd = per_printf_uint8(func_fprintf, f, opaque, field, 
                                   cpd, name);
        } else if (!strcmp(field->info->name,"uint8 equal")) {
            cpd = per_printf_uint8_equal(func_fprintf, f, opaque, field,
                                         cpd, name);
        } else if (!strcmp(field->info->name,"uint16")) {
            cpd = per_printf_uint16(func_fprintf, f, opaque, field,
                                    cpd, name);
        } else if (!strcmp(field->info->name,"uint16 equal")) {
            cpd = per_printf_uint16_equal(func_fprintf, f, opaque, field,
                                          cpd, name);
        } else if (!strcmp(field->info->name,"uint32")) {
            cpd = per_printf_uint32(func_fprintf, f, opaque, field,
                                    cpd, name);
        } else if (!strcmp(field->info->name,"uint32 equal")) {
            cpd = per_printf_uint32_equal(func_fprintf, f, opaque, field,
                                          cpd, name);
        } else if (!strcmp(field->info->name,"uint64")) {
            cpd = per_printf_uint64(func_fprintf, f, opaque, field,
                                    cpd, name);
        } else if (!strcmp(field->info->name,"uint64 equal")) {
            cpd = per_printf_uint64_equal(func_fprintf, f, opaque, field,
                                          cpd, name);
        } else if (!strcmp(field->info->name,"float64")) {
            cpd = per_printf_float64(func_fprintf, f, opaque, field,
                                     cpd, name);
        } else if (!strcmp(field->info->name,"CPU_Double_U")) {
            cpd = per_printf_CPU_Double_U(func_fprintf, f, opaque, field,
                                          cpd, name);
        } else if (!strcmp(field->info->name,"timer")) {
            cpd = per_printf_timer(func_fprintf, f, opaque, field,
                                   cpd, name);
        } else if (!strcmp(field->info->name,"buffer") ||
                  !strcmp(field->info->name,"unused_buffer")) {
            cpd = per_printf_buffer(func_fprintf, f, opaque, field,
                                    cpd, name);
        } else if (!strcmp(field->info->name,"bitmap")) {
            cpd = per_printf_bitmap(func_fprintf, f, opaque, field,
                                    cpd, name);
        } else if (!strcmp(field->info->name,"qtailq")) {
            cpd = per_printf_qtailq(func_fprintf, f, opaque, field,
                                    cpd, name);
        } else if (!strcmp(field->info->name,"str")) {
            cpd = per_printf_str(func_fprintf, f, opaque, field,
                                 cpd, name);
        }
    }
    return cpd;
}

CurrPosDebug* vmsd_data_1(fprintf_function func_fprintf, void *f, const char* name, CurrPosDebug* cpd) {
    
    print_path(cpd, func_fprintf, f, name);
    if (name) {
        func_fprintf(f, "%s",name);
    } 
    func_fprintf(f, "\n");
    
    const VMStateDescription *vmsd = cpd->vmsd;
    void *opaque = cpd->opaque;
    VMStateField *field = cpd->field ? cpd->field : vmsd->fields;
    
    void* curr_elem;
    
    /*Check name*/
    if (name) {
        
        if (!field) {
            func_fprintf(f, "Current field hasn't child fields\n");
            return cpd;
        }
        
        /*field is array*/
        if (cpd->is_array) {
            /*get number of elements in the field*/
            int n_elems = vmstate_n_elems(opaque, field);
            int ind = per_get_index(name, cpd->field->name);
            if (ind < 0 || ind >= n_elems) {
                func_fprintf(f, "Invalid field name received\n");
                return cpd;
            }
            
            /*
                *go to need index
            */
            int size = vmstate_size(opaque, field);
            opaque += size * ind;
            
            return Print_information_fields(func_fprintf,f,opaque,field,cpd,name);
            
        } else if (cpd->is_qlist) {
            /*get number of elements in the field*/
            size_t entry_offset = field->start;
            int ind = per_get_index(name, cpd->field->name);
            uint8_t size = get_qlist_size(opaque, entry_offset);
            if (ind < 0 || ind >= size) {
                func_fprintf(f, "Invalid field name received\n");
                return cpd;
            }

            /*
                *go to need index
            */
            void* elm = QTAILQ_RAW_FIRST(opaque);
            uint8_t i = 0;
            while (i < ind) {
                elm = QTAILQ_RAW_NEXT(elm, entry_offset);
                i++;
            }
            
            return Print_information_fields(func_fprintf,f,elm,field,cpd,name);
            
        }
        
        
        /*find file with name = "name"*/
        while (field->name != NULL) {

            if (strcmp(field->name, name)) {
                field++;
                continue;
            }
            curr_elem = opaque + field->offset;
            return Print_information_fields(func_fprintf,f,curr_elem,field,cpd,name);
        }
        
        func_fprintf(f, "Current field hasn't child field with name = \"%s\"\n",name);
        return cpd;
        
    } else {
        while (field->name != NULL) {
            
            curr_elem = opaque + field->offset;
            
            cpd = Print_information_fields(func_fprintf,f,curr_elem,field,cpd,name);
            
            field++;
        }
        return cpd;
    }
    
    // if(name) {
    //     int ind = -1;
    //     int n_elems = vmstate_n_elems(opaque, field);
    //     if(cpd->field && n_elems > 1) {
    //         ind = per_get_index_mas(name,cpd->field->name);
    //         if(ind < 0 || ind >= n_elems) {
    //             func_fprintf(f, "Invalid field name received\n");
    //             return cpd;
    //         }
    //     }
    //     if(ind != -1) {
    //         /*
    //             *go to need index
    //         */
    //         int size = vmstate_size(opaque, field);
    //         opaque += size * ind;
    
    //         // if(field->vmsd) {
    //         //     vmsd = field->vmsd;
    //         //     if(vmsd->fields) {
    //         //         field = vmsd->fields;
    //         //     }
    //         //     cpd = create_next_cpd(cpd,vmsd,field,opaque, name);
    //         // } else {
    //         //     /*я как бы до сих пор смотрю на mas[i]!! надо пофиксить*/
    
    //         //     cpd = Print_information_fields(option,func_fprintf,f,opaque,field,cpd,name,true);
    //         // }
    
    //         cpd = Print_information_fields(option,func_fprintf,f,opaque,field,cpd,name,true);
    
    //         return cpd;
    //     } else {
    //         option = true;
    //     }
    // }
    
    // while (field->name != NULL) {
    
    //     /*search field with name*/
    //     if(option && strcmp(field->name,name)) {
    //         field++;
    //         continue;
    //     }
    
    //     void *curr_elem = opaque + field->offset;
    
    //     cpd = Print_information_fields(option,func_fprintf,f,curr_elem,field,cpd,name,false);
    
    //     field++;
    // }
    // return cpd;
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
