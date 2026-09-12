// test_tokens.cpp - TDD coverage for voice_registry.hpp's write_voice_tokens
// / remove_voice_tokens. Uses a throwaway HKCU root
// (Software\DECtalkDTC01Test) so it never touches the real SAPI voice tree;
// write_voice_tokens/remove_voice_tokens are handed that throwaway key as
// `root`, so the 18 tokens land under
// HKCU\Software\DECtalkDTC01Test\Software\Microsoft\Speech\Voices\Tokens\...
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <string>

#include "registry.hpp"
#include "voice_registry.hpp"
#include "voices.hpp"

namespace {

const wchar_t* const kTestRootPath = L"Software\\DECtalkDTC01Test";
const wchar_t* const kTokensSubpath = L"Software\\Microsoft\\Speech\\Voices\\Tokens";

// Recursively delete root\path, tolerant of it not existing.
void delete_tree(HKEY root, const std::wstring& path)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS) {
        return;
    }
    for (;;) {
        wchar_t name[256];
        DWORD name_len = 256;
        if (RegEnumKeyExW(key, 0, name, &name_len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) {
            break;
        }
        delete_tree(key, name);
    }
    RegCloseKey(key);
    RegDeleteKeyW(root, path.c_str());
}

bool subkey_exists(HKEY root, const std::wstring& path)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(key);
    return true;
}

// Number of direct subkeys under root\path, or -1 if the key doesn't exist.
int count_subkeys(HKEY root, const std::wstring& path)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return -1;
    }
    DWORD count = 0;
    LONG rc = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &count, nullptr, nullptr,
                               nullptr, nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return (rc == ERROR_SUCCESS) ? static_cast<int>(count) : -1;
}

}  // namespace

int wmain()
{
    const std::wstring tokens_path = std::wstring(kTestRootPath) + L"\\" + kTokensSubpath;
    const std::wstring clsid_str = L"{12345678-9ABC-DEF0-1234-56789ABCDEF0}";

    // Clean slate, in case a previous run crashed before cleanup.
    delete_tree(HKEY_CURRENT_USER, kTestRootPath);

    HKEY test_root = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kTestRootPath, 0, nullptr, 0,
                              KEY_ALL_ACCESS, nullptr, &test_root, nullptr);
    assert(rc == ERROR_SUCCESS && "failed to create throwaway test root");

    // --- write_voice_tokens ---
    dectalk::sapi::write_voice_tokens(test_root, clsid_str);

    const int written = count_subkeys(HKEY_CURRENT_USER, tokens_path);
    if (written != dtc01::voice_count()) {
        std::fprintf(stderr, "expected %d token subkeys, found %d\n", dtc01::voice_count(), written);
        assert(false);
    }

    for (int i = 0; i < dtc01::voice_count(); ++i) {
        const std::wstring id = dectalk::sapi::token_id_for(dtc01::VOICES[i]);
        assert(subkey_exists(HKEY_CURRENT_USER, tokens_path + L"\\" + id));
    }

    {
        const std::wstring paul_id = L"DECtalk_DTC01_v20_paul";
        assert(subkey_exists(HKEY_CURRENT_USER, tokens_path + L"\\" + paul_id));

        dectalk::registry::key paul_token(test_root, std::wstring(kTokensSubpath) + L"\\" + paul_id, KEY_READ);
        assert(paul_token.get(L"CLSID") == clsid_str);

        dectalk::registry::key attrs(paul_token, L"Attributes", KEY_READ);
        assert(attrs.get(L"DtcVoice") == L"paul");
        assert(attrs.get(L"DtcFirmware") == L"v20");
        assert(attrs.get(L"Language") == L"409");
    }

    // --- remove_voice_tokens ---
    dectalk::sapi::remove_voice_tokens(test_root);

    const int remaining = count_subkeys(HKEY_CURRENT_USER, tokens_path);
    if (remaining != 0) {
        std::fprintf(stderr, "expected 0 token subkeys after removal, found %d\n", remaining);
        assert(false);
    }

    // --- cleanup: never leave the throwaway tree behind ---
    RegCloseKey(test_root);
    delete_tree(HKEY_CURRENT_USER, kTestRootPath);
    assert(!subkey_exists(HKEY_CURRENT_USER, kTestRootPath));

    std::puts("test_tokens: PASS");
    return 0;
}
