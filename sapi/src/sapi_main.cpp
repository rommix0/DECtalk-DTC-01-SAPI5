// sapi_main.cpp - the four exported COM entry points for
// DectalkDtc01SAPI.dll: DllGetClassObject / DllCanUnloadNow /
// DllRegisterServer / DllUnregisterServer, plus DllMain to capture this
// module's HMODULE. Adapted from BstSpeech-sapi-master's src/sapi_main.cpp,
// trimmed to this driver's scope: a single coclass (DectalkTtsEngine, Task
// B2), no custom-voice enumerator, no 32-bit pipe shim (the DECtalk core
// builds natively at both bitnesses -- see DectalkTtsEngine.hpp).
//
// DectalkTtsEngine resolves its own module (for module-relative ROM/core-dll
// lookup) via GetModuleHandleExW(..._FROM_ADDRESS) on a function address that
// lives in this DLL, so it needs nothing from DllMain for that; DllMain only
// has to remember the HMODULE for InprocServer32 registration.
#include <new>
#include <windows.h>
#include <sapi.h>

#include "com.hpp"
#include "voice_registry.hpp"
#include "DectalkTtsEngine.hpp"

namespace {

HINSTANCE g_dll_handle = nullptr;
dectalk::com::class_object_factory g_cls_obj_factory;

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID /*lpReserved*/)
{
    if (dwReason == DLL_PROCESS_ATTACH) {
        g_dll_handle = hInstance;
        DisableThreadLibraryCalls(hInstance);

        try {
            g_cls_obj_factory.register_class<dtc01::sapi::DectalkTtsEngine>();
        }
        catch (...) {
            return FALSE;
        }
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv)
{
    return g_cls_obj_factory.create(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow()
{
    return dectalk::com::object_counter::is_zero() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer()
{
    try {
        dectalk::com::class_registrar r(g_dll_handle);
        r.register_class<dtc01::sapi::DectalkTtsEngine>();

        // clsid_as_string<T>() formats __uuidof(T) via StringFromCLSID, which
        // for this coclass's uuid ("E877CE12-F153-40DF-AEAC-2AFA4D4A3DE8")
        // always yields exactly "{E877CE12-F153-40DF-AEAC-2AFA4D4A3DE8}" --
        // deriving it here instead of repeating the literal means there is
        // nothing to let drift out of sync with the coclass's real CLSID.
        const std::wstring clsid_str =
            dectalk::com::clsid_as_string<dtc01::sapi::DectalkTtsEngine>();
        dectalk::sapi::write_voice_tokens(HKEY_LOCAL_MACHINE, clsid_str);
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}

STDAPI DllUnregisterServer()
{
    try {
        dectalk::sapi::remove_voice_tokens(HKEY_LOCAL_MACHINE);
        dectalk::com::class_registrar r(g_dll_handle);
        r.unregister_class<dtc01::sapi::DectalkTtsEngine>();
        return S_OK;
    }
    catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    }
    catch (...) {
        return E_UNEXPECTED;
    }
}
