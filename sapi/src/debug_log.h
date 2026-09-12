#pragma once

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <share.h>

// Diagnostic log shared by the SAPI engine DLL, the DectalkConfig utility, and
// the native-core layer.
//
// Written to %LOCALAPPDATA%\DECtalkDTC01\dectalk-sapi.log, which is writable
// without elevation -- the install directory is not, and a SAPI engine runs
// inside whatever application is speaking. Every line carries the process
// name, its bitness and its pid, because a single utterance from a 64-bit
// host crosses two processes (the host and the 32-bit worker), both of which
// append to the same file.
//
// Turn it off without reinstalling by creating this registry value:
//   HKCU\Software\DECtalkDTC01  DWORD  Logging = 0
// and back on with Logging = 1 (or by deleting the value -- absent means on).
// The value is cached once per process on the hot path; see RefreshEnabled()
// below for the test-only seam that re-reads it.
//
// The file is capped and rotated to one previous copy, so leaving it on
// cannot fill a disk during a long session.

namespace DebugLog {

inline constexpr long MAX_LOG_BYTES = 4 * 1024 * 1024;

inline bool BuildLogPath(wchar_t* path, size_t size, const wchar_t* suffix)
{
    wchar_t base[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) == 0) {
        if (GetTempPathW(MAX_PATH, base) == 0) {
            return false;
        }
    }
    wchar_t dir[MAX_PATH];
    if (swprintf_s(dir, L"%s\\DECtalkDTC01", base) < 0) {
        return false;
    }
    CreateDirectoryW(dir, nullptr);
    return swprintf_s(path, size, L"%s\\dectalk-sapi%s.log", dir, suffix) >= 0;
}

namespace detail {

inline int& CachedEnabled()
{
    static int cached = -1;
    return cached;
}

// Absent value (or absent key) means logging is ON by default.
inline int ReadRegistryEnabled()
{
    int result = 1;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\DECtalkDTC01", 0, KEY_READ, &key)
        == ERROR_SUCCESS) {
        DWORD value = 1;
        DWORD size = sizeof(value);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"Logging", nullptr, &type,
                             reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD) {
            result = (value != 0) ? 1 : 0;
        }
        RegCloseKey(key);
    }
    return result;
}

}  // namespace detail

// Read once per process: a hot path should not touch the registry per line.
inline bool Enabled()
{
    int& cached = detail::CachedEnabled();
    if (cached < 0) {
        cached = detail::ReadRegistryEnabled();
    }
    return cached == 1;
}

// Test-only seam: re-reads HKCU\Software\DECtalkDTC01\Logging and updates the
// cached flag, so a test can flip Logging within a single process and see
// DECTALK_LOG react on the very next call. Nothing on the DECTALK_LOG hot
// path calls this -- Enabled() above still only reads the registry once.
inline void RefreshEnabled()
{
    detail::CachedEnabled() = detail::ReadRegistryEnabled();
}

inline const char* ProcessTag()
{
    static char tag[64] = {};
    if (!tag[0]) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* name = wcsrchr(exe, L'\\');
        name = name ? name + 1 : exe;
        char narrow[48] = {};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow) - 1, nullptr, nullptr);
        sprintf_s(tag, "%s/%d-bit pid %lu", narrow,
                  static_cast<int>(sizeof(void*) * 8), GetCurrentProcessId());
    }
    return tag;
}

inline void Rotate(const wchar_t* path)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &info)) {
        return;
    }
    if (info.nFileSizeHigh == 0 && info.nFileSizeLow < static_cast<DWORD>(MAX_LOG_BYTES)) {
        return;
    }
    wchar_t previous[MAX_PATH];
    if (BuildLogPath(previous, MAX_PATH, L".1")) {
        DeleteFileW(previous);
        MoveFileW(path, previous);
    }
}

// Speech latency is the whole point of this engine, so a log line costs one
// write and one flush rather than an open, a write and a close. The handle is
// kept for the life of the process; the flush is what keeps the file useful
// if the host crashes.
//
// The share mode matters more than it looks: _wfopen_s opens a file for
// EXCLUSIVE access, so the first process to log would lock every other one
// out for as long as it ran -- and since a screen reader host and the 32-bit
// worker are both long-lived, whichever started first would silently steal
// the log from everyone else. _wfsopen with _SH_DENYNO lets them all append.
// The open is also retried rather than latched, so a process that starts
// while the file is briefly unavailable still ends up logging.
inline FILE* Handle()
{
    static FILE* file = nullptr;
    static DWORD next_try = 0;

    if (!file) {
        const DWORD now = GetTickCount();
        if (next_try != 0 && now < next_try) {
            return nullptr;
        }
        next_try = now + 5000;

        wchar_t path[MAX_PATH];
        if (BuildLogPath(path, MAX_PATH, L"")) {
            Rotate(path);
            file = _wfsopen(path, L"a", _SH_DENYNO);
        }
    }
    return file;
}

inline void Log(const char* format, ...)
{
    if (!Enabled()) {
        return;
    }
    FILE* file = Handle();
    if (!file) {
        return;
    }

    // The whole line is composed first and written once: the SAPI engine and
    // the 32-bit worker both append to this file, and a single write per line
    // keeps their output from interleaving mid-line.
    char line[2048];
    SYSTEMTIME now;
    GetLocalTime(&now);
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
                        "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] ",
                        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                        now.wSecond, now.wMilliseconds, ProcessTag());
    if (n < 0) {
        return;
    }

    va_list args;
    va_start(args, format);
    const int m = _vsnprintf_s(line + n, sizeof(line) - n, _TRUNCATE, format, args);
    va_end(args);
    if (m > 0) {
        n += m;
    }
    if (n < static_cast<int>(sizeof(line)) - 1) {
        line[n++] = '\n';
    }

    fwrite(line, 1, static_cast<size_t>(n), file);
    fflush(file);
}

}  // namespace DebugLog

#define DECTALK_LOG(...) DebugLog::Log(__VA_ARGS__)
