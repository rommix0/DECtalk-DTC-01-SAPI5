// test_exports.cpp - TDD coverage for Task B1's COM scaffolding: confirms
// the built DectalkDtc01SAPI.dll exports the four COM entry points SAPI (and
// regsvr32) need. LoadLibraryW's the DLL named on argv[1] and
// GetProcAddress's each export by name, rather than shelling out to
// dumpbin -- deterministic and has no external tool dependency.
//
// Before sapi_main.cpp/the DLL target existed, this failed at LoadLibraryW
// (no such file) -- the RED state for this task.
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t* argv[])
{
    if (argc < 2) {
        std::fwprintf(stderr, L"usage: test_exports <path-to-DectalkDtc01SAPI.dll>\n");
        return 1;
    }

    const wchar_t* dll_path = argv[1];
    HMODULE mod = LoadLibraryW(dll_path);
    if (!mod) {
        std::fwprintf(stderr, L"test_exports: LoadLibraryW(%ls) failed, GetLastError=%lu\n",
                      dll_path, GetLastError());
        return 1;
    }

    const char* const kExports[] = {
        "DllGetClassObject",
        "DllCanUnloadNow",
        "DllRegisterServer",
        "DllUnregisterServer",
    };

    bool all_present = true;
    for (const char* name : kExports) {
        FARPROC proc = GetProcAddress(mod, name);
        if (!proc) {
            std::fprintf(stderr, "test_exports: missing export %s\n", name);
            all_present = false;
        }
    }

    FreeLibrary(mod);

    assert(all_present && "one or more required exports missing");
    if (!all_present) {
        return 1;
    }

    std::puts("test_exports: PASS");
    return 0;
}
