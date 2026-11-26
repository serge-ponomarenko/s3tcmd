#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "util.h"

namespace s3tc {

namespace {

std::mutex g_mutex;
std::wstring g_path;
// Kept open: opening and closing the file per line made a TC walk over 81,000 cached folders take
// 48 s instead of 1 s (measured).
HANDLE g_file = INVALID_HANDLE_VALUE;

void CloseLocked() {
    if (g_file != INVALID_HANDLE_VALUE) CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
}

}  // namespace

void LogSetFile(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (path == g_path) return;
    CloseLocked();
    g_path = path;
}

bool LogEnabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return !g_path.empty();
}

void Log(const wchar_t* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_path.empty()) return;

    wchar_t msg[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t prefix[64];
    swprintf_s(prefix, L"%02u:%02u:%02u.%03u [%5lu] ", st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds, GetCurrentThreadId());

    if (g_file == INVALID_HANDLE_VALUE) {
        // FILE_APPEND_DATA: every write goes to the end, also if another TC instance appends.
        g_file = CreateFileW(g_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_file == INVALID_HANDLE_VALUE) return;
    }
    std::string line = WideToUtf8(std::wstring(prefix) + msg + L"\r\n");
    DWORD written = 0;
    if (!WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)) {
        CloseLocked();  // e.g. the file was deleted: reopen on the next line
    }
}

}  // namespace s3tc
