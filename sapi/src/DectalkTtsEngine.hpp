// DectalkTtsEngine.hpp - the SAPI5 TTS engine coclass for the DECtalk DTC-01.
//
// Implements ISpTTSEngine (Speak / GetOutputFormat) and ISpObjectWithToken
// (SetObjectToken / GetObjectToken); IUnknown is supplied by com.hpp's
// IUnknownImpl<T> template (this class is only ever instantiated through
// dectalk::com::object<DectalkTtsEngine>, exactly like BstSpeech's
// ISpTTSEngineImpl).
//
// Unlike the BstSpeech template this is adapted from, there is no 32-bit shim,
// helper process or pipe bridge: the DECtalk core builds natively at both
// bitnesses, so this drives a dtc01::Machine directly in-process. The vendored
// 68000 core keeps CPU state in process globals, so every Machine call --
// create, consume_boot_announcement, feed_text, run_block, is_idle,
// set_volume, and destruction -- happens while dtc01::exec_mutex() is held.
#pragma once

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <comdef.h>

#include <memory>
#include <string>

#include "com.hpp"
#include "dtc01_core.hpp"
#include "voices.hpp"

namespace dtc01 {
namespace sapi {

class __declspec(uuid("{E877CE12-F153-40DF-AEAC-2AFA4D4A3DE8}")) DectalkTtsEngine :
    public ISpTTSEngine, public ISpObjectWithToken
{
public:
    DectalkTtsEngine();
    ~DectalkTtsEngine();

    DectalkTtsEngine(const DectalkTtsEngine&) = delete;
    DectalkTtsEngine& operator=(const DectalkTtsEngine&) = delete;

    // ISpTTSEngine
    STDMETHOD(Speak)(DWORD dwSpeakFlags, REFGUID rguidFormatId,
                     const WAVEFORMATEX* pWaveFormatEx, const SPVTEXTFRAG* pTextFragList,
                     ISpTTSEngineSite* pOutputSite) override;
    STDMETHOD(GetOutputFormat)(const GUID* pTargetFmtId, const WAVEFORMATEX* pTargetWaveFormatEx,
                               GUID* pOutputFormatId, WAVEFORMATEX** ppCoMemOutputWaveFormatEx) override;

    // ISpObjectWithToken
    STDMETHOD(SetObjectToken)(ISpObjectToken* pToken) override;
    STDMETHOD(GetObjectToken)(ISpObjectToken** ppToken) override;

protected:
    // Consulted by com::IUnknownImpl<DectalkTtsEngine>::QueryInterface.
    [[nodiscard]] void* get_interface(REFIID riid) noexcept
    {
        void* ptr = dectalk::com::try_primary_interface<ISpTTSEngine>(this, riid);
        return ptr ? ptr : dectalk::com::try_interface<ISpObjectWithToken>(this, riid);
    }

private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpDataKey, __uuidof(ISpDataKey));

    // Lazily create the Machine (and consume the power-on announcement) on
    // first Speak. Must be called with exec_mutex() already held. Returns
    // false and leaves machine_ null if the ROMs/DLL could not be resolved or
    // the core failed to start.
    [[nodiscard]] bool ensure_machine();

    // Puts machine_ back in the state it had right after its boot
    // announcement, from the process-wide cache ensure_machine() fills.
    // Must be called with exec_mutex() held and machine_ created. Returns
    // false if the core DLL has no state snapshots.
    [[nodiscard]] bool rewind_machine();

    ISpObjectTokenPtr token_;

    // Resolved from the token's Attributes in SetObjectToken.
    std::string voice_key_;   // e.g. "paul"
    std::string firmware_;    // "v20" / "v18"
    std::string mnemonic_;    // e.g. "np"
    bool voice_resolved_ = false;

    std::unique_ptr<Machine> machine_;
    std::wstring boot_key_;   // this machine's entry in the booted-state cache
};

}  // namespace sapi
}  // namespace dtc01
