/*
 *
 * Copyright 2012 Alistair Leslie-Hughes
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

#define COBJMACROS
#include <stdio.h>

#include "windows.h"
#include "ole2.h"
#include "olectl.h"
#include "oleauto.h"
#include "dispex.h"

#include "wine/test.h"

#include "initguid.h"
#include "scrrun.h"

static IFileSystem3 *fs3;

static void test_interfaces(void)
{
    static const WCHAR nonexistent_dirW[] = {
        'c', ':', '\\', 'N', 'o', 'n', 'e', 'x', 'i', 's', 't', 'e', 'n', 't', 0};
    static const WCHAR pathW[] = {'p','a','t','h',0};
    static const WCHAR file_kernel32W[] = {
        '\\', 'k', 'e', 'r', 'n', 'e', 'l', '3', '2', '.', 'd', 'l', 'l', 0};
    HRESULT hr;
    IDispatch *disp;
    IDispatchEx *dispex;
    IObjectWithSite *site;
    VARIANT_BOOL b;
    BSTR path;
    WCHAR windows_path[MAX_PATH];
    WCHAR file_path[MAX_PATH];

    IFileSystem3_QueryInterface(fs3, &IID_IDispatch, (void**)&disp);

    GetSystemDirectoryW(windows_path, MAX_PATH);
    lstrcpyW(file_path, windows_path);
    lstrcatW(file_path, file_kernel32W);

    hr = IDispatch_QueryInterface(disp, &IID_IObjectWithSite, (void**)&site);
    ok(hr == E_NOINTERFACE, "got 0x%08x, expected 0x%08x\n", hr, E_NOINTERFACE);

    hr = IDispatch_QueryInterface(disp, &IID_IDispatchEx, (void**)&dispex);
    ok(hr == E_NOINTERFACE, "got 0x%08x, expected 0x%08x\n", hr, E_NOINTERFACE);

    b = VARIANT_TRUE;
    hr = IFileSystem3_FileExists(fs3, NULL, &b);
    ok(hr == S_OK, "got 0x%08x, expected 0x%08x\n", hr, S_OK);
    ok(b == VARIANT_FALSE, "got %x\n", b);

    hr = IFileSystem3_FileExists(fs3, NULL, NULL);
    ok(hr == E_POINTER, "got 0x%08x, expected 0x%08x\n", hr, E_POINTER);

    path = SysAllocString(pathW);
    b = VARIANT_TRUE;
    hr = IFileSystem3_FileExists(fs3, path, &b);
    ok(hr == S_OK, "got 0x%08x, expected 0x%08x\n", hr, S_OK);
    ok(b == VARIANT_FALSE, "got %x\n", b);
    SysFreeString(path);

    path = SysAllocString(file_path);
    b = VARIANT_FALSE;
    hr = IFileSystem3_FileExists(fs3, path, &b);
    ok(hr == S_OK, "got 0x%08x, expected 0x%08x\n", hr, S_OK);
    ok(b == VARIANT_TRUE, "got %x\n", b);
    SysFreeString(path);

    path = SysAllocString(windows_path);
    b = VARIANT_TRUE;
    hr = IFileSystem3_FileExists(fs3, path, &b);
    ok(hr == S_OK, "got 0x%08x, expected 0x%08x\n", hr, S_OK);
    ok(b == VARIANT_FALSE, "got %x\n", b);
    SysFreeString(path);

    /* Folder Exists */
    hr = IFileSystem3_FolderExists(fs3, NULL, NULL);
    ok(hr == E_POINTER, "got 0x%08x, expected 0x%08x\n", hr, E_POINTER);

    path = SysAllocString(windows_path);
    hr = IFileSystem3_FolderExists(fs3, path, &b);
    ok(hr == S_OK, "got 0x%08x, expected 0x%08x\n", hr, S_OK);
    ok(b == VARIANT_TRUE, "Folder doesn't exists\n");
    SysFreeString(path);

    path = SysAllocString(nonexistent_dirW);
    hr = IFileSystem3_FolderExists(fs3, path, &b);
    ok(hr == S_OK, "got 0x%08x, expected 0x%08x\n", hr, S_OK);
    ok(b == VARIANT_FALSE, "Folder exists\n");
    SysFreeString(path);

    path = SysAllocString(file_path);
    hr = IFileSystem3_FolderExists(fs3, path, &b);
    ok(hr == S_OK, "got 0x%08x, expected 0x%08x\n", hr, S_OK);
    ok(b == VARIANT_FALSE, "Folder exists\n");
    SysFreeString(path);

    IDispatch_Release(disp);
}

static void test_createfolder(void)
{
    HRESULT hr;
    WCHAR pathW[MAX_PATH];
    BSTR path;
    IFolder *folder;

    /* create existing directory */
    GetCurrentDirectoryW(sizeof(pathW)/sizeof(WCHAR), pathW);
    path = SysAllocString(pathW);
    folder = (void*)0xdeabeef;
    hr = IFileSystem3_CreateFolder(fs3, path, &folder);
    ok(hr == CTL_E_FILEALREADYEXISTS, "got 0x%08x\n", hr);
    ok(folder == NULL, "got %p\n", folder);
    SysFreeString(path);
}

static void test_textstream(void)
{
    static WCHAR testfileW[] = {'t','e','s','t','f','i','l','e','.','t','x','t',0};
    ITextStream *stream;
    VARIANT_BOOL b;
    HANDLE file;
    HRESULT hr;
    BSTR name, data;

    file = CreateFileW(testfileW, GENERIC_READ, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CloseHandle(file);

    name = SysAllocString(testfileW);
    b = VARIANT_FALSE;
    hr = IFileSystem3_FileExists(fs3, name, &b);
    ok(hr == S_OK, "got 0x%08x, expected 0x%08x\n", hr, S_OK);
    ok(b == VARIANT_TRUE, "got %x\n", b);

    hr = IFileSystem3_OpenTextFile(fs3, name, ForReading, VARIANT_FALSE, TristateFalse, &stream);
    ok(hr == S_OK, "got 0x%08x\n", hr);

    b = 10;
    hr = ITextStream_get_AtEndOfStream(stream, &b);
todo_wine {
    ok(hr == S_FALSE || broken(hr == S_OK), "got 0x%08x\n", hr);
    ok(b == VARIANT_TRUE, "got 0x%x\n", b);
}
    ITextStream_Release(stream);

    hr = IFileSystem3_OpenTextFile(fs3, name, ForWriting, VARIANT_FALSE, TristateFalse, &stream);
    ok(hr == S_OK, "got 0x%08x\n", hr);

    b = 10;
    hr = ITextStream_get_AtEndOfStream(stream, &b);
todo_wine {
    ok(hr == CTL_E_BADFILEMODE, "got 0x%08x\n", hr);
    ok(b == VARIANT_TRUE || broken(b == 10), "got 0x%x\n", b);
}
    b = 10;
    hr = ITextStream_get_AtEndOfLine(stream, &b);
todo_wine {
    ok(hr == CTL_E_BADFILEMODE, "got 0x%08x\n", hr);
    ok(b == VARIANT_FALSE || broken(b == 10), "got 0x%x\n", b);
}
    hr = ITextStream_Read(stream, 1, &data);
    ok(hr == CTL_E_BADFILEMODE, "got 0x%08x\n", hr);

    hr = ITextStream_ReadLine(stream, &data);
    ok(hr == CTL_E_BADFILEMODE, "got 0x%08x\n", hr);

    hr = ITextStream_ReadAll(stream, &data);
    ok(hr == CTL_E_BADFILEMODE, "got 0x%08x\n", hr);

    ITextStream_Release(stream);

    hr = IFileSystem3_OpenTextFile(fs3, name, ForAppending, VARIANT_FALSE, TristateFalse, &stream);
    ok(hr == S_OK, "got 0x%08x\n", hr);
    SysFreeString(name);

    b = 10;
    hr = ITextStream_get_AtEndOfStream(stream, &b);
todo_wine {
    ok(hr == CTL_E_BADFILEMODE, "got 0x%08x\n", hr);
    ok(b == VARIANT_TRUE || broken(b == 10), "got 0x%x\n", b);
}
    b = 10;
    hr = ITextStream_get_AtEndOfLine(stream, &b);
todo_wine {
    ok(hr == CTL_E_BADFILEMODE, "got 0x%08x\n", hr);
    ok(b == VARIANT_FALSE || broken(b == 10), "got 0x%x\n", b);
}
    hr = ITextStream_Read(stream, 1, &data);
    ok(hr == CTL_E_BADFILEMODE, "got 0x%08x\n", hr);

    hr = ITextStream_ReadLine(stream, &data);
    ok(hr == CTL_E_BADFILEMODE, "got 0x%08x\n", hr);

    hr = ITextStream_ReadAll(stream, &data);
    ok(hr == CTL_E_BADFILEMODE, "got 0x%08x\n", hr);

    ITextStream_Release(stream);

    DeleteFileW(testfileW);
}

static void test_GetFileVersion(void)
{
    static const WCHAR k32W[] = {'\\','k','e','r','n','e','l','3','2','.','d','l','l',0};
    static const WCHAR k33W[] = {'\\','k','e','r','n','e','l','3','3','.','d','l','l',0};
    WCHAR pathW[MAX_PATH], filenameW[MAX_PATH];
    BSTR path, version;
    HRESULT hr;

    GetSystemDirectoryW(pathW, sizeof(pathW)/sizeof(WCHAR));

    lstrcpyW(filenameW, pathW);
    lstrcatW(filenameW, k32W);

    path = SysAllocString(filenameW);
    hr = IFileSystem3_GetFileVersion(fs3, path, &version);
    ok(hr == S_OK, "got 0x%08x\n", hr);
    ok(*version != 0, "got %s\n", wine_dbgstr_w(version));
    SysFreeString(version);
    SysFreeString(path);

    lstrcpyW(filenameW, pathW);
    lstrcatW(filenameW, k33W);

    path = SysAllocString(filenameW);
    version = (void*)0xdeadbeef;
    hr = IFileSystem3_GetFileVersion(fs3, path, &version);
    ok(broken(hr == S_OK) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), "got 0x%08x\n", hr);
    if (hr == S_OK)
    {
        ok(*version == 0, "got %s\n", wine_dbgstr_w(version));
        SysFreeString(version);
    }
    else
        ok(version == (void*)0xdeadbeef, "got %p\n", version);
    SysFreeString(path);
}

static void test_GetParentFolderName(void)
{
    static const WCHAR path1[] = {'a',0};
    static const WCHAR path2[] = {'a','/','a','/','a',0};
    static const WCHAR path3[] = {'a','\\','a','\\','a',0};
    static const WCHAR path4[] = {'a','/','a','/','/','\\','\\',0};
    static const WCHAR path5[] = {'c',':','\\','\\','a',0};
    static const WCHAR path6[] = {'a','c',':','\\','a',0};
    static const WCHAR result2[] = {'a','/','a',0};
    static const WCHAR result3[] = {'a','\\','a',0};
    static const WCHAR result4[] = {'a',0};
    static const WCHAR result5[] = {'c',':','\\',0};
    static const WCHAR result6[] = {'a','c',':',0};

    static const struct {
        const WCHAR *path;
        const WCHAR *result;
    } tests[] = {
        {NULL, NULL},
        {path1, NULL},
        {path2, result2},
        {path3, result3},
        {path4, result4},
        {path5, result5},
        {path6, result6}
    };

    BSTR path, result;
    HRESULT hr;
    int i;

    hr = IFileSystem3_GetParentFolderName(fs3, NULL, NULL);
    ok(hr == E_POINTER, "GetParentFolderName returned %x, expected E_POINTER\n", hr);

    for(i=0; i<sizeof(tests)/sizeof(tests[0]); i++) {
        result = (BSTR)0xdeadbeef;
        path = tests[i].path ? SysAllocString(tests[i].path) : NULL;
        hr = IFileSystem3_GetParentFolderName(fs3, path, &result);
        ok(hr == S_OK, "%d) GetParentFolderName returned %x, expected S_OK\n", i, hr);
        if(!tests[i].result)
            ok(!result, "%d) result = %s\n", i, wine_dbgstr_w(result));
        else
            ok(!lstrcmpW(result, tests[i].result), "%d) result = %s\n", i, wine_dbgstr_w(result));
        SysFreeString(path);
        SysFreeString(result);
    }
}

static void test_GetFileName(void)
{
    static const WCHAR path1[] = {'a',0};
    static const WCHAR path2[] = {'a','/','a','.','b',0};
    static const WCHAR path3[] = {'a','\\',0};
    static const WCHAR path4[] = {'c',':',0};
    static const WCHAR path5[] = {'/','\\',0};
    static const WCHAR result2[] = {'a','.','b',0};
    static const WCHAR result3[] = {'a',0};

    static const struct {
        const WCHAR *path;
        const WCHAR *result;
    } tests[] = {
        {NULL, NULL},
        {path1, path1},
        {path2, result2},
        {path3, result3},
        {path4, NULL},
        {path5, NULL}
    };

    BSTR path, result;
    HRESULT hr;
    int i;

    hr = IFileSystem3_GetFileName(fs3, NULL, NULL);
    ok(hr == E_POINTER, "GetFileName returned %x, expected E_POINTER\n", hr);

    for(i=0; i<sizeof(tests)/sizeof(tests[0]); i++) {
        result = (BSTR)0xdeadbeef;
        path = tests[i].path ? SysAllocString(tests[i].path) : NULL;
        hr = IFileSystem3_GetFileName(fs3, path, &result);
        ok(hr == S_OK, "%d) GetFileName returned %x, expected S_OK\n", i, hr);
        if(!tests[i].result)
            ok(!result, "%d) result = %s\n", i, wine_dbgstr_w(result));
        else
            ok(!lstrcmpW(result, tests[i].result), "%d) result = %s\n", i, wine_dbgstr_w(result));
        SysFreeString(path);
        SysFreeString(result);
    }
}

static void test_GetBaseName(void)
{
    static const WCHAR path1[] = {'a',0};
    static const WCHAR path2[] = {'a','/','a','.','b','.','c',0};
    static const WCHAR path3[] = {'a','.','b','\\',0};
    static const WCHAR path4[] = {'c',':',0};
    static const WCHAR path5[] = {'/','\\',0};
    static const WCHAR path6[] = {'.','a',0};
    static const WCHAR result1[] = {'a',0};
    static const WCHAR result2[] = {'a','.','b',0};
    static const WCHAR result6[] = {0};

    static const struct {
        const WCHAR *path;
        const WCHAR *result;
    } tests[] = {
        {NULL, NULL},
        {path1, result1},
        {path2, result2},
        {path3, result1},
        {path4, NULL},
        {path5, NULL},
        {path6, result6}
    };

    BSTR path, result;
    HRESULT hr;
    int i;

    hr = IFileSystem3_GetBaseName(fs3, NULL, NULL);
    ok(hr == E_POINTER, "GetBaseName returned %x, expected E_POINTER\n", hr);

    for(i=0; i<sizeof(tests)/sizeof(tests[0]); i++) {
        result = (BSTR)0xdeadbeef;
        path = tests[i].path ? SysAllocString(tests[i].path) : NULL;
        hr = IFileSystem3_GetBaseName(fs3, path, &result);
        ok(hr == S_OK, "%d) GetBaseName returned %x, expected S_OK\n", i, hr);
        if(!tests[i].result)
            ok(!result, "%d) result = %s\n", i, wine_dbgstr_w(result));
        else
            ok(!lstrcmpW(result, tests[i].result), "%d) result = %s\n", i, wine_dbgstr_w(result));
        SysFreeString(path);
        SysFreeString(result);
    }
}

static void test_GetAbsolutePathName(void)
{
    static const WCHAR dir1[] = {'t','e','s','t','_','d','i','r','1',0};
    static const WCHAR dir2[] = {'t','e','s','t','_','d','i','r','2',0};
    static const WCHAR dir_match1[] = {'t','e','s','t','_','d','i','r','*',0};
    static const WCHAR dir_match2[] = {'t','e','s','t','_','d','i','*',0};
    static const WCHAR cur_dir[] = {'.',0};

    WIN32_FIND_DATAW fdata;
    HANDLE find;
    WCHAR buf[MAX_PATH];
    BSTR path, result;
    HRESULT hr;

    hr = IFileSystem3_GetAbsolutePathName(fs3, NULL, NULL);
    ok(hr == E_POINTER, "GetAbsolutePathName returned %x, expected E_POINTER\n", hr);

    hr = IFileSystem3_GetAbsolutePathName(fs3, NULL, &result);
    ok(hr == S_OK, "GetAbsolutePathName returned %x, expected S_OK\n", hr);
    GetFullPathNameW(cur_dir, MAX_PATH, buf, NULL);
    ok(!lstrcmpW(buf, result), "result = %s, expected %s\n", wine_dbgstr_w(result), wine_dbgstr_w(buf));
    SysFreeString(result);

    find = FindFirstFileW(dir_match2, &fdata);
    if(find != INVALID_HANDLE_VALUE) {
        skip("GetAbsolutePathName tests\n");
        FindClose(find);
        return;
    }

    path = SysAllocString(dir_match1);
    hr = IFileSystem3_GetAbsolutePathName(fs3, path, &result);
    ok(hr == S_OK, "GetAbsolutePathName returned %x, expected S_OK\n", hr);
    GetFullPathNameW(dir_match1, MAX_PATH, buf, NULL);
    ok(!lstrcmpW(buf, result), "result = %s, expected %s\n", wine_dbgstr_w(result), wine_dbgstr_w(buf));
    SysFreeString(result);

    ok(CreateDirectoryW(dir1, NULL), "CreateDirectory(%s) failed\n", wine_dbgstr_w(dir1));
    hr = IFileSystem3_GetAbsolutePathName(fs3, path, &result);
    ok(hr == S_OK, "GetAbsolutePathName returned %x, expected S_OK\n", hr);
    GetFullPathNameW(dir1, MAX_PATH, buf, NULL);
    ok(!lstrcmpW(buf, result), "result = %s, expected %s\n", wine_dbgstr_w(result), wine_dbgstr_w(buf));
    SysFreeString(result);

    ok(CreateDirectoryW(dir2, NULL), "CreateDirectory(%s) failed\n", wine_dbgstr_w(dir2));
    hr = IFileSystem3_GetAbsolutePathName(fs3, path, &result);
    ok(hr == S_OK, "GetAbsolutePathName returned %x, expected S_OK\n", hr);
    if(!lstrcmpW(buf, result)) {
        ok(!lstrcmpW(buf, result), "result = %s, expected %s\n",
                wine_dbgstr_w(result), wine_dbgstr_w(buf));
    }else {
        GetFullPathNameW(dir2, MAX_PATH, buf, NULL);
        ok(!lstrcmpW(buf, result), "result = %s, expected %s\n",
                wine_dbgstr_w(result), wine_dbgstr_w(buf));
    }
    SysFreeString(result);

    SysFreeString(path);
    path = SysAllocString(dir_match2);
    hr = IFileSystem3_GetAbsolutePathName(fs3, path, &result);
    ok(hr == S_OK, "GetAbsolutePathName returned %x, expected S_OK\n", hr);
    GetFullPathNameW(dir_match2, MAX_PATH, buf, NULL);
    ok(!lstrcmpW(buf, result), "result = %s, expected %s\n", wine_dbgstr_w(result), wine_dbgstr_w(buf));
    SysFreeString(result);
    SysFreeString(path);

    RemoveDirectoryW(dir1);
    RemoveDirectoryW(dir2);
}

static void test_GetFile(void)
{
    static const WCHAR get_file[] = {'g','e','t','_','f','i','l','e','.','t','s','t',0};

    BSTR path = SysAllocString(get_file);
    FileAttribute fa;
    DWORD gfa;
    IFile *file;
    HRESULT hr;
    HANDLE hf;

    hr = IFileSystem3_GetFile(fs3, path, NULL);
    ok(hr == E_POINTER, "GetFile returned %x, expected E_POINTER\n", hr);
    hr = IFileSystem3_GetFile(fs3, NULL, &file);
    ok(hr == E_INVALIDARG, "GetFile returned %x, expected E_INVALIDARG\n", hr);

    hf = CreateFileW(path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if(hf != INVALID_HANDLE_VALUE) {
        CloseHandle(hf);
        skip("File already exists, skipping GetFile tests\n");
        SysFreeString(path);
        return;
    }

    file = (IFile*)0xdeadbeef;
    hr = IFileSystem3_GetFile(fs3, path, &file);
    ok(!file, "file != NULL\n");
    ok(hr == CTL_E_FILENOTFOUND, "GetFile returned %x, expected CTL_E_FILENOTFOUND\n", hr);

    hf = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if(hf == INVALID_HANDLE_VALUE) {
        skip("Can't create temporary file\n");
        SysFreeString(path);
        return;
    }
    CloseHandle(hf);

    hr = IFileSystem3_GetFile(fs3, path, &file);
    ok(hr == S_OK, "GetFile returned %x, expected S_OK\n", hr);

    hr = IFile_get_Attributes(file, &fa);
    gfa = GetFileAttributesW(get_file) & ~FILE_ATTRIBUTE_NORMAL;
    ok(hr == S_OK, "get_Attributes returned %x, expected S_OK\n", hr);
    ok(fa == gfa, "fa = %x, expected %x\n", fa, gfa);
    IFile_Release(file);

    DeleteFileW(path);
    SysFreeString(path);
}

START_TEST(filesystem)
{
    HRESULT hr;

    CoInitialize(NULL);

    hr = CoCreateInstance(&CLSID_FileSystemObject, NULL, CLSCTX_INPROC_SERVER|CLSCTX_INPROC_HANDLER,
            &IID_IFileSystem3, (void**)&fs3);
    if(FAILED(hr)) {
        win_skip("Could not create FileSystem object: %08x\n", hr);
        return;
    }

    test_interfaces();
    test_createfolder();
    test_textstream();
    test_GetFileVersion();
    test_GetParentFolderName();
    test_GetFileName();
    test_GetBaseName();
    test_GetAbsolutePathName();
    test_GetFile();

    IFileSystem3_Release(fs3);

    CoUninitialize();
}
