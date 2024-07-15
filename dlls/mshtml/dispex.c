/*
 * Copyright 2008-2009 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "ole2.h"
#include "mscoree.h"

#include "wine/debug.h"

#include "mshtml_private.h"
#include "htmlscript.h"

WINE_DEFAULT_DEBUG_CHANNEL(mshtml);

#define MAX_ARGS 16

static ExternalCycleCollectionParticipant dispex_ccp;

static CRITICAL_SECTION cs_dispex_static_data;
static CRITICAL_SECTION_DEBUG cs_dispex_static_data_dbg =
{
    0, 0, &cs_dispex_static_data,
    { &cs_dispex_static_data_dbg.ProcessLocksList, &cs_dispex_static_data_dbg.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": dispex_static_data") }
};
static CRITICAL_SECTION cs_dispex_static_data = { &cs_dispex_static_data_dbg, -1, 0, 0, 0, 0 };

typedef struct {
    IID iid;
    VARIANT default_value;
} func_arg_info_t;

typedef struct {
    DISPID id;
    BSTR name;
    tid_t tid;
    dispex_hook_invoke_t hook;
    SHORT call_vtbl_off;
    SHORT put_vtbl_off;
    SHORT get_vtbl_off;
    SHORT func_disp_idx;
    USHORT argc;
    USHORT default_value_cnt;
    VARTYPE prop_vt;
    VARTYPE *arg_types;
    func_arg_info_t *arg_info;
} func_info_t;

struct dispex_data_t {
    dispex_static_data_t *desc;
    compat_mode_t compat_mode;

    DWORD func_cnt;
    DWORD func_size;
    func_info_t *funcs;
    func_info_t **name_table;
    DWORD func_disp_cnt;

    struct list entry;
};

typedef struct {
    VARIANT var;
    LPWSTR name;
    DWORD flags;
} dynamic_prop_t;

#define DYNPROP_DELETED    0x01
#define DYNPROP_HIDDEN     0x02

typedef struct {
    DispatchEx dispex;
    DispatchEx *obj;
    func_info_t *info;
} func_disp_t;

typedef struct {
    func_disp_t *func_obj;
    VARIANT val;
} func_obj_entry_t;

struct dispex_dynamic_data_t {
    DWORD buf_size;
    DWORD prop_cnt;
    dynamic_prop_t *props;
    func_obj_entry_t *func_disps;
};

#define DISPID_DYNPROP_0    0x50000000
#define DISPID_DYNPROP_MAX  0x5fffffff

#define FDEX_VERSION_MASK 0xf0000000

static ITypeLib *typelib, *typelib_private;
static ITypeInfo *typeinfos[LAST_tid];
static struct list dispex_data_list = LIST_INIT(dispex_data_list);

static REFIID tid_ids[] = {
#define XIID(iface) &IID_ ## iface,
#define XDIID(iface) &DIID_ ## iface,
TID_LIST
    NULL,
PRIVATE_TID_LIST
#undef XIID
#undef XDIID
};

static HRESULT load_typelib(void)
{
    WCHAR module_path[MAX_PATH + 3];
    HRESULT hres;
    ITypeLib *tl;
    DWORD len;

    hres = LoadRegTypeLib(&LIBID_MSHTML, 4, 0, LOCALE_SYSTEM_DEFAULT, &tl);
    if(FAILED(hres)) {
        ERR("LoadRegTypeLib failed: %08lx\n", hres);
        return hres;
    }

    if(InterlockedCompareExchangePointer((void**)&typelib, tl, NULL))
        ITypeLib_Release(tl);

    len = GetModuleFileNameW(hInst, module_path, MAX_PATH + 1);
    if (!len || len == MAX_PATH + 1)
    {
        ERR("Could not get module file name, len %lu.\n", len);
        return E_FAIL;
    }
    lstrcatW(module_path, L"\\1");

    hres = LoadTypeLibEx(module_path, REGKIND_NONE, &tl);
    if(FAILED(hres)) {
        ERR("LoadTypeLibEx failed for private typelib: %08lx\n", hres);
        return hres;
    }

    if(InterlockedCompareExchangePointer((void**)&typelib_private, tl, NULL))
        ITypeLib_Release(tl);

    return S_OK;
}

static HRESULT get_typeinfo(tid_t tid, ITypeInfo **typeinfo)
{
    HRESULT hres;

    if (!typelib)
        hres = load_typelib();
    if (!typelib)
        return hres;

    if(!typeinfos[tid]) {
        ITypeInfo *ti;

        hres = ITypeLib_GetTypeInfoOfGuid(tid > LAST_public_tid ? typelib_private : typelib, tid_ids[tid], &ti);
        if(FAILED(hres)) {
            ERR("GetTypeInfoOfGuid(%s) failed: %08lx\n", debugstr_mshtml_guid(tid_ids[tid]), hres);
            return hres;
        }

        if(InterlockedCompareExchangePointer((void**)(typeinfos+tid), ti, NULL))
            ITypeInfo_Release(ti);
    }

    *typeinfo = typeinfos[tid];
    return S_OK;
}

void release_typelib(void)
{
    dispex_data_t *iter;
    unsigned i, j;

    while(!list_empty(&dispex_data_list)) {
        iter = LIST_ENTRY(list_head(&dispex_data_list), dispex_data_t, entry);
        list_remove(&iter->entry);

        for(i = 0; i < iter->func_cnt; i++) {
            if(iter->funcs[i].default_value_cnt && iter->funcs[i].arg_info) {
                for(j = 0; j < iter->funcs[i].argc; j++)
                    VariantClear(&iter->funcs[i].arg_info[j].default_value);
            }
            free(iter->funcs[i].arg_types);
            free(iter->funcs[i].arg_info);
            SysFreeString(iter->funcs[i].name);
        }

        free(iter->funcs);
        free(iter->name_table);
        free(iter);
    }

    if(!typelib)
        return;

    for(i=0; i < ARRAY_SIZE(typeinfos); i++)
        if(typeinfos[i])
            ITypeInfo_Release(typeinfos[i]);

    ITypeLib_Release(typelib);
    ITypeLib_Release(typelib_private);
    DeleteCriticalSection(&cs_dispex_static_data);
}

HRESULT get_class_typeinfo(const CLSID *clsid, ITypeInfo **typeinfo)
{
    HRESULT hres;

    if (!typelib)
        hres = load_typelib();
    if (!typelib)
        return hres;

    hres = ITypeLib_GetTypeInfoOfGuid(typelib, clsid, typeinfo);
    if (FAILED(hres))
        hres = ITypeLib_GetTypeInfoOfGuid(typelib_private, clsid, typeinfo);
    if(FAILED(hres))
        ERR("GetTypeInfoOfGuid failed: %08lx\n", hres);
    return hres;
}

/* Not all argument types are supported yet */
#define BUILTIN_ARG_TYPES_SWITCH                        \
    CASE_VT(VT_I2, INT16, V_I2);                        \
    CASE_VT(VT_UI2, UINT16, V_UI2);                     \
    CASE_VT(VT_I4, INT32, V_I4);                        \
    CASE_VT(VT_UI4, UINT32, V_UI4);                     \
    CASE_VT(VT_R4, float, V_R4);                        \
    CASE_VT(VT_BSTR, BSTR, V_BSTR);                     \
    CASE_VT(VT_DISPATCH, IDispatch*, V_DISPATCH);       \
    CASE_VT(VT_BOOL, VARIANT_BOOL, V_BOOL)

/* List all types used by IDispatchEx-based properties */
#define BUILTIN_TYPES_SWITCH                            \
    BUILTIN_ARG_TYPES_SWITCH;                           \
    CASE_VT(VT_VARIANT, VARIANT, *);                    \
    CASE_VT(VT_PTR, void*, V_BYREF);                    \
    CASE_VT(VT_UNKNOWN, IUnknown*, V_UNKNOWN);          \
    CASE_VT(VT_UI8, ULONGLONG, V_UI8)

static BOOL is_arg_type_supported(VARTYPE vt)
{
    switch(vt) {
#define CASE_VT(x,a,b) case x: return TRUE
    BUILTIN_ARG_TYPES_SWITCH;
#undef CASE_VT
    }
    return FALSE;
}

static void add_func_info(dispex_data_t *data, tid_t tid, const FUNCDESC *desc, ITypeInfo *dti,
                          dispex_hook_invoke_t hook, const WCHAR *name_override)
{
    func_info_t *info;
    BSTR name;
    HRESULT hres;

    if(name_override)
        name = SysAllocString(name_override);
    else if(desc->wFuncFlags & FUNCFLAG_FRESTRICTED)
        return;
    else {
        hres = ITypeInfo_GetDocumentation(dti, desc->memid, &name, NULL, NULL, NULL);
        if(FAILED(hres)) {
            WARN("GetDocumentation failed: %08lx\n", hres);
            return;
        }
    }

    for(info = data->funcs; info < data->funcs+data->func_cnt; info++) {
        if(info->id == desc->memid || !wcscmp(info->name, name)) {
            if(info->tid != tid) {
                SysFreeString(name);
                return; /* Duplicated in other interface */
            }
            break;
        }
    }

    TRACE("adding %s...\n", debugstr_w(name));

    if(info == data->funcs+data->func_cnt) {
        if(data->func_cnt == data->func_size) {
            info = realloc(data->funcs, data->func_size * 2 * sizeof(func_info_t));
            if(!info) {
                SysFreeString(name);
                return;
            }
            memset(info + data->func_size, 0, data->func_size * sizeof(func_info_t));
            data->funcs = info;
            data->func_size *= 2;
        }
        info = data->funcs+data->func_cnt;

        data->func_cnt++;

        info->id = desc->memid;
        info->name = name;
        info->tid = tid;
        info->func_disp_idx = -1;
        info->prop_vt = VT_EMPTY;
        info->hook = hook;
    }else {
        SysFreeString(name);
    }

    if(desc->invkind & DISPATCH_METHOD) {
        unsigned i;

        info->func_disp_idx = data->func_disp_cnt++;
        info->argc = desc->cParams;

        assert(info->argc < MAX_ARGS);
        assert(desc->funckind == FUNC_DISPATCH);

        info->arg_info = calloc(info->argc, sizeof(*info->arg_info));
        if(!info->arg_info)
            return;

        info->prop_vt = desc->elemdescFunc.tdesc.vt;
        if(info->prop_vt != VT_VOID && info->prop_vt != VT_PTR && !is_arg_type_supported(info->prop_vt)) {
            TRACE("%s: return type %d\n", debugstr_w(info->name), info->prop_vt);
            return; /* Fallback to ITypeInfo::Invoke */
        }

        info->arg_types = malloc(sizeof(*info->arg_types) * (info->argc + (info->prop_vt == VT_VOID ? 0 : 1)));
        if(!info->arg_types)
            return;

        for(i=0; i < info->argc; i++)
            info->arg_types[i] = desc->lprgelemdescParam[i].tdesc.vt;

        if(info->prop_vt == VT_PTR)
            info->arg_types[info->argc] = VT_BYREF | VT_DISPATCH;
        else if(info->prop_vt != VT_VOID)
            info->arg_types[info->argc] = VT_BYREF | info->prop_vt;

        if(desc->cParamsOpt) {
            TRACE("%s: optional params\n", debugstr_w(info->name));
            return; /* Fallback to ITypeInfo::Invoke */
        }

        for(i=0; i < info->argc; i++) {
            TYPEDESC *tdesc = &desc->lprgelemdescParam[i].tdesc;
            if(tdesc->vt == VT_PTR && tdesc->lptdesc->vt == VT_USERDEFINED) {
                ITypeInfo *ref_type_info;
                TYPEATTR *attr;

                hres = ITypeInfo_GetRefTypeInfo(dti, tdesc->lptdesc->hreftype, &ref_type_info);
                if(FAILED(hres)) {
                    ERR("Could not get referenced type info: %08lx\n", hres);
                    return;
                }

                hres = ITypeInfo_GetTypeAttr(ref_type_info, &attr);
                if(SUCCEEDED(hres)) {
                    assert(attr->typekind == TKIND_DISPATCH);
                    info->arg_info[i].iid = attr->guid;
                    ITypeInfo_ReleaseTypeAttr(ref_type_info, attr);
                }else {
                    ERR("GetTypeAttr failed: %08lx\n", hres);
                }
                ITypeInfo_Release(ref_type_info);
                if(FAILED(hres))
                    return;
                info->arg_types[i] = VT_DISPATCH;
            }else if(!is_arg_type_supported(info->arg_types[i])) {
                TRACE("%s: unsupported arg type %s\n", debugstr_w(info->name), debugstr_vt(info->arg_types[i]));
                return; /* Fallback to ITypeInfo for unsupported arg types */
            }

            if(desc->lprgelemdescParam[i].paramdesc.wParamFlags & PARAMFLAG_FHASDEFAULT) {
                hres = VariantCopy(&info->arg_info[i].default_value,
                                   &desc->lprgelemdescParam[i].paramdesc.pparamdescex->varDefaultValue);
                if(FAILED(hres)) {
                    ERR("Could not copy default value: %08lx\n", hres);
                    return;
                }
                TRACE("%s param %d: default value %s\n", debugstr_w(info->name),
                      i, debugstr_variant(&info->arg_info[i].default_value));
                info->default_value_cnt++;
            }
        }

        assert(info->argc <= MAX_ARGS);
        assert(desc->callconv == CC_STDCALL);

        info->call_vtbl_off = desc->oVft/sizeof(void*);
    }else if(desc->invkind & (DISPATCH_PROPERTYPUT|DISPATCH_PROPERTYGET)) {
        VARTYPE vt = VT_EMPTY;

        if(desc->wFuncFlags & FUNCFLAG_FHIDDEN)
            info->func_disp_idx = -2;

        if(desc->invkind & DISPATCH_PROPERTYGET) {
            vt = desc->elemdescFunc.tdesc.vt;
            info->get_vtbl_off = desc->oVft/sizeof(void*);
        }
        if(desc->invkind & DISPATCH_PROPERTYPUT) {
            assert(desc->cParams == 1);
            vt = desc->lprgelemdescParam->tdesc.vt;
            info->put_vtbl_off = desc->oVft/sizeof(void*);
        }

        assert(info->prop_vt == VT_EMPTY || vt == info->prop_vt);
        info->prop_vt = vt;
    }
}

static HRESULT process_interface(dispex_data_t *data, tid_t tid, ITypeInfo *disp_typeinfo, const dispex_hook_t *hooks)
{
    unsigned i = 7; /* skip IDispatch functions */
    ITypeInfo *typeinfo;
    FUNCDESC *funcdesc;
    HRESULT hres;

    hres = get_typeinfo(tid, &typeinfo);
    if(FAILED(hres))
        return hres;

    while(1) {
        const dispex_hook_t *hook = NULL;

        hres = ITypeInfo_GetFuncDesc(typeinfo, i++, &funcdesc);
        if(FAILED(hres))
            break;

        if(hooks) {
            for(hook = hooks; hook->dispid != DISPID_UNKNOWN; hook++) {
                if(hook->dispid == funcdesc->memid)
                    break;
            }
            if(hook->dispid == DISPID_UNKNOWN)
                hook = NULL;
        }

        if(!hook || hook->invoke || hook->name) {
            add_func_info(data, tid, funcdesc, disp_typeinfo ? disp_typeinfo : typeinfo,
                          hook ? hook->invoke : NULL, hook ? hook->name : NULL);
        }

        ITypeInfo_ReleaseFuncDesc(typeinfo, funcdesc);
    }

    return S_OK;
}

void dispex_info_add_interface(dispex_data_t *info, tid_t tid, const dispex_hook_t *hooks)
{
    HRESULT hres;

    hres = process_interface(info, tid, NULL, hooks);
    if(FAILED(hres))
        ERR("process_interface failed: %08lx\n", hres);
}

static int __cdecl dispid_cmp(const void *p1, const void *p2)
{
    return ((const func_info_t*)p1)->id - ((const func_info_t*)p2)->id;
}

static int __cdecl func_name_cmp(const void *p1, const void *p2)
{
    return wcsicmp((*(func_info_t* const*)p1)->name, (*(func_info_t* const*)p2)->name);
}

static dispex_data_t *preprocess_dispex_data(dispex_static_data_t *desc, compat_mode_t compat_mode)
{
    const tid_t *tid;
    dispex_data_t *data;
    DWORD i;
    ITypeInfo *dti;
    HRESULT hres;

    if(desc->disp_tid) {
        hres = get_typeinfo(desc->disp_tid, &dti);
        if(FAILED(hres)) {
            ERR("Could not get disp type info: %08lx\n", hres);
            return NULL;
        }
    }

    data = malloc(sizeof(dispex_data_t));
    if (!data) {
        ERR("Out of memory\n");
        return NULL;
    }
    data->desc = desc;
    data->compat_mode = compat_mode;
    data->func_cnt = 0;
    data->func_disp_cnt = 0;
    data->func_size = 16;
    data->funcs = calloc(data->func_size, sizeof(func_info_t));
    if (!data->funcs) {
        free(data);
        ERR("Out of memory\n");
        return NULL;
    }
    list_add_tail(&dispex_data_list, &data->entry);

    if(desc->init_info)
        desc->init_info(data, compat_mode);

    for(tid = desc->iface_tids; *tid; tid++) {
        hres = process_interface(data, *tid, dti, NULL);
        if(FAILED(hres))
            break;
    }

    if(!data->func_cnt) {
        free(data->funcs);
        data->name_table = NULL;
        data->funcs = NULL;
        data->func_size = 0;
        return data;
    }


    data->funcs = realloc(data->funcs, data->func_cnt * sizeof(func_info_t));
    qsort(data->funcs, data->func_cnt, sizeof(func_info_t), dispid_cmp);

    data->name_table = malloc(data->func_cnt * sizeof(func_info_t*));
    for(i=0; i < data->func_cnt; i++)
        data->name_table[i] = data->funcs+i;
    qsort(data->name_table, data->func_cnt, sizeof(func_info_t*), func_name_cmp);
    return data;
}

static int __cdecl id_cmp(const void *p1, const void *p2)
{
    return *(const DISPID*)p1 - *(const DISPID*)p2;
}

HRESULT get_dispids(tid_t tid, DWORD *ret_size, DISPID **ret)
{
    unsigned i, func_cnt;
    FUNCDESC *funcdesc;
    ITypeInfo *ti;
    TYPEATTR *attr;
    DISPID *ids;
    HRESULT hres;

    hres = get_typeinfo(tid, &ti);
    if(FAILED(hres))
        return hres;

    hres = ITypeInfo_GetTypeAttr(ti, &attr);
    if(FAILED(hres)) {
        ITypeInfo_Release(ti);
        return hres;
    }

    func_cnt = attr->cFuncs;
    ITypeInfo_ReleaseTypeAttr(ti, attr);

    ids = malloc(func_cnt * sizeof(DISPID));
    if(!ids) {
        ITypeInfo_Release(ti);
        return E_OUTOFMEMORY;
    }

    for(i=0; i < func_cnt; i++) {
        hres = ITypeInfo_GetFuncDesc(ti, i, &funcdesc);
        if(FAILED(hres))
            break;

        ids[i] = funcdesc->memid;
        ITypeInfo_ReleaseFuncDesc(ti, funcdesc);
    }

    ITypeInfo_Release(ti);
    if(FAILED(hres)) {
        free(ids);
        return hres;
    }

    qsort(ids, func_cnt, sizeof(DISPID), id_cmp);

    *ret_size = func_cnt;
    *ret = ids;
    return S_OK;
}

static inline BOOL is_custom_dispid(DISPID id)
{
    return MSHTML_DISPID_CUSTOM_MIN <= id && id <= MSHTML_DISPID_CUSTOM_MAX;
}

static inline BOOL is_dynamic_dispid(DISPID id)
{
    return DISPID_DYNPROP_0 <= id && id <= DISPID_DYNPROP_MAX;
}

dispex_prop_type_t get_dispid_type(DISPID id)
{
    if(is_dynamic_dispid(id))
        return DISPEXPROP_DYNAMIC;
    if(is_custom_dispid(id))
        return DISPEXPROP_CUSTOM;
    return DISPEXPROP_BUILTIN;
}

static HRESULT variant_copy(VARIANT *dest, VARIANT *src)
{
    if(V_VT(src) == VT_BSTR && !V_BSTR(src)) {
        V_VT(dest) = VT_BSTR;
        V_BSTR(dest) = NULL;
        return S_OK;
    }

    return VariantCopy(dest, src);
}

static inline dispex_dynamic_data_t *get_dynamic_data(DispatchEx *This)
{
    if(This->dynamic_data)
        return This->dynamic_data;

    This->dynamic_data = calloc(1, sizeof(dispex_dynamic_data_t));
    if(!This->dynamic_data)
        return NULL;

    if(This->info->desc->vtbl->populate_props)
        This->info->desc->vtbl->populate_props(This);

    return This->dynamic_data;
}

static HRESULT alloc_dynamic_prop(DispatchEx *This, const WCHAR *name, dynamic_prop_t *prop, dynamic_prop_t **ret)
{
    dispex_dynamic_data_t *data = This->dynamic_data;

    if(prop) {
        prop->flags &= ~DYNPROP_DELETED;
        *ret = prop;
        return S_OK;
    }

    if(!data->buf_size) {
        data->props = malloc(sizeof(dynamic_prop_t) * 4);
        if(!data->props)
            return E_OUTOFMEMORY;
        data->buf_size = 4;
    }else if(data->buf_size == data->prop_cnt) {
        dynamic_prop_t *new_props;

        new_props = realloc(data->props, sizeof(dynamic_prop_t) * (data->buf_size << 1));
        if(!new_props)
            return E_OUTOFMEMORY;

        data->props = new_props;
        data->buf_size <<= 1;
    }

    prop = data->props + data->prop_cnt;

    prop->name = wcsdup(name);
    if(!prop->name)
        return E_OUTOFMEMORY;

    VariantInit(&prop->var);
    prop->flags = 0;
    data->prop_cnt++;
    *ret = prop;
    return S_OK;
}

static HRESULT get_dynamic_prop(DispatchEx *This, const WCHAR *name, DWORD flags, dynamic_prop_t **ret)
{
    const BOOL alloc = flags & fdexNameEnsure;
    dispex_dynamic_data_t *data;
    dynamic_prop_t *prop;

    data = get_dynamic_data(This);
    if(!data)
        return E_OUTOFMEMORY;

    for(prop = data->props; prop < data->props+data->prop_cnt; prop++) {
        if(flags & fdexNameCaseInsensitive ? !wcsicmp(prop->name, name) : !wcscmp(prop->name, name)) {
            *ret = prop;
            if(prop->flags & DYNPROP_DELETED) {
                if(!alloc)
                    return DISP_E_UNKNOWNNAME;
                prop->flags &= ~DYNPROP_DELETED;
            }
            return S_OK;
        }
    }

    if(!alloc)
        return DISP_E_UNKNOWNNAME;

    TRACE("creating dynamic prop %s\n", debugstr_w(name));
    return alloc_dynamic_prop(This, name, NULL, ret);
}

HRESULT dispex_get_dprop_ref(DispatchEx *This, const WCHAR *name, BOOL alloc, VARIANT **ret)
{
    dynamic_prop_t *prop;
    HRESULT hres;

    hres = get_dynamic_prop(This, name, alloc ? fdexNameEnsure : 0, &prop);
    if(FAILED(hres))
        return hres;

    if(alloc)
        prop->flags |= DYNPROP_HIDDEN;
    *ret = &prop->var;
    return S_OK;
}

HRESULT dispex_get_dynid(DispatchEx *This, const WCHAR *name, BOOL hidden, DISPID *id)
{
    dynamic_prop_t *prop;
    HRESULT hres;

    hres = get_dynamic_prop(This, name, fdexNameEnsure, &prop);
    if(FAILED(hres))
        return hres;

    if(hidden)
        prop->flags |= DYNPROP_HIDDEN;
    *id = DISPID_DYNPROP_0 + (prop - This->dynamic_data->props);
    return S_OK;
}

static HRESULT dispex_value(DispatchEx *This, LCID lcid, WORD flags, DISPPARAMS *params,
        VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    HRESULT hres;

    if(This->info->desc->vtbl->value)
        return This->info->desc->vtbl->value(This, lcid, flags, params, res, ei, caller);

    switch(flags) {
    case DISPATCH_PROPERTYGET:
        V_VT(res) = VT_BSTR;
        hres = dispex_to_string(This, &V_BSTR(res));
        if(FAILED(hres))
            return hres;
        break;
    default:
        FIXME("Unimplemented flags %x\n", flags);
        return E_NOTIMPL;
    }

    return S_OK;
}

static HRESULT typeinfo_invoke(DispatchEx *This, func_info_t *func, WORD flags, DISPPARAMS *dp, VARIANT *res,
        EXCEPINFO *ei)
{
    DISPPARAMS params = {dp->rgvarg, NULL, dp->cArgs, 0};
    ITypeInfo *ti;
    IUnknown *unk;
    UINT argerr=0;
    HRESULT hres;

    if(params.cArgs > func->argc) {
        params.rgvarg += params.cArgs - func->argc;
        params.cArgs = func->argc;
    }

    hres = get_typeinfo(func->tid, &ti);
    if(FAILED(hres)) {
        ERR("Could not get type info: %08lx\n", hres);
        return hres;
    }

    hres = IWineJSDispatchHost_QueryInterface(&This->IWineJSDispatchHost_iface, tid_ids[func->tid], (void**)&unk);
    if(FAILED(hres)) {
        ERR("Could not get iface %s: %08lx\n", debugstr_mshtml_guid(tid_ids[func->tid]), hres);
        return E_FAIL;
    }

    hres = ITypeInfo_Invoke(ti, unk, func->id, flags, &params, res, ei, &argerr);

    IUnknown_Release(unk);
    return hres;
}

static HRESULT get_disp_prop(IDispatchEx *dispex, const WCHAR *name, LCID lcid, VARIANT *res,
        EXCEPINFO *ei, IServiceProvider *caller)
{
    DISPPARAMS dp = { 0 };
    DISPID dispid;
    HRESULT hres;
    BSTR bstr;

    if(!(bstr = SysAllocString(name)))
        return E_OUTOFMEMORY;
    hres = IDispatchEx_GetDispID(dispex, bstr, fdexNameCaseSensitive, &dispid);
    SysFreeString(bstr);
    if(SUCCEEDED(hres))
        hres = IDispatchEx_InvokeEx(dispex, dispid, lcid, DISPATCH_PROPERTYGET, &dp, res, ei, caller);
    return hres;
}

static HRESULT function_apply(func_disp_t *func, DISPPARAMS *dp, LCID lcid, VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    IWineJSDispatchHost *this_iface;
    DISPPARAMS params = { 0 };
    IDispatchEx *array = NULL;
    UINT argc = 0;
    VARIANT *arg;
    HRESULT hres;

    arg = dp->rgvarg + dp->cArgs - 1;
    if(dp->cArgs < 1 || V_VT(arg) != VT_DISPATCH || !V_DISPATCH(arg))
        return CTL_E_ILLEGALFUNCTIONCALL;

    hres = IDispatch_QueryInterface(V_DISPATCH(arg), &IID_IWineJSDispatchHost, (void**)&this_iface);
    if(FAILED(hres))
        return CTL_E_ILLEGALFUNCTIONCALL;

    if(dp->cArgs >= 2) {
        VARIANT length;

        arg--;
        if(V_VT(arg) != VT_DISPATCH) {
            hres = CTL_E_ILLEGALFUNCTIONCALL;
            goto fail;
        }

        /* FIXME: Native checks if it's an acual JS array. */
        hres = IDispatch_QueryInterface(V_DISPATCH(arg), &IID_IDispatchEx, (void**)&array);
        if(FAILED(hres))
            goto fail;

        V_VT(&length) = VT_EMPTY;
        hres = get_disp_prop(array, L"length", lcid, &length, ei, caller);
        if(FAILED(hres)) {
            if(hres == DISP_E_UNKNOWNNAME)
                hres = CTL_E_ILLEGALFUNCTIONCALL;
            goto fail;
        }
        if(V_VT(&length) != VT_I4) {
            VARIANT tmp = length;
            hres = change_type(&length, &tmp, VT_I4, caller);
            if(FAILED(hres)) {
                hres = CTL_E_ILLEGALFUNCTIONCALL;
                goto fail;
            }
        }
        if(V_I4(&length) < 0) {
            hres = CTL_E_ILLEGALFUNCTIONCALL;
            goto fail;
        }
        params.cArgs = V_I4(&length);

        /* alloc new params */
        if(params.cArgs) {
            if(!(params.rgvarg = malloc(params.cArgs * sizeof(VARIANTARG)))) {
                hres = E_OUTOFMEMORY;
                goto fail;
            }
            for(argc = 0; argc < params.cArgs; argc++) {
                WCHAR buf[12];

                arg = params.rgvarg + params.cArgs - argc - 1;
                swprintf(buf, ARRAY_SIZE(buf), L"%u", argc);
                hres = get_disp_prop(array, buf, lcid, arg, ei, caller);
                if(FAILED(hres)) {
                    if(hres == DISP_E_UNKNOWNNAME) {
                        V_VT(arg) = VT_EMPTY;
                        continue;
                    }
                    goto fail;
                }
            }
        }
    }

    hres = IWineJSDispatchHost_CallFunction(this_iface, func->info->id, func->info->tid, &params, res, ei, caller);

fail:
    while(argc--)
        VariantClear(&params.rgvarg[params.cArgs - argc - 1]);
    free(params.rgvarg);
    if(array)
        IDispatchEx_Release(array);
    IWineJSDispatchHost_Release(this_iface);
    return hres == E_UNEXPECTED ? CTL_E_ILLEGALFUNCTIONCALL : hres;
}

static HRESULT function_call(func_disp_t *func, DISPPARAMS *dp, LCID lcid, VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    DISPPARAMS params = { dp->rgvarg, NULL, dp->cArgs - 1, 0 };
    IWineJSDispatchHost *this_iface;
    VARIANT *arg;
    HRESULT hres;

    arg = dp->rgvarg + dp->cArgs - 1;
    if(dp->cArgs < 1 || V_VT(arg) != VT_DISPATCH || !V_DISPATCH(arg))
        return CTL_E_ILLEGALFUNCTIONCALL;

    hres = IDispatch_QueryInterface(V_DISPATCH(arg), &IID_IWineJSDispatchHost, (void**)&this_iface);
    if(FAILED(hres))
        return CTL_E_ILLEGALFUNCTIONCALL;

    hres = IWineJSDispatchHost_CallFunction(this_iface, func->info->id, func->info->tid, &params, res, ei, caller);
    IWineJSDispatchHost_Release(this_iface);
    return (hres == E_UNEXPECTED) ? CTL_E_ILLEGALFUNCTIONCALL : hres;
}

static const struct {
    const WCHAR *name;
    HRESULT (*invoke)(func_disp_t*,DISPPARAMS*,LCID,VARIANT*,EXCEPINFO*,IServiceProvider*);
} function_props[] = {
    { L"apply", function_apply },
    { L"call",  function_call }
};

static inline func_disp_t *impl_from_DispatchEx(DispatchEx *iface)
{
    return CONTAINING_RECORD(iface, func_disp_t, dispex);
}

static void function_destructor(DispatchEx *dispex)
{
    func_disp_t *This = impl_from_DispatchEx(dispex);
    assert(!This->obj);
    free(This);
}

static HRESULT function_value(DispatchEx *dispex, LCID lcid, WORD flags, DISPPARAMS *params,
        VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    func_disp_t *This = impl_from_DispatchEx(dispex);
    HRESULT hres;

    switch(flags) {
    case DISPATCH_METHOD|DISPATCH_PROPERTYGET:
        if(!res)
            return E_INVALIDARG;
        /* fall through */
    case DISPATCH_METHOD:
        if(!This->obj)
            return E_UNEXPECTED;
        hres = dispex_call_builtin(This->obj, This->info->id, params, res, ei, caller);
        break;
    case DISPATCH_PROPERTYGET: {
        unsigned name_len;
        WCHAR *ptr;
        BSTR str;

        static const WCHAR func_prefixW[] =
            {'\n','f','u','n','c','t','i','o','n',' '};
        static const WCHAR func_suffixW[] =
            {'(',')',' ','{','\n',' ',' ',' ',' ','[','n','a','t','i','v','e',' ','c','o','d','e',']','\n','}','\n'};

        /* FIXME: This probably should be more generic. Also we should try to get IID_IActiveScriptSite and SID_GetCaller. */
        if(!caller)
            return E_ACCESSDENIED;

        name_len = SysStringLen(This->info->name);
        ptr = str = SysAllocStringLen(NULL, name_len + ARRAY_SIZE(func_prefixW) + ARRAY_SIZE(func_suffixW));
        if(!str)
            return E_OUTOFMEMORY;

        memcpy(ptr, func_prefixW, sizeof(func_prefixW));
        ptr += ARRAY_SIZE(func_prefixW);

        memcpy(ptr, This->info->name, name_len*sizeof(WCHAR));
        ptr += name_len;

        memcpy(ptr, func_suffixW, sizeof(func_suffixW));

        V_VT(res) = VT_BSTR;
        V_BSTR(res) = str;
        return S_OK;
    }
    default:
        FIXME("Unimplemented flags %x\n", flags);
        hres = E_NOTIMPL;
    }

    return hres;
}

static HRESULT function_get_dispid(DispatchEx *dispex, const WCHAR *name, DWORD flags, DISPID *dispid)
{
    DWORD i;

    for(i = 0; i < ARRAY_SIZE(function_props); i++) {
        if((flags & fdexNameCaseInsensitive) ? wcsicmp(name, function_props[i].name) : wcscmp(name, function_props[i].name))
            continue;
        *dispid = MSHTML_DISPID_CUSTOM_MIN + i;
        return S_OK;
    }
    return DISP_E_UNKNOWNNAME;
}

static HRESULT function_get_name(DispatchEx *dispex, DISPID id, BSTR *name)
{
    DWORD idx = id - MSHTML_DISPID_CUSTOM_MIN;

    if(idx >= ARRAY_SIZE(function_props))
        return DISP_E_MEMBERNOTFOUND;

    return (*name = SysAllocString(function_props[idx].name)) ? S_OK : E_OUTOFMEMORY;
}

static HRESULT function_invoke(DispatchEx *dispex, DISPID id, LCID lcid, WORD flags, DISPPARAMS *params,
        VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    func_disp_t *This = impl_from_DispatchEx(dispex);
    DWORD idx = id - MSHTML_DISPID_CUSTOM_MIN;

    if(idx >= ARRAY_SIZE(function_props))
        return DISP_E_MEMBERNOTFOUND;

    switch(flags) {
    case DISPATCH_METHOD|DISPATCH_PROPERTYGET:
        if(!res)
            return E_INVALIDARG;
        /* fall through */
    case DISPATCH_METHOD:
        return function_props[idx].invoke(This, params, lcid, res, ei, caller);
    default:
        return MSHTML_E_INVALID_PROPERTY;
    }

    return S_OK;
}

static const dispex_static_data_vtbl_t function_dispex_vtbl = {
    .destructor       = function_destructor,
    .value            = function_value,
    .get_dispid       = function_get_dispid,
    .get_name         = function_get_name,
    .invoke           = function_invoke
};

static const tid_t function_iface_tids[] = {0};

static dispex_static_data_t function_dispex = {
    "Function",
    &function_dispex_vtbl,
    NULL_tid,
    function_iface_tids
};

static func_disp_t *create_func_disp(DispatchEx *obj, func_info_t *info)
{
    func_disp_t *ret;

    ret = calloc(1, sizeof(func_disp_t));
    if(!ret)
        return NULL;

    init_dispatch(&ret->dispex, &function_dispex, NULL, dispex_compat_mode(obj));
    ret->obj = obj;
    ret->info = info;

    return ret;
}

static HRESULT invoke_disp_value(DispatchEx *This, IDispatch *func_disp, LCID lcid, WORD flags, DISPPARAMS *dp,
        VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    DISPID named_arg = DISPID_THIS;
    DISPPARAMS new_dp = {NULL, &named_arg, 0, 1};
    IDispatchEx *dispex;
    HRESULT hres;

    if(dp->cNamedArgs) {
        FIXME("named args not supported\n");
        return E_NOTIMPL;
    }

    new_dp.rgvarg = malloc((dp->cArgs + 1) * sizeof(VARIANTARG));
    if(!new_dp.rgvarg)
        return E_OUTOFMEMORY;

    new_dp.cArgs = dp->cArgs+1;
    memcpy(new_dp.rgvarg+1, dp->rgvarg, dp->cArgs*sizeof(VARIANTARG));

    V_VT(new_dp.rgvarg) = VT_DISPATCH;
    V_DISPATCH(new_dp.rgvarg) = (IDispatch*)&This->IWineJSDispatchHost_iface;

    hres = IDispatch_QueryInterface(func_disp, &IID_IDispatchEx, (void**)&dispex);
    TRACE(">>>\n");
    if(SUCCEEDED(hres)) {
        hres = IDispatchEx_InvokeEx(dispex, DISPID_VALUE, lcid, flags, &new_dp, res, ei, caller);
        IDispatchEx_Release(dispex);
    }else {
        UINT err = 0;
        hres = IDispatch_Invoke(func_disp, DISPID_VALUE, &IID_NULL, lcid, flags, &new_dp, res, ei, &err);
    }
    if(SUCCEEDED(hres))
        TRACE("<<< %s\n", debugstr_variant(res));
    else
        WARN("<<< %08lx\n", hres);

    free(new_dp.rgvarg);
    return hres;
}

static HRESULT get_func_obj_entry(DispatchEx *This, func_info_t *func, func_obj_entry_t **ret)
{
    dispex_dynamic_data_t *dynamic_data;
    func_obj_entry_t *entry;

    dynamic_data = get_dynamic_data(This);
    if(!dynamic_data)
        return E_OUTOFMEMORY;

    if(!dynamic_data->func_disps) {
        dynamic_data->func_disps = calloc(This->info->func_disp_cnt, sizeof(*dynamic_data->func_disps));
        if(!dynamic_data->func_disps)
            return E_OUTOFMEMORY;
    }

    entry = dynamic_data->func_disps + func->func_disp_idx;
    if(!entry->func_obj) {
        entry->func_obj = create_func_disp(This, func);
        if(!entry->func_obj)
            return E_OUTOFMEMORY;

        IWineJSDispatchHost_AddRef(&entry->func_obj->dispex.IWineJSDispatchHost_iface);
        V_VT(&entry->val) = VT_DISPATCH;
        V_DISPATCH(&entry->val) = (IDispatch*)&entry->func_obj->dispex.IWineJSDispatchHost_iface;
    }

    *ret = entry;
    return S_OK;
}

static HRESULT get_builtin_func(dispex_data_t *data, DISPID id, func_info_t **ret)
{
    int min, max, n;

    min = 0;
    max = data->func_cnt-1;

    while(min <= max) {
        n = (min+max)/2;

        if(data->funcs[n].id == id) {
            *ret = data->funcs+n;
            return S_OK;
        }

        if(data->funcs[n].id < id)
            min = n+1;
        else
            max = n-1;
    }

    WARN("invalid id %lx\n", id);
    return DISP_E_MEMBERNOTFOUND;
}

static HRESULT get_builtin_id(DispatchEx *This, const WCHAR *name, DWORD grfdex, DISPID *ret)
{
    int min, max, n, c;
    HRESULT hres;

    if(This->info->desc->vtbl->lookup_dispid) {
        hres = This->info->desc->vtbl->lookup_dispid(This, name, grfdex, ret);
        if(hres != DISP_E_UNKNOWNNAME)
            return hres;
    }

    min = 0;
    max = This->info->func_cnt-1;

    while(min <= max) {
        n = (min+max)/2;

        c = wcsicmp(This->info->name_table[n]->name, name);
        if(!c) {
            if((grfdex & fdexNameCaseSensitive) && wcscmp(This->info->name_table[n]->name, name))
                break;

            *ret = This->info->name_table[n]->id;
            return S_OK;
        }

        if(c > 0)
            max = n-1;
        else
            min = n+1;
    }

    if(This->info->desc->vtbl->get_dispid) {
        hres = This->info->desc->vtbl->get_dispid(This, name, grfdex, ret);
        if(hres != DISP_E_UNKNOWNNAME)
            return hres;
    }

    return DISP_E_UNKNOWNNAME;
}

HRESULT change_type(VARIANT *dst, VARIANT *src, VARTYPE vt, IServiceProvider *caller)
{
    V_VT(dst) = VT_EMPTY;

    if(caller) {
        IVariantChangeType *change_type = NULL;
        HRESULT hres;

        hres = IServiceProvider_QueryService(caller, &SID_VariantConversion, &IID_IVariantChangeType, (void**)&change_type);
        if(SUCCEEDED(hres)) {
            hres = IVariantChangeType_ChangeType(change_type, dst, src, LOCALE_NEUTRAL, vt);
            IVariantChangeType_Release(change_type);
            if(SUCCEEDED(hres))
                return S_OK;
        }
    }

    switch(vt) {
    case VT_BOOL:
        if(V_VT(src) == VT_BSTR) {
            V_VT(dst) = VT_BOOL;
            V_BOOL(dst) = variant_bool(V_BSTR(src) && *V_BSTR(src));
            return S_OK;
        }
        break;
    case VT_UNKNOWN:
    case VT_DISPATCH:
        if(V_VT(src) == VT_EMPTY || V_VT(src) == VT_NULL) {
            V_VT(dst) = vt;
            V_DISPATCH(dst) = NULL;
            return S_OK;
        }
        break;
    }

    return VariantChangeType(dst, src, 0, vt);
}

static HRESULT builtin_propget(DispatchEx *This, func_info_t *func, DISPPARAMS *dp, VARIANT *res)
{
    IUnknown *iface;
    HRESULT hres;

    if(dp && dp->cArgs) {
        FIXME("cArgs %d\n", dp->cArgs);
        return E_NOTIMPL;
    }

    assert(func->get_vtbl_off);

    hres = IWineJSDispatchHost_QueryInterface(&This->IWineJSDispatchHost_iface, tid_ids[func->tid], (void**)&iface);
    if(SUCCEEDED(hres)) {
        switch(func->prop_vt) {
#define CASE_VT(vt,type,access) \
        case vt: { \
            type val; \
            hres = ((HRESULT (WINAPI*)(IUnknown*,type*))((void**)iface->lpVtbl)[func->get_vtbl_off])(iface,&val); \
            if(SUCCEEDED(hres)) \
                access(res) = val; \
            } \
            break
        BUILTIN_TYPES_SWITCH;
#undef CASE_VT
        default:
            FIXME("Unhandled vt %d\n", func->prop_vt);
            hres = E_NOTIMPL;
        }
        IUnknown_Release(iface);
    }

    if(FAILED(hres))
        return hres;

    if(func->prop_vt != VT_VARIANT)
        V_VT(res) = func->prop_vt == VT_PTR ? VT_DISPATCH : func->prop_vt;
    return S_OK;
}

static HRESULT builtin_propput(DispatchEx *This, func_info_t *func, DISPPARAMS *dp, IServiceProvider *caller)
{
    VARIANT *v, tmpv;
    IUnknown *iface;
    HRESULT hres;

    if(dp->cArgs != 1 || (dp->cNamedArgs == 1 && *dp->rgdispidNamedArgs != DISPID_PROPERTYPUT)
            || dp->cNamedArgs > 1) {
        FIXME("invalid args\n");
        return E_INVALIDARG;
    }

    if(!func->put_vtbl_off) {
        if(dispex_compat_mode(This) >= COMPAT_MODE_IE9) {
            WARN("No setter\n");
            return S_OK;
        }
        FIXME("No setter\n");
        return E_FAIL;
    }

    v = dp->rgvarg;
    if(func->prop_vt != VT_VARIANT && V_VT(v) != func->prop_vt) {
        hres = change_type(&tmpv, v, func->prop_vt, caller);
        if(FAILED(hres))
            return hres;
        v = &tmpv;
    }

    hres = IWineJSDispatchHost_QueryInterface(&This->IWineJSDispatchHost_iface, tid_ids[func->tid], (void**)&iface);
    if(SUCCEEDED(hres)) {
        switch(func->prop_vt) {
#define CASE_VT(vt,type,access) \
        case vt: \
            hres = ((HRESULT (WINAPI*)(IUnknown*,type))((void**)iface->lpVtbl)[func->put_vtbl_off])(iface,access(v)); \
            break
        BUILTIN_TYPES_SWITCH;
#undef CASE_VT
        default:
            FIXME("Unimplemented vt %d\n", func->prop_vt);
            hres = E_NOTIMPL;
        }

        IUnknown_Release(iface);
    }

    if(v == &tmpv)
        VariantClear(v);
    return hres;
}

static HRESULT call_builtin_function(DispatchEx *This, func_info_t *func, DISPPARAMS *dp,
                                     VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    VARIANT arg_buf[MAX_ARGS], *arg_ptrs[MAX_ARGS], *arg, retv, ret_ref, vhres;
    unsigned i, nconv = 0;
    IUnknown *iface;
    HRESULT hres;

    if(func->hook) {
        hres = func->hook(This, DISPATCH_METHOD, dp, res, ei, caller);
        if(hres != S_FALSE)
            return hres;
    }

    if(!func->call_vtbl_off)
        return typeinfo_invoke(This, func, DISPATCH_METHOD, dp, res, ei);

    if(dp->cArgs + func->default_value_cnt < func->argc) {
        FIXME("Invalid argument count (expected %u, got %u)\n", func->argc, dp->cArgs);
        return E_INVALIDARG;
    }

    hres = IWineJSDispatchHost_QueryInterface(&This->IWineJSDispatchHost_iface, tid_ids[func->tid], (void**)&iface);
    if(FAILED(hres))
        return hres;

    for(i=0; i < func->argc; i++) {
        BOOL own_value = FALSE;
        if(i >= dp->cArgs) {
            /* use default value */
            arg_ptrs[i] = &func->arg_info[i].default_value;
            continue;
        }
        arg = dp->rgvarg+dp->cArgs-i-1;
        if(func->arg_types[i] == V_VT(arg)) {
            arg_ptrs[i] = arg;
        }else {
            hres = change_type(arg_buf+nconv, arg, func->arg_types[i], caller);
            if(FAILED(hres))
                break;
            arg_ptrs[i] = arg_buf + nconv++;
            own_value = TRUE;
        }

        if(func->arg_types[i] == VT_DISPATCH && !IsEqualGUID(&func->arg_info[i].iid, &IID_NULL)
            && V_DISPATCH(arg_ptrs[i])) {
            IDispatch *iface;
            if(!own_value) {
                arg_buf[nconv] = *arg_ptrs[i];
                arg_ptrs[i] = arg_buf + nconv++;
            }
            hres = IDispatch_QueryInterface(V_DISPATCH(arg_ptrs[i]), &func->arg_info[i].iid, (void**)&iface);
            if(FAILED(hres)) {
                WARN("Could not get %s iface: %08lx\n", debugstr_guid(&func->arg_info[i].iid), hres);
                break;
            }
            if(own_value)
                IDispatch_Release(V_DISPATCH(arg_ptrs[i]));
            V_DISPATCH(arg_ptrs[i]) = iface;
        }
    }

    if(SUCCEEDED(hres)) {
        if(func->prop_vt == VT_VOID) {
            V_VT(&retv) = VT_EMPTY;
        }else {
            V_VT(&retv) = func->prop_vt;
            arg_ptrs[func->argc] = &ret_ref;
            V_VT(&ret_ref) = VT_BYREF|func->prop_vt;

            switch(func->prop_vt) {
#define CASE_VT(vt,type,access)                         \
            case vt:                                    \
                V_BYREF(&ret_ref) = &access(&retv);     \
                break
            BUILTIN_ARG_TYPES_SWITCH;
#undef CASE_VT
            case VT_PTR:
                V_VT(&retv) = VT_DISPATCH;
                V_VT(&ret_ref) = VT_BYREF | VT_DISPATCH;
                V_BYREF(&ret_ref) = &V_DISPATCH(&retv);
                break;
            default:
                assert(0);
            }
        }

        V_VT(&vhres) = VT_ERROR;
        hres = DispCallFunc(iface, func->call_vtbl_off*sizeof(void*), CC_STDCALL, VT_ERROR,
                    func->argc + (func->prop_vt == VT_VOID ? 0 : 1), func->arg_types, arg_ptrs, &vhres);
    }

    while(nconv--)
        VariantClear(arg_buf+nconv);
    IUnknown_Release(iface);
    if(FAILED(hres))
        return hres;
    if(FAILED(V_ERROR(&vhres)))
        return V_ERROR(&vhres);

    if(res)
        *res = retv;
    else
        VariantClear(&retv);
    return V_ERROR(&vhres);
}

static HRESULT invoke_builtin_function(DispatchEx *This, func_info_t *func, WORD flags, DISPPARAMS *dp, VARIANT *res,
        EXCEPINFO *ei, IServiceProvider *caller)
{
    HRESULT hres;

    switch(flags) {
    case DISPATCH_METHOD|DISPATCH_PROPERTYGET:
        if(!res)
            return E_INVALIDARG;
        /* fall through */
    case DISPATCH_METHOD:
        if(This->dynamic_data && This->dynamic_data->func_disps
           && This->dynamic_data->func_disps[func->func_disp_idx].func_obj) {
            func_obj_entry_t *entry = This->dynamic_data->func_disps + func->func_disp_idx;

            if(V_VT(&entry->val) != VT_DISPATCH) {
                FIXME("calling %s not supported\n", debugstr_variant(&entry->val));
                return E_NOTIMPL;
            }

            if((IDispatch*)&entry->func_obj->dispex.IWineJSDispatchHost_iface != V_DISPATCH(&entry->val)) {
                if(!V_DISPATCH(&entry->val)) {
                    FIXME("Calling null\n");
                    return E_FAIL;
                }

                hres = invoke_disp_value(This, V_DISPATCH(&entry->val), 0, flags, dp, res, ei, NULL);
                break;
            }
        }

        hres = call_builtin_function(This, func, dp, res, ei, caller);
        break;
    case DISPATCH_PROPERTYGET: {
        func_obj_entry_t *entry;

        if(func->id == DISPID_VALUE) {
            BSTR ret;

            hres = dispex_to_string(This, &ret);
            if(FAILED(hres))
                return hres;

            V_VT(res) = VT_BSTR;
            V_BSTR(res) = ret;
            return S_OK;
        }

        hres = get_func_obj_entry(This, func, &entry);
        if(FAILED(hres))
            return hres;

        V_VT(res) = VT_EMPTY;
        return VariantCopy(res, &entry->val);
    }
    case DISPATCH_PROPERTYPUT: {
        func_obj_entry_t *entry;

        if(dp->cArgs != 1 || (dp->cNamedArgs == 1 && *dp->rgdispidNamedArgs != DISPID_PROPERTYPUT)
           || dp->cNamedArgs > 1) {
            FIXME("invalid args\n");
            return E_INVALIDARG;
        }

        /*
         * NOTE: Although we have IDispatchEx tests showing, that it's not allowed to set
         * function property using InvokeEx, it's possible to do that from jscript.
         * Native probably uses some undocumented interface in this case, but it should
         * be fine for us to allow IDispatchEx handle that.
         */
        hres = get_func_obj_entry(This, func, &entry);
        if(FAILED(hres))
            return hres;

        return VariantCopy(&entry->val, dp->rgvarg);
    }
    default:
        FIXME("Unimplemented flags %x\n", flags);
        hres = E_NOTIMPL;
    }

    return hres;
}

static HRESULT invoke_builtin_prop(DispatchEx *This, DISPID id, LCID lcid, WORD flags, DISPPARAMS *dp,
        VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    func_info_t *func;
    HRESULT hres;

    hres = get_builtin_func(This->info, id, &func);
    if(id == DISPID_VALUE && hres == DISP_E_MEMBERNOTFOUND)
        return dispex_value(This, lcid, flags, dp, res, ei, caller);
    if(FAILED(hres))
        return hres;

    if(func->func_disp_idx >= 0)
        return invoke_builtin_function(This, func, flags, dp, res, ei, caller);

    if(func->hook) {
        hres = func->hook(This, flags, dp, res, ei, caller);
        if(hres != S_FALSE)
            return hres;
    }

    switch(flags) {
    case DISPATCH_PROPERTYPUT:
        if(res)
            V_VT(res) = VT_EMPTY;
        hres = builtin_propput(This, func, dp, caller);
        break;
    case DISPATCH_PROPERTYGET:
        hres = builtin_propget(This, func, dp, res);
        break;
    default:
        if(!func->get_vtbl_off) {
            hres = typeinfo_invoke(This, func, flags, dp, res, ei);
        }else {
            VARIANT v;

            hres = builtin_propget(This, func, NULL, &v);
            if(FAILED(hres))
                return hres;

            if(flags != (DISPATCH_PROPERTYGET|DISPATCH_METHOD) || dp->cArgs) {
                if(V_VT(&v) != VT_DISPATCH) {
                    FIXME("Not a function %s flags %08x\n", debugstr_variant(&v), flags);
                    VariantClear(&v);
                    return E_FAIL;
                }

                hres = invoke_disp_value(This, V_DISPATCH(&v), lcid, flags, dp, res, ei, caller);
                IDispatch_Release(V_DISPATCH(&v));
            }else if(res) {
                *res = v;
            }else {
                VariantClear(&v);
            }
        }
    }

    return hres;
}

HRESULT dispex_call_builtin(DispatchEx *dispex, DISPID id, DISPPARAMS *dp,
                            VARIANT *res, EXCEPINFO *ei, IServiceProvider *caller)
{
    func_info_t *func;
    HRESULT hres;

    hres = get_builtin_func(dispex->info, id, &func);
    if(FAILED(hres))
        return hres;

    return call_builtin_function(dispex, func, dp, res, ei, caller);
}

HRESULT remove_attribute(DispatchEx *This, DISPID id, VARIANT_BOOL *success)
{
    switch(get_dispid_type(id)) {
    case DISPEXPROP_CUSTOM:
        FIXME("DISPEXPROP_CUSTOM not supported\n");
        return E_NOTIMPL;

    case DISPEXPROP_DYNAMIC: {
        DWORD idx = id - DISPID_DYNPROP_0;
        dynamic_prop_t *prop;

        prop = This->dynamic_data->props+idx;
        VariantClear(&prop->var);
        prop->flags |= DYNPROP_DELETED;
        *success = VARIANT_TRUE;
        return S_OK;
    }
    case DISPEXPROP_BUILTIN: {
        VARIANT var;
        DISPPARAMS dp = {&var,NULL,1,0};
        func_info_t *func;
        HRESULT hres;

        hres = get_builtin_func(This->info, id, &func);
        if(FAILED(hres))
            return hres;

        /* For builtin functions, we set their value to the original function. */
        if(func->func_disp_idx >= 0) {
            func_obj_entry_t *entry;

            if(!This->dynamic_data || !This->dynamic_data->func_disps
                || !This->dynamic_data->func_disps[func->func_disp_idx].func_obj) {
                *success = VARIANT_FALSE;
                return S_OK;
            }

            entry = This->dynamic_data->func_disps + func->func_disp_idx;
            if(V_VT(&entry->val) == VT_DISPATCH
                    && V_DISPATCH(&entry->val) == (IDispatch*)&entry->func_obj->dispex.IWineJSDispatchHost_iface) {
                *success = VARIANT_FALSE;
                return S_OK;
            }

            VariantClear(&entry->val);
            V_VT(&entry->val) = VT_DISPATCH;
            V_DISPATCH(&entry->val) = (IDispatch*)&entry->func_obj->dispex.IWineJSDispatchHost_iface;
            IDispatch_AddRef(V_DISPATCH(&entry->val));
            *success = VARIANT_TRUE;
            return S_OK;
        }
        *success = VARIANT_TRUE;

        V_VT(&var) = VT_EMPTY;
        hres = builtin_propput(This, func, &dp, NULL);
        if(FAILED(hres)) {
            VARIANT *ref;
            hres = dispex_get_dprop_ref(This, func->name, FALSE, &ref);
            if(FAILED(hres) || V_VT(ref) != VT_BSTR)
                *success = VARIANT_FALSE;
            else
                VariantClear(ref);
        }
        return S_OK;
    }
    default:
        assert(0);
        return E_FAIL;
    }
}

compat_mode_t dispex_compat_mode(DispatchEx *dispex)
{
    return dispex->info != dispex->info->desc->delayed_init_info
        ? dispex->info->compat_mode
        : dispex->info->desc->vtbl->get_compat_mode(dispex);
}

HRESULT dispex_to_string(DispatchEx *dispex, BSTR *ret)
{
    static const WCHAR prefix[8] = L"[object ";
    static const WCHAR suffix[] = L"]";
    WCHAR buf[ARRAY_SIZE(prefix) + 28 + ARRAY_SIZE(suffix)], *p = buf;
    compat_mode_t compat_mode = dispex_compat_mode(dispex);
    const char *name = dispex->info->desc->name;

    if(!ret)
        return E_INVALIDARG;

    memcpy(p, prefix, sizeof(prefix));
    p += ARRAY_SIZE(prefix);
    if(compat_mode < COMPAT_MODE_IE9)
        p--;
    else {
        while(*name)
            *p++ = *name++;
        assert(p + ARRAY_SIZE(suffix) - buf <= ARRAY_SIZE(buf));
    }
    memcpy(p, suffix, sizeof(suffix));

    *ret = SysAllocString(buf);
    return *ret ? S_OK : E_OUTOFMEMORY;
}

static dispex_data_t *ensure_dispex_info(DispatchEx *dispex, dispex_static_data_t *desc,
                                         compat_mode_t compat_mode, HTMLInnerWindow *script_global)
{
    HRESULT hres;

    if(!desc->info_cache[compat_mode]) {
        EnterCriticalSection(&cs_dispex_static_data);
        if(!desc->info_cache[compat_mode])
            desc->info_cache[compat_mode] = preprocess_dispex_data(desc, compat_mode);
        LeaveCriticalSection(&cs_dispex_static_data);
        if(!desc->info_cache[compat_mode])
            return NULL;
    }

    if(compat_mode >= COMPAT_MODE_IE9 && script_global) {
        if(!script_global->jscript)
            initialize_script_global(script_global);
        if(script_global->jscript) {
            hres = IWineJScript_InitHostObject(script_global->jscript, &dispex->IWineJSDispatchHost_iface,
                                               &dispex->jsdisp);
            if(FAILED(hres))
                ERR("Failed to initialize jsdisp: %08lx\n", hres);
        }
    }

    return desc->info_cache[compat_mode];
}

static BOOL ensure_real_info(DispatchEx *dispex)
{
    if(dispex->info != dispex->info->desc->delayed_init_info)
        return TRUE;

    dispex->info = ensure_dispex_info(dispex, dispex->info->desc, dispex_compat_mode(dispex), NULL);
    return dispex->info != NULL;
}

static inline DispatchEx *impl_from_IWineJSDispatchHost(IWineJSDispatchHost *iface)
{
    return CONTAINING_RECORD(iface, DispatchEx, IWineJSDispatchHost_iface);
}

static HRESULT WINAPI DispatchEx_QueryInterface(IWineJSDispatchHost *iface, REFIID riid, void **ppv)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);

    TRACE("%s (%p)->(%s %p)\n", This->info->desc->name, This, debugstr_mshtml_guid(riid), ppv);

    if(This->info->desc->vtbl->query_interface) {
        *ppv = This->info->desc->vtbl->query_interface(This, riid);
        if(*ppv)
            goto ret;
    }

    if(IsEqualGUID(&IID_IUnknown, riid) || IsEqualGUID(&IID_IDispatch, riid) || IsEqualGUID(&IID_IDispatchEx, riid))
        *ppv = &This->IWineJSDispatchHost_iface;
    else if(IsEqualGUID(&IID_IWineJSDispatchHost, riid))
        *ppv = &This->IWineJSDispatchHost_iface;
    else if(IsEqualGUID(&IID_nsXPCOMCycleCollectionParticipant, riid)) {
        *ppv = &dispex_ccp;
        return S_OK;
    }else if(IsEqualGUID(&IID_nsCycleCollectionISupports, riid)) {
        *ppv = &This->IWineJSDispatchHost_iface;
        return S_OK;
    }else if(IsEqualGUID(&IID_IDispatchJS, riid) ||
             IsEqualGUID(&IID_UndocumentedScriptIface, riid) ||
             IsEqualGUID(&IID_IMarshal, riid) ||
             IsEqualGUID(&IID_IManagedObject, riid)) {
        *ppv = NULL;
        return E_NOINTERFACE;
    }else {
        *ppv = NULL;
        WARN("%s (%p)->(%s %p)\n", This->info->desc->name, This, debugstr_mshtml_guid(riid), ppv);
        return E_NOINTERFACE;
    }

ret:
    IWineJSDispatchHost_AddRef(&This->IWineJSDispatchHost_iface);
    return S_OK;
}

static ULONG WINAPI DispatchEx_AddRef(IWineJSDispatchHost *iface)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    LONG ref = ccref_incr(&This->ccref, (nsISupports*)&This->IWineJSDispatchHost_iface);

    TRACE("%s (%p) ref=%ld\n", This->info->desc->name, This, ref);

    return ref;
}

static ULONG WINAPI DispatchEx_Release(IWineJSDispatchHost *iface)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    LONG ref = ccref_decr(&This->ccref, (nsISupports*)&This->IWineJSDispatchHost_iface, &dispex_ccp);

    TRACE("%s (%p) ref=%ld\n", This->info->desc->name, This, ref);

    /* Gecko ccref may not free the object immediately when ref count reaches 0, so we need
     * an extra care for objects that need an immediate clean up. See Gecko's
     * NS_IMPL_CYCLE_COLLECTING_NATIVE_RELEASE_WITH_LAST_RELEASE for details. */
    if(!ref && This->info->desc->vtbl->last_release) {
        ccref_incr(&This->ccref, (nsISupports*)&This->IWineJSDispatchHost_iface);
        This->info->desc->vtbl->last_release(This);
        ccref_decr(&This->ccref, (nsISupports*)&This->IWineJSDispatchHost_iface, &dispex_ccp);
    }

    return ref;
}

static HRESULT WINAPI DispatchEx_GetTypeInfoCount(IWineJSDispatchHost *iface, UINT *pctinfo)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);

    TRACE("%s (%p)->(%p)\n", This->info->desc->name, This, pctinfo);

    *pctinfo = 1;
    return S_OK;
}

static HRESULT WINAPI DispatchEx_GetTypeInfo(IWineJSDispatchHost *iface, UINT iTInfo,
                                             LCID lcid, ITypeInfo **ppTInfo)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    HRESULT hres;

    TRACE("%s (%p)->(%u %lu %p)\n", This->info->desc->name, This, iTInfo, lcid, ppTInfo);

    hres = get_typeinfo(This->info->desc->disp_tid, ppTInfo);
    if(FAILED(hres))
        return hres;

    ITypeInfo_AddRef(*ppTInfo);
    return S_OK;
}

static HRESULT WINAPI DispatchEx_GetIDsOfNames(IWineJSDispatchHost *iface, REFIID riid,
                                               LPOLESTR *rgszNames, UINT cNames,
                                               LCID lcid, DISPID *rgDispId)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    HRESULT hres = S_OK;

    TRACE("%s (%p)->(%s %p %u %lu %p)\n", This->info->desc->name, This, debugstr_guid(riid), rgszNames,
          cNames, lcid, rgDispId);

    /* Native ignores all cNames > 1, and doesn't even fill them */
    if(cNames)
        hres = IWineJSDispatchHost_GetDispID(&This->IWineJSDispatchHost_iface, rgszNames[0], 0, rgDispId);

    return hres;
}

static HRESULT WINAPI DispatchEx_Invoke(IWineJSDispatchHost *iface, DISPID dispIdMember,
                            REFIID riid, LCID lcid, WORD wFlags, DISPPARAMS *pDispParams,
                            VARIANT *pVarResult, EXCEPINFO *pExcepInfo, UINT *puArgErr)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);

    TRACE("%s (%p)->(%ld %s %ld %d %p %p %p %p)\n", This->info->desc->name, This, dispIdMember,
          debugstr_guid(riid), lcid, wFlags, pDispParams, pVarResult, pExcepInfo, puArgErr);

    return IWineJSDispatchHost_InvokeEx(&This->IWineJSDispatchHost_iface, dispIdMember, lcid, wFlags, pDispParams,
            pVarResult, pExcepInfo, NULL);
}

static HRESULT WINAPI DispatchEx_GetDispID(IWineJSDispatchHost *iface, BSTR bstrName, DWORD grfdex, DISPID *pid)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    dynamic_prop_t *dprop = NULL;
    HRESULT hres;

    TRACE("%s (%p)->(%s %lx %p)\n", This->info->desc->name, This, debugstr_w(bstrName), grfdex, pid);

    if(grfdex & ~(fdexNameCaseSensitive|fdexNameCaseInsensitive|fdexNameEnsure|fdexNameImplicit|FDEX_VERSION_MASK))
        FIXME("Unsupported grfdex %lx\n", grfdex);

    if(!ensure_real_info(This))
        return E_OUTOFMEMORY;
    if(This->jsdisp)
        return IWineJSDispatch_GetDispID(This->jsdisp, bstrName, grfdex, pid);

    hres = get_builtin_id(This, bstrName, grfdex, pid);
    if(hres != DISP_E_UNKNOWNNAME)
        return hres;

    hres = get_dynamic_prop(This, bstrName, grfdex & ~fdexNameEnsure, &dprop);
    if(FAILED(hres)) {
        if(This->info->desc->vtbl->find_dispid) {
            hres = This->info->desc->vtbl->find_dispid(This, bstrName, grfdex, pid);
            if(SUCCEEDED(hres))
                return hres;
        }
        if(grfdex & fdexNameEnsure)
            hres = alloc_dynamic_prop(This, bstrName, dprop, &dprop);
        if(FAILED(hres))
            return hres;
    }

    *pid = DISPID_DYNPROP_0 + (dprop - This->dynamic_data->props);
    return S_OK;
}

HRESULT dispex_prop_get(DispatchEx *dispex, DISPID id, LCID lcid, VARIANT *r, EXCEPINFO *ei, IServiceProvider *caller)
{
    switch(get_dispid_type(id)) {
    case DISPEXPROP_CUSTOM: {
        DISPPARAMS dp = { .cArgs = 0 };
        if(!dispex->info->desc->vtbl->invoke)
            return DISP_E_MEMBERNOTFOUND;
        return dispex->info->desc->vtbl->invoke(dispex, id, lcid, DISPATCH_PROPERTYGET, &dp, r, ei, caller);
    }

    case DISPEXPROP_DYNAMIC: {
        DWORD idx = id - DISPID_DYNPROP_0;
        dynamic_prop_t *prop;

        if(!get_dynamic_data(dispex) || dispex->dynamic_data->prop_cnt <= idx)
            return DISP_E_MEMBERNOTFOUND;

        prop = dispex->dynamic_data->props+idx;
        if(prop->flags & DYNPROP_DELETED)
            return DISP_E_MEMBERNOTFOUND;

        V_VT(r) = VT_EMPTY;
        return variant_copy(r, &prop->var);
    }

    case DISPEXPROP_BUILTIN: {
        DISPPARAMS dp = { .cArgs = 0 };
        return invoke_builtin_prop(dispex, id, lcid, DISPATCH_PROPERTYGET, &dp, r, ei, caller);
    }

    default:
        assert(0);
        return E_FAIL;
    }
}

HRESULT dispex_prop_put(DispatchEx *dispex, DISPID id, LCID lcid, VARIANT *v, EXCEPINFO *ei, IServiceProvider *caller)
{
    static DISPID propput_dispid = DISPID_PROPERTYPUT;

    switch(get_dispid_type(id)) {
    case DISPEXPROP_CUSTOM: {
        DISPPARAMS dp = { .cArgs = 1, .rgvarg = v, .cNamedArgs = 1, .rgdispidNamedArgs = &propput_dispid };
        if(!dispex->info->desc->vtbl->invoke)
            return DISP_E_MEMBERNOTFOUND;
        return dispex->info->desc->vtbl->invoke(dispex, id, lcid, DISPATCH_PROPERTYPUT, &dp, NULL, ei, caller);
    }

    case DISPEXPROP_DYNAMIC: {
        DWORD idx = id - DISPID_DYNPROP_0;
        dynamic_prop_t *prop;
        HRESULT hres;

        if(!get_dynamic_data(dispex) || dispex->dynamic_data->prop_cnt <= idx)
            return DISP_E_MEMBERNOTFOUND;

        prop = dispex->dynamic_data->props + idx;

        TRACE("put %s\n", debugstr_variant(v));
        VariantClear(&prop->var);
        hres = variant_copy(&prop->var, v);
        if(FAILED(hres))
            return hres;

        prop->flags &= ~DYNPROP_DELETED;
        return S_OK;
    }

    case DISPEXPROP_BUILTIN: {
        DISPPARAMS dp = { .cArgs = 1, .rgvarg = v, .cNamedArgs = 1, .rgdispidNamedArgs = &propput_dispid };
        return invoke_builtin_prop(dispex, id, lcid, DISPATCH_PROPERTYPUT, &dp, NULL, ei, caller);
    }

    default:
        assert(0);
        return E_FAIL;
    }
}

static HRESULT dispex_prop_call(DispatchEx *dispex, DISPID id, LCID lcid, WORD flags, DISPPARAMS *dp, VARIANT *r,
                                EXCEPINFO *ei, IServiceProvider *caller)
{
    switch(get_dispid_type(id)) {
    case DISPEXPROP_CUSTOM:
        if(!dispex->info->desc->vtbl->invoke)
            return DISP_E_MEMBERNOTFOUND;
        return dispex->info->desc->vtbl->invoke(dispex, id, lcid, flags, dp, r, ei, caller);

    case DISPEXPROP_DYNAMIC: {
        DWORD idx = id - DISPID_DYNPROP_0;
        dynamic_prop_t *prop;

        if(!get_dynamic_data(dispex) || dispex->dynamic_data->prop_cnt <= idx)
            return DISP_E_MEMBERNOTFOUND;

        prop = dispex->dynamic_data->props + idx;

        switch(flags) {
        case DISPATCH_METHOD|DISPATCH_PROPERTYGET:
            if(!r)
                return E_INVALIDARG;
            /* fall through */
        case DISPATCH_METHOD:
            if(V_VT(&prop->var) != VT_DISPATCH) {
                FIXME("invoke %s\n", debugstr_variant(&prop->var));
                return E_NOTIMPL;
            }

            return invoke_disp_value(dispex, V_DISPATCH(&prop->var), lcid, flags, dp, r, ei, caller);
        default:
            FIXME("unhandled flags %x on dynamic property\n", flags);
            return E_NOTIMPL;
        }
    }
    case DISPEXPROP_BUILTIN:
        if(flags == DISPATCH_CONSTRUCT) {
            if(id == DISPID_VALUE) {
                if(dispex->info->desc->vtbl->value) {
                    return dispex->info->desc->vtbl->value(dispex, lcid, flags, dp, r, ei, caller);
                }
                FIXME("DISPATCH_CONSTRUCT flag but missing value function\n");
                return E_FAIL;
            }
            FIXME("DISPATCH_CONSTRUCT flag without DISPID_VALUE\n");
            return E_FAIL;
        }

        return invoke_builtin_prop(dispex, id, lcid, flags, dp, r, ei, caller);

    default:
        assert(0);
        return E_FAIL;
    }
}

static HRESULT WINAPI DispatchEx_InvokeEx(IWineJSDispatchHost *iface, DISPID id, LCID lcid, WORD wFlags, DISPPARAMS *pdp,
        VARIANT *pvarRes, EXCEPINFO *pei, IServiceProvider *pspCaller)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);

    TRACE("%s (%p)->(%lx %lx %x %p %p %p %p)\n", This->info->desc->name, This, id, lcid, wFlags, pdp, pvarRes, pei, pspCaller);

    if(!ensure_real_info(This))
        return E_OUTOFMEMORY;

    if(wFlags == (DISPATCH_PROPERTYPUT|DISPATCH_PROPERTYPUTREF))
        wFlags = DISPATCH_PROPERTYPUT;

    if(This->info->desc->vtbl->disp_invoke) {
        HRESULT hres = This->info->desc->vtbl->disp_invoke(This, id, lcid, wFlags, pdp, pvarRes, pei, pspCaller);
        if(hres != S_FALSE)
            return hres;
    }

    if(This->jsdisp)
        return IWineJSDispatch_InvokeEx(This->jsdisp, id, lcid, wFlags, pdp, pvarRes, pei, pspCaller);

    switch(wFlags) {
    case DISPATCH_PROPERTYGET:
        if(!pvarRes)
            return E_INVALIDARG;
        return dispex_prop_get(This, id, lcid, pvarRes, pei, pspCaller);

    case DISPATCH_PROPERTYPUT: {
        if(pdp->cArgs != 1 || (pdp->cNamedArgs == 1 && *pdp->rgdispidNamedArgs != DISPID_PROPERTYPUT)
           || pdp->cNamedArgs > 1) {
            FIXME("invalid args\n");
            return E_INVALIDARG;
        }
        return dispex_prop_put(This, id, lcid, pdp->rgvarg, pei, pspCaller);
    }

    default:
        return dispex_prop_call(This, id, lcid, wFlags, pdp, pvarRes, pei, pspCaller);
    }
}

static HRESULT WINAPI DispatchEx_DeleteMemberByName(IWineJSDispatchHost *iface, BSTR name, DWORD grfdex)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    DISPID id;
    HRESULT hres;

    TRACE("%s (%p)->(%s %lx)\n", This->info->desc->name, This, debugstr_w(name), grfdex);

    if(!ensure_real_info(This))
        return E_OUTOFMEMORY;
    if(This->jsdisp)
        return IWineJSDispatch_DeleteMemberByName(This->jsdisp, name, grfdex);
    if(dispex_compat_mode(This) < COMPAT_MODE_IE8 && !This->info->desc->vtbl->delete)
        return E_NOTIMPL;

    hres = IWineJSDispatchHost_GetDispID(&This->IWineJSDispatchHost_iface, name, grfdex & ~fdexNameEnsure, &id);
    if(FAILED(hres)) {
        compat_mode_t compat_mode = dispex_compat_mode(This);
        TRACE("property %s not found\n", debugstr_w(name));
        return compat_mode < COMPAT_MODE_IE8 ? E_NOTIMPL :
               compat_mode < COMPAT_MODE_IE9 ? hres : S_OK;
    }

    return IWineJSDispatchHost_DeleteMemberByDispID(&This->IWineJSDispatchHost_iface, id);
}

static HRESULT WINAPI DispatchEx_DeleteMemberByDispID(IWineJSDispatchHost *iface, DISPID id)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);

    TRACE("%s (%p)->(%lx)\n", This->info->desc->name, This, id);

    if(!ensure_real_info(This))
        return E_OUTOFMEMORY;
    if(This->jsdisp)
        return IWineJSDispatch_DeleteMemberByDispID(This->jsdisp, id);
    if(is_custom_dispid(id) && This->info->desc->vtbl->delete)
        return This->info->desc->vtbl->delete(This, id);

    if(dispex_compat_mode(This) < COMPAT_MODE_IE8) {
        /* Not implemented by IE */
        return E_NOTIMPL;
    }

    if(is_dynamic_dispid(id)) {
        DWORD idx = id - DISPID_DYNPROP_0;
        dynamic_prop_t *prop;

        if(!get_dynamic_data(This) || idx >= This->dynamic_data->prop_cnt)
            return S_OK;

        prop = This->dynamic_data->props + idx;
        VariantClear(&prop->var);
        prop->flags |= DYNPROP_DELETED;
        return S_OK;
    }

    return S_OK;
}

static HRESULT WINAPI DispatchEx_GetMemberProperties(IWineJSDispatchHost *iface, DISPID id, DWORD grfdexFetch, DWORD *pgrfdex)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    FIXME("%s (%p)->(%lx %lx %p)\n", This->info->desc->name, This, id, grfdexFetch, pgrfdex);
    return E_NOTIMPL;
}

static HRESULT WINAPI DispatchEx_GetMemberName(IWineJSDispatchHost *iface, DISPID id, BSTR *pbstrName)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    func_info_t *func;
    HRESULT hres;

    TRACE("%s (%p)->(%lx %p)\n", This->info->desc->name, This, id, pbstrName);

    if(!ensure_real_info(This))
        return E_OUTOFMEMORY;
    if(This->jsdisp)
        return IWineJSDispatch_GetMemberName(This->jsdisp, id, pbstrName);

    if(is_custom_dispid(id)) {
        if(This->info->desc->vtbl->get_name)
            return This->info->desc->vtbl->get_name(This, id, pbstrName);
        return DISP_E_MEMBERNOTFOUND;
    }

    if(is_dynamic_dispid(id)) {
        DWORD idx = id - DISPID_DYNPROP_0;

        if(!get_dynamic_data(This) || This->dynamic_data->prop_cnt <= idx)
            return DISP_E_MEMBERNOTFOUND;

        *pbstrName = SysAllocString(This->dynamic_data->props[idx].name);
        if(!*pbstrName)
            return E_OUTOFMEMORY;

        return S_OK;
    }

    hres = get_builtin_func(This->info, id, &func);
    if(FAILED(hres))
        return hres;

    *pbstrName = SysAllocString(func->name);
    if(!*pbstrName)
        return E_OUTOFMEMORY;
    return S_OK;
}

static HRESULT next_dynamic_id(DispatchEx *dispex, DWORD idx, DISPID *ret_id)
{
    while(idx < dispex->dynamic_data->prop_cnt &&
          (dispex->dynamic_data->props[idx].flags & (DYNPROP_DELETED | DYNPROP_HIDDEN)))
        idx++;

    if(idx == dispex->dynamic_data->prop_cnt) {
        *ret_id = DISPID_STARTENUM;
        return S_FALSE;
    }

    *ret_id = DISPID_DYNPROP_0+idx;
    return S_OK;
}

HRESULT dispex_next_id(DispatchEx *dispex, DISPID id, DISPID *ret)
{
    func_info_t *func;
    HRESULT hres;

    if(is_dynamic_dispid(id)) {
        DWORD idx = id - DISPID_DYNPROP_0;

        if(!get_dynamic_data(dispex) || dispex->dynamic_data->prop_cnt <= idx)
            return DISP_E_MEMBERNOTFOUND;

        return next_dynamic_id(dispex, idx+1, ret);
    }

    if(!is_custom_dispid(id)) {
        if(id == DISPID_STARTENUM) {
            func = dispex->info->funcs;
        }else {
            hres = get_builtin_func(dispex->info, id, &func);
            if(FAILED(hres))
                return hres;
            func++;
        }

        while(func < dispex->info->funcs + dispex->info->func_cnt) {
            if(func->func_disp_idx == -1) {
                *ret = func->id;
                return S_OK;
            }
            func++;
        }

        id = DISPID_STARTENUM;
    }

    if(dispex->info->desc->vtbl->next_dispid) {
        hres = dispex->info->desc->vtbl->next_dispid(dispex, id, ret);
        if(hres != S_FALSE)
            return hres;
    }

    if(get_dynamic_data(dispex) && dispex->dynamic_data->prop_cnt)
        return next_dynamic_id(dispex, 0, ret);

    *ret = DISPID_STARTENUM;
    return S_FALSE;
}

static HRESULT WINAPI DispatchEx_GetNextDispID(IWineJSDispatchHost *iface, DWORD grfdex, DISPID id, DISPID *pid)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);

    TRACE("%s (%p)->(%lx %lx %p)\n", This->info->desc->name, This, grfdex, id, pid);

    if(!ensure_real_info(This))
        return E_OUTOFMEMORY;
    if(This->jsdisp)
        return IWineJSDispatch_GetNextDispID(This->jsdisp, grfdex, id, pid);
    return dispex_next_id(This, id, pid);
}

static HRESULT WINAPI DispatchEx_GetNameSpaceParent(IWineJSDispatchHost *iface, IUnknown **ppunk)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    FIXME("%s (%p)->(%p)\n", This->info->desc->name, This, ppunk);
    return E_NOTIMPL;
}

static HRESULT WINAPI JSDispatchHost_GetJSDispatch(IWineJSDispatchHost *iface, IWineJSDispatch **ret)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);

    if(!This->jsdisp)
        return E_NOINTERFACE;
    *ret = This->jsdisp;
    IWineJSDispatch_AddRef(*ret);
    return S_OK;
}

static HRESULT WINAPI JSDispatchHost_LookupProperty(IWineJSDispatchHost *iface, const WCHAR *name, DWORD flags,
                                                    struct property_info *desc)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    func_info_t *func;
    DISPID id;
    HRESULT hres;

    TRACE("%s (%p)->(%s)\n", This->info->desc->name, This, debugstr_w(name));

    hres = get_builtin_id(This, name, flags, &id);
    if(FAILED(hres))
        return hres;

    hres = get_builtin_func(This->info, id, &func);
    if(FAILED(hres))
        return hres;
    desc->id = id;
    desc->flags = PROPF_WRITABLE | PROPF_CONFIGURABLE;
    if(func->func_disp_idx < 0) {
        desc->flags |= PROPF_ENUMERABLE;
        desc->func_iid = 0;
    }else {
        desc->func_iid = func->tid;
    }
    desc->name = func->name;
    return S_OK;
}

static HRESULT WINAPI JSDispatchHost_NextProperty(IWineJSDispatchHost *iface, DISPID id, struct property_info *desc)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    func_info_t *func;
    HRESULT hres;

    TRACE("%s (%p)->(%lx)\n", This->info->desc->name, This, id);

    if(id == DISPID_STARTENUM) {
        func = This->info->funcs;
    }else {
        hres = get_builtin_func(This->info, id, &func);
        if(FAILED(hres))
            return hres;
        func++;
    }

    while(func < This->info->funcs + This->info->func_cnt) {
        if(func->func_disp_idx == -1) {
            desc->id = func->id;
            desc->name = func->name;
            desc->flags = PROPF_WRITABLE | PROPF_CONFIGURABLE | PROPF_ENUMERABLE;
            desc->func_iid = 0;
            return S_OK;
        }
        func++;
    }
    return S_FALSE;
}

static HRESULT WINAPI JSDispatchHost_GetProperty(IWineJSDispatchHost *iface, DISPID id, LCID lcid, VARIANT *r,
                                                 EXCEPINFO *ei, IServiceProvider *caller)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);

    TRACE("%s (%p)->(%lx)\n", This->info->desc->name, This, id);

    return dispex_prop_get(This, id, lcid, r, ei, caller);
}

static HRESULT WINAPI JSDispatchHost_SetProperty(IWineJSDispatchHost *iface, DISPID id, LCID lcid, VARIANT *v,
                                                 EXCEPINFO *ei, IServiceProvider *caller)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);

    TRACE("%s (%p)->(%lx)\n", This->info->desc->name, This, id);

    return dispex_prop_put(This, id, lcid, v, ei, caller);
}

static HRESULT WINAPI JSDispatchHost_CallFunction(IWineJSDispatchHost *iface, DISPID id, UINT32 iid, DISPPARAMS *dp, VARIANT *ret,
                                              EXCEPINFO *ei, IServiceProvider *caller)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);
    func_info_t *func;
    HRESULT hres;

    TRACE("%s (%p)->(%lx %x %p %p %p %p)\n", This->info->desc->name, This, id, iid, dp, ret, ei, caller);

    hres = get_builtin_func(This->info, id, &func);
    if(FAILED(hres))
        return hres;
    if(func->tid != iid || func->func_disp_idx < 0)
        return E_UNEXPECTED;
    return call_builtin_function(This, func, dp, ret, ei, caller);
}

static HRESULT WINAPI JSDispatchHost_ToString(IWineJSDispatchHost *iface, BSTR *str)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(iface);

    TRACE("%s (%p)\n", This->info->desc->name, This);

    return dispex_to_string(This, str);
}

static IWineJSDispatchHostVtbl JSDispatchHostVtbl = {
    DispatchEx_QueryInterface,
    DispatchEx_AddRef,
    DispatchEx_Release,
    DispatchEx_GetTypeInfoCount,
    DispatchEx_GetTypeInfo,
    DispatchEx_GetIDsOfNames,
    DispatchEx_Invoke,
    DispatchEx_GetDispID,
    DispatchEx_InvokeEx,
    DispatchEx_DeleteMemberByName,
    DispatchEx_DeleteMemberByDispID,
    DispatchEx_GetMemberProperties,
    DispatchEx_GetMemberName,
    DispatchEx_GetNextDispID,
    DispatchEx_GetNameSpaceParent,
    JSDispatchHost_GetJSDispatch,
    JSDispatchHost_LookupProperty,
    JSDispatchHost_NextProperty,
    JSDispatchHost_GetProperty,
    JSDispatchHost_SetProperty,
    JSDispatchHost_CallFunction,
    JSDispatchHost_ToString,
};

static nsresult NSAPI dispex_traverse(void *ccp, void *p, nsCycleCollectionTraversalCallback *cb)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(p);
    dynamic_prop_t *prop;

    describe_cc_node(&This->ccref, This->info->desc->name, cb);

    if(This->info->desc->vtbl->traverse)
        This->info->desc->vtbl->traverse(This, cb);

    if(!This->dynamic_data)
        return NS_OK;

    for(prop = This->dynamic_data->props; prop < This->dynamic_data->props + This->dynamic_data->prop_cnt; prop++)
        traverse_variant(&prop->var, "dispex_data", cb);

    if(This->dynamic_data->func_disps) {
        func_obj_entry_t *iter = This->dynamic_data->func_disps, *end = iter + This->info->func_disp_cnt;

        for(iter = This->dynamic_data->func_disps; iter < end; iter++) {
            if(!iter->func_obj)
                continue;
            note_cc_edge((nsISupports*)&iter->func_obj->dispex.IWineJSDispatchHost_iface, "func_obj", cb);
            traverse_variant(&iter->val, "func_val", cb);
        }
    }

    return NS_OK;
}

void dispex_props_unlink(DispatchEx *This)
{
    dynamic_prop_t *prop;

    if(!This->dynamic_data)
        return;

    for(prop = This->dynamic_data->props; prop < This->dynamic_data->props + This->dynamic_data->prop_cnt; prop++) {
        prop->flags |= DYNPROP_DELETED;
        unlink_variant(&prop->var);
    }

    if(This->dynamic_data->func_disps) {
        func_obj_entry_t *iter = This->dynamic_data->func_disps, *end = iter + This->info->func_disp_cnt;

        for(iter = This->dynamic_data->func_disps; iter < end; iter++) {
            if(!iter->func_obj)
                continue;
            iter->func_obj->obj = NULL;
            IWineJSDispatchHost_Release(&iter->func_obj->dispex.IWineJSDispatchHost_iface);
            VariantClear(&iter->val);
        }

        free(This->dynamic_data->func_disps);
        This->dynamic_data->func_disps = NULL;
    }
}

static nsresult NSAPI dispex_unlink(void *p)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(p);

    if(This->info->desc->vtbl->unlink)
        This->info->desc->vtbl->unlink(This);

    dispex_props_unlink(This);
    return NS_OK;
}

static void NSAPI dispex_delete_cycle_collectable(void *p)
{
    DispatchEx *This = impl_from_IWineJSDispatchHost(p);
    IWineJSDispatch *jsdisp = This->jsdisp;
    dynamic_prop_t *prop;

    if(This->info->desc->vtbl->unlink)
        This->info->desc->vtbl->unlink(This);

    if(!This->dynamic_data)
        goto destructor;

    for(prop = This->dynamic_data->props; prop < This->dynamic_data->props + This->dynamic_data->prop_cnt; prop++) {
        VariantClear(&prop->var);
        free(prop->name);
    }

    free(This->dynamic_data->props);

    if(This->dynamic_data->func_disps) {
        func_obj_entry_t *iter;

        for(iter = This->dynamic_data->func_disps; iter < This->dynamic_data->func_disps + This->info->func_disp_cnt; iter++) {
            if(iter->func_obj) {
                iter->func_obj->obj = NULL;
                IWineJSDispatchHost_Release(&iter->func_obj->dispex.IWineJSDispatchHost_iface);
            }
            VariantClear(&iter->val);
        }

        free(This->dynamic_data->func_disps);
    }

    free(This->dynamic_data);

destructor:
    This->info->desc->vtbl->destructor(This);
    if(jsdisp)
        IWineJSDispatch_Free(jsdisp);
}

void init_dispex_cc(void)
{
    static const CCObjCallback dispex_ccp_callback = {
        dispex_traverse,
        dispex_unlink,
        dispex_delete_cycle_collectable
    };
    ccp_init(&dispex_ccp, &dispex_ccp_callback);
}

const void *dispex_get_vtbl(DispatchEx *dispex)
{
    return dispex->info->desc->vtbl;
}

void init_dispatch(DispatchEx *dispex, dispex_static_data_t *data, HTMLInnerWindow *script_global, compat_mode_t compat_mode)
{
    assert(compat_mode < COMPAT_MODE_CNT);

    dispex->IWineJSDispatchHost_iface.lpVtbl = &JSDispatchHostVtbl;
    dispex->dynamic_data = NULL;
    dispex->jsdisp = NULL;
    ccref_init(&dispex->ccref, 1);

    if(data->vtbl->get_compat_mode) {
        /* delayed init */
        if(!data->delayed_init_info) {
            EnterCriticalSection(&cs_dispex_static_data);
            if(!data->delayed_init_info) {
                dispex_data_t *info = calloc(1, sizeof(*data->delayed_init_info));
                if(info) {
                    info->desc = data;
                    data->delayed_init_info = info;
                }
            }
            LeaveCriticalSection(&cs_dispex_static_data);
        }
        dispex->info = data->delayed_init_info;
    }else {
        dispex->info = ensure_dispex_info(dispex, data, compat_mode, script_global);
    }
}

void init_dispatch_with_owner(DispatchEx *dispex, dispex_static_data_t *desc, DispatchEx *owner)
{
    HTMLInnerWindow *script_global = get_script_global(owner);
    init_dispatch(dispex, desc, script_global, dispex_compat_mode(owner));
    if(script_global)
        IHTMLWindow2_Release(&script_global->base.IHTMLWindow2_iface);
}
