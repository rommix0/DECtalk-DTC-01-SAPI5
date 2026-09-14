// Exercises DebugLog against the real HKCU\Software\DECtalkDTC01 key -- there
// is no registry sandbox for a command-line tool, so the settings already
// there are copied aside and put back when the test ends
// (settings_backup.hpp). The log itself goes to a scratch folder: LOCALAPPDATA
// is pointed there for this process, so the test never touches a real
// %LOCALAPPDATA%\DECtalkDTC01\dectalk-sapi.log.
#include "debug_log.h"
#include "settings_backup.hpp"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kRootKey[] = L"Software\\DECtalkDTC01";
constexpr wchar_t kValueName[] = L"Logging";

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
    const dectalk::test::SettingsBackup backup;
    if (!backup.ok()) {
        std::fputs("test_log: could not back up HKCU\\Software\\DECtalkDTC01\n", stderr);
        return 1;
    }

    wchar_t temp[MAX_PATH];
    assert(GetTempPathW(MAX_PATH, temp) != 0);
    const std::wstring scratch = std::wstring(temp) + L"dectalk-test-log";
    CreateDirectoryW(scratch.c_str(), nullptr);
    assert(SetEnvironmentVariableW(L"LOCALAPPDATA", scratch.c_str()));

    const std::wstring path = LogPath();
    DeleteFileW(path.c_str());

    // --- No Logging value: the log is off, and no file appears. ---
    DeleteLogging();
    DebugLog::RefreshEnabled();
    assert(!DebugLog::Enabled());
    DECTALK_LOG("off by default %d", 7);
    assert(!FileExists(path));

    // --- Logging = 1: the marker must land in the file. ---
    WriteLogging(1);
    DebugLog::RefreshEnabled();

    DECTALK_LOG("test marker %d", 42);

    assert(FileExists(path));
    std::string contents;
    assert(ReadFile(path, contents));
    assert(contents.find("test marker 42") != std::string::npos);
    assert(contents.find("off by default") == std::string::npos);

    // --- Logging = 0: a further line must not grow the file. ---
    WriteLogging(0);
    DebugLog::RefreshEnabled();

    const long long size_before = FileSize(path);
    assert(size_before >= 0);

    DECTALK_LOG("should not appear %d", 99);

    const long long size_after = FileSize(path);
    assert(size_after == size_before);

    return 0;
}
