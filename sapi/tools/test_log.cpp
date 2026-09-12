// Exercises DebugLog directly against the real HKCU\Software\DECtalkDTC01
// key and the real %LOCALAPPDATA%\DECtalkDTC01\dectalk-sapi.log file (there
// is no registry/filesystem sandbox available to a command-line tool). Same
// HKCU cleanup discipline as sapi/tools/test_settings.cpp: read/save the
// prior Logging value (or note it was absent) and restore it at the end.
#include "debug_log.h"
#include <cassert>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kRootKey[] = L"Software\\DECtalkDTC01";
constexpr wchar_t kValueName[] = L"Logging";

bool ReadLogging(DWORD& out_value)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRootKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const bool ok = RegQueryValueExW(key, kValueName, nullptr, &type,
                                      reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
                    type == REG_DWORD;
    RegCloseKey(key);
    if (ok) {
        out_value = value;
    }
    return ok;
}

void WriteLogging(DWORD value)
{
    HKEY key = nullptr;
    assert(RegCreateKeyExW(HKEY_CURRENT_USER, kRootKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                            nullptr) == ERROR_SUCCESS);
    assert(RegSetValueExW(key, kValueName, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
                           sizeof(value)) == ERROR_SUCCESS);
    RegCloseKey(key);
}

void DeleteLogging()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRootKey, 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, kValueName);
        RegCloseKey(key);
    }
}

// Mirrors DebugLog::BuildLogPath's own resolution so the test can find/delete
// the same file the log actually writes to.
std::wstring LogPath()
{
    wchar_t path[MAX_PATH];
    assert(DebugLog::BuildLogPath(path, MAX_PATH, L""));
    return path;
}

bool FileExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool ReadFile(const std::wstring& path, std::string& out)
{
    FILE* f = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
    if (!f) {
        return false;
    }
    std::vector<char> buf;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        buf.insert(buf.end(), chunk, chunk + n);
    }
    fclose(f);
    out.assign(buf.begin(), buf.end());
    return true;
}

long long FileSize(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        return -1;
    }
    return (static_cast<long long>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

}  // namespace

int wmain()
{
    DWORD prior_value = 0;
    const bool had_prior = ReadLogging(prior_value);

    // --- Logging ON: the marker must land in the file. ---
    WriteLogging(1);
    DebugLog::RefreshEnabled();

    const std::wstring path = LogPath();
    DeleteFileW(path.c_str());

    DECTALK_LOG("test marker %d", 42);

    assert(FileExists(path));
    std::string contents;
    assert(ReadFile(path, contents));
    assert(contents.find("test marker 42") != std::string::npos);

    // --- Logging OFF: a further line must not grow the file. ---
    WriteLogging(0);
    DebugLog::RefreshEnabled();

    const long long size_before = FileSize(path);
    assert(size_before >= 0);

    DECTALK_LOG("should not appear %d", 99);

    const long long size_after = FileSize(path);
    assert(size_after == size_before);

    // --- Clean up: restore whatever was there before this test ran. ---
    if (had_prior) {
        WriteLogging(prior_value);
    } else {
        DeleteLogging();
    }
    DebugLog::RefreshEnabled();

    return 0;
}
