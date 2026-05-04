#include "logs.h"

#include "service.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>

namespace sapisrv::log {

namespace {

std::mutex g_mu;
std::wstring g_dir;
HANDLE g_file = INVALID_HANDLE_VALUE;
int g_open_yyyymmdd = 0;
DWORD g_pid = 0;

int today_yyyymmdd() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

std::wstring file_path_for(int yyyymmdd) {
    wchar_t buf[64];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"\\sapisrv-%08d.log", yyyymmdd);
    return g_dir + buf;
}

// Caller holds g_mu.
void ensure_open_locked() {
    int today = today_yyyymmdd();
    if (g_file != INVALID_HANDLE_VALUE && today == g_open_yyyymmdd) return;
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
    auto path = file_path_for(today);
    g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file != INVALID_HANDLE_VALUE) g_open_yyyymmdd = today;
}

void emit(const char* level, const char* fmt, va_list ap) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char prefix[64];
    int plen = _snprintf_s(prefix, sizeof(prefix), _TRUNCATE,
                           "%04d-%02d-%02dT%02d:%02d:%02d.%03d %s [%lu] ",
                           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                           st.wMilliseconds, level, g_pid);
    char body[1024];
    int blen = _vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    if (blen < 0) blen = static_cast<int>(strlen(body));

    char line[1200];
    int total = _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s\n",
                            prefix, body);
    if (total < 0) total = static_cast<int>(strlen(line));

    fwrite(line, 1, total, stderr);
    fflush(stderr);

    std::lock_guard<std::mutex> lk(g_mu);
    ensure_open_locked();
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_file, line, total, &written, nullptr);
    }
}

}  // namespace

void init(const std::wstring& log_dir) {
    g_pid = GetCurrentProcessId();
    g_dir = log_dir;
    // Best-effort directory creation. Walks the path and creates each segment.
    std::wstring p;
    for (size_t i = 0; i < g_dir.size(); ++i) {
        p.push_back(g_dir[i]);
        if (g_dir[i] == L'\\' || g_dir[i] == L'/') {
            CreateDirectoryW(p.c_str(), nullptr);
        }
    }
    CreateDirectoryW(g_dir.c_str(), nullptr);
    std::lock_guard<std::mutex> lk(g_mu);
    ensure_open_locked();
}

void info (const char* fmt, ...) { va_list ap; va_start(ap, fmt); emit("INFO ", fmt, ap); va_end(ap); }
void warn (const char* fmt, ...) { va_list ap; va_start(ap, fmt); emit("WARN ", fmt, ap); va_end(ap); }
void error(const char* fmt, ...) { va_list ap; va_start(ap, fmt); emit("ERROR", fmt, ap); va_end(ap); }

std::wstring default_log_dir() {
    wchar_t override_buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"SAPISRV_LOGS_DIR", override_buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return override_buf;
    wchar_t pd[MAX_PATH] = {0};
    DWORD pn = GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH);
    if (pn == 0 || pn >= MAX_PATH) wcscpy_s(pd, MAX_PATH, L"C:\\ProgramData");
    return std::wstring(pd) + L"\\sapicli\\logs";
}

int run_logs_follow(const std::wstring& log_dir) {
    int day = today_yyyymmdd();
    wchar_t buf[64];
    _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"\\sapisrv-%08d.log", day);
    std::wstring path = log_dir + buf;

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"logs: cannot open %s (file may not exist yet)\n", path.c_str());
        return 1;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    LARGE_INTEGER pos = sz;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);

    char chunk[4096];
    HANDLE stop = sapisrv::stop_event();
    for (;;) {
        if (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) break;

        // Day rollover?
        int now_day = today_yyyymmdd();
        if (now_day != day) {
            CloseHandle(h);
            day = now_day;
            _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"\\sapisrv-%08d.log", day);
            path = log_dir + buf;
            h = CreateFileW(path.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                Sleep(200);
                continue;
            }
        }

        DWORD got = 0;
        if (!ReadFile(h, chunk, sizeof(chunk), &got, nullptr)) {
            Sleep(200);
            continue;
        }
        if (got == 0) {
            Sleep(200);
            continue;
        }
        fwrite(chunk, 1, got, stdout);
        fflush(stdout);
    }
    CloseHandle(h);
    return 0;
}

}  // namespace sapisrv::log
