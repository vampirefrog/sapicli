#include "handlers.h"

#include "core/voices.h"

#include <stdexcept>
#include <string>

namespace sapisrv {

namespace {

// Convert UTF-16 to UTF-8 with JSON escaping for ", \, and control chars.
void append_json_string_utf8(std::string& out, const std::wstring& s) {
    out.push_back('"');
    for (wchar_t wc : s) {
        if (wc == L'"' || wc == L'\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(wc));
        } else if (wc == L'\n') { out.append("\\n"); }
        else if (wc == L'\r') { out.append("\\r"); }
        else if (wc == L'\t') { out.append("\\t"); }
        else if (wc < 0x20) {
            char buf[8];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x", wc);
            out.append(buf);
        } else if (wc < 0x80) {
            out.push_back(static_cast<char>(wc));
        } else if (wc < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (wc >> 6)));
            out.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else {
            // Surrogate pairs handled as separate code units; sufficient for SAPI strings.
            out.push_back(static_cast<char>(0xE0 | (wc >> 12)));
            out.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        }
    }
    out.push_back('"');
}

void append_pair(std::string& out, const char* key, const std::wstring& val, bool last = false) {
    out.push_back('"');
    out.append(key);
    out.append("\":");
    append_json_string_utf8(out, val);
    if (!last) out.push_back(',');
}

}  // namespace

Response handle_voices() {
    Response r;
    try {
        auto voices = sapicli::enumerate_voices();
        std::string& body = r.body;
        body.push_back('[');
        for (size_t i = 0; i < voices.size(); ++i) {
            if (i) body.push_back(',');
            body.push_back('{');
            append_pair(body, "id", voices[i].id);
            append_pair(body, "description", voices[i].description);
            append_pair(body, "age", voices[i].age);
            append_pair(body, "gender", voices[i].gender);
            append_pair(body, "language", voices[i].language);
            append_pair(body, "name", voices[i].name);
            append_pair(body, "vendor", voices[i].vendor, /*last=*/true);
            body.push_back('}');
        }
        body.push_back(']');
    } catch (const std::exception& e) {
        r.status = 500;
        r.status_text = "Internal Server Error";
        r.body = std::string("{\"error\":\"") + e.what() + "\"}";
    }
    return r;
}

}  // namespace sapisrv
