#include "static.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace sapisrv {

namespace {

std::wstring g_www_dir;
std::once_flag g_www_init;

std::wstring resolve_www_dir() {
    wchar_t override_buf[MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"SAPISRV_WWW_DIR", override_buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return override_buf;

    wchar_t exe[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return L".\\www";
    std::wstring path = exe;
    auto sep = path.find_last_of(L"\\/");
    std::wstring exe_dir = (sep == std::wstring::npos) ? L"." : path.substr(0, sep);

    // Try <exe_dir>\www first; if missing, try walking up to find a www\.
    std::wstring candidate = exe_dir + L"\\www";
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;

    // Walk up looking for a www\ sibling (handy in dev: build output is x64\Release).
    std::wstring up = exe_dir;
    for (int i = 0; i < 4; ++i) {
        auto s = up.find_last_of(L"\\/");
        if (s == std::wstring::npos) break;
        up = up.substr(0, s);
        candidate = up + L"\\www";
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
    }
    return exe_dir + L"\\www";
}

const char* mime_for(const std::wstring& path) {
    auto ends_with = [&](const wchar_t* ext) {
        size_t el = wcslen(ext);
        return path.size() >= el && _wcsicmp(path.c_str() + path.size() - el, ext) == 0;
    };
    if (ends_with(L".html") || ends_with(L".htm")) return "text/html; charset=utf-8";
    if (ends_with(L".js"))   return "application/javascript; charset=utf-8";
    if (ends_with(L".css"))  return "text/css; charset=utf-8";
    if (ends_with(L".json")) return "application/json; charset=utf-8";
    if (ends_with(L".png"))  return "image/png";
    if (ends_with(L".jpg") || ends_with(L".jpeg")) return "image/jpeg";
    if (ends_with(L".gif"))  return "image/gif";
    if (ends_with(L".svg"))  return "image/svg+xml";
    if (ends_with(L".ico"))  return "image/x-icon";
    if (ends_with(L".wasm")) return "application/wasm";
    if (ends_with(L".txt"))  return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool valid_url_path(const std::wstring& url_path) {
    // Reject anything containing ".." or null. Allow only [A-Za-z0-9._/-].
    if (url_path.find(L"..") != std::wstring::npos) return false;
    for (wchar_t c : url_path) {
        if (c == L'/' || c == L'-' || c == L'_' || c == L'.') continue;
        if (c >= L'0' && c <= L'9') continue;
        if (c >= L'A' && c <= L'Z') continue;
        if (c >= L'a' && c <= L'z') continue;
        return false;
    }
    return true;
}

void send_404(StreamWriter& out) {
    out.start(404, "Not Found", "text/plain; charset=utf-8");
    const char body[] = "Not Found";
    out.write(body, sizeof(body) - 1);
    out.finish();
}

}  // namespace

std::wstring default_www_dir() {
    std::call_once(g_www_init, [] { g_www_dir = resolve_www_dir(); });
    return g_www_dir;
}

void handle_static(const std::wstring& url_path, StreamWriter& out) {
    if (!valid_url_path(url_path)) { send_404(out); return; }

    std::wstring rel = url_path;
    if (rel.empty() || rel == L"/") rel = L"/index.html";
    // Convert URL slashes to backslashes for the filesystem.
    for (auto& c : rel) if (c == L'/') c = L'\\';

    std::wstring full = default_www_dir() + rel;

    HANDLE h = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) { send_404(out); return; }

    out.start(200, "OK", mime_for(full));
    char buf[16384];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, sizeof(buf), &got, nullptr) || got == 0) break;
        out.write(buf, got);
    }
    CloseHandle(h);
    out.finish();
}

}  // namespace sapisrv
