#include "handlers.h"
#include "auth.h"

#include "core/encoders/encoder.h"
#include "core/voices.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
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

// JSON-escape a UTF-8 string (no re-decoding — muxaudio strings are ASCII).
void append_json_string(std::string& out, const char* s) {
    out.push_back('"');
    if (s) {
        for (const char* p = s; *p; ++p) {
            unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(*p); }
            else if (c == '\n') out.append("\\n");
            else if (c == '\r') out.append("\\r");
            else if (c == '\t') out.append("\\t");
            else if (c < 0x20) {
                char buf[8];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "\\u%04x", c);
                out.append(buf);
            } else {
                out.push_back(*p);
            }
        }
    }
    out.push_back('"');
}

}  // namespace

void handle_voices(StreamWriter& out) {
    try {
        auto voices = sapicli::enumerate_voices();
        std::string body;
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
        out.start(200, "OK", "application/json; charset=utf-8");
        out.write(body.data(), body.size());
        out.finish();
    } catch (const std::exception& e) {
        std::string body = std::string("{\"error\":\"") + e.what() + "\"}";
        out.start(500, "Internal Server Error", "application/json; charset=utf-8");
        out.write(body.data(), body.size());
        out.finish();
    }
}

void handle_health(StreamWriter& out) {
    using clock = std::chrono::steady_clock;
    static const auto start = clock::now();
    long long up_s = std::chrono::duration_cast<std::chrono::seconds>(clock::now() - start).count();
    char body[128];
    int n = _snprintf_s(body, sizeof(body), _TRUNCATE,
                        "{\"status\":\"ok\",\"pid\":%lu,\"uptime_s\":%lld}",
                        GetCurrentProcessId(), up_s);
    out.start(200, "OK", "application/json; charset=utf-8");
    if (n > 0) out.write(body, n);
    out.finish();
}

void handle_codecs(StreamWriter& out) {
    std::string body;
    body.push_back('[');
    auto codecs = sapicli::list_encoder_codecs();
    for (size_t i = 0; i < codecs.size(); ++i) {
        if (i) body.push_back(',');
        body.push_back('{');
        body.append("\"name\":");
        append_json_string(body, codecs[i].name.c_str());
        body.append(",\"description\":");
        append_json_string(body, codecs[i].description.c_str());

        // Sample rates: { is_range: bool, values: [n, n, ...] }
        mux_sample_rate_list rates{};
        body.append(",\"sample_rates\":{");
        if (mux_get_supported_sample_rates(codecs[i].type, &rates) == MUX_OK) {
            body.append("\"is_range\":");
            body.append(rates.is_range ? "true" : "false");
            body.append(",\"values\":[");
            for (int j = 0; j < rates.count; ++j) {
                if (j) body.push_back(',');
                char buf[16];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d", rates.rates[j]);
                body.append(buf);
            }
            body.push_back(']');
        } else {
            body.append("\"is_range\":false,\"values\":[]");
        }
        body.push_back('}');

        // Encoder params: array of { name, description, type, min, max, default }.
        const mux_param_desc* pd = nullptr;
        int pd_count = 0;
        body.append(",\"params\":[");
        if (mux_get_encoder_params(codecs[i].type, &pd, &pd_count) == MUX_OK && pd) {
            for (int j = 0; j < pd_count; ++j) {
                if (j) body.push_back(',');
                body.push_back('{');
                body.append("\"name\":");        append_json_string(body, pd[j].name);
                body.append(",\"description\":"); append_json_string(body, pd[j].description);
                char nbuf[64];
                switch (pd[j].type) {
                    case MUX_PARAM_TYPE_INT:
                        body.append(",\"type\":\"int\"");
                        _snprintf_s(nbuf, sizeof(nbuf), _TRUNCATE,
                                    ",\"min\":%d,\"max\":%d,\"default\":%d",
                                    pd[j].range.i.min, pd[j].range.i.max, pd[j].range.i.def);
                        body.append(nbuf);
                        break;
                    case MUX_PARAM_TYPE_FLOAT:
                        body.append(",\"type\":\"float\"");
                        _snprintf_s(nbuf, sizeof(nbuf), _TRUNCATE,
                                    ",\"min\":%g,\"max\":%g,\"default\":%g",
                                    pd[j].range.f.min, pd[j].range.f.max, pd[j].range.f.def);
                        body.append(nbuf);
                        break;
                    case MUX_PARAM_TYPE_BOOL:
                        body.append(",\"type\":\"bool\",\"default\":");
                        body.append(pd[j].range.b.def ? "true" : "false");
                        break;
                    case MUX_PARAM_TYPE_STRING:
                        body.append(",\"type\":\"string\",\"default\":");
                        append_json_string(body, pd[j].range.s.def);
                        break;
                }
                body.push_back('}');
            }
        }
        body.push_back(']');
        body.push_back('}');
    }
    body.push_back(']');
    out.start(200, "OK", "application/json; charset=utf-8");
    out.write(body.data(), body.size());
    out.finish();
}

void handle_default_key(StreamWriter& out) {
    // Returns the api key marked "default": true in keys.json so the
    // bundled web client can self-configure. Empty string if no default
    // is set, in which case the client falls back to unauthenticated
    // (public_tier) mode.
    std::string key = default_api_key();
    std::string body = "{\"api_key\":\"" + key + "\"}";
    out.start(200, "OK", "application/json; charset=utf-8");
    out.write(body.data(), body.size());
    out.finish();
}

}  // namespace sapisrv
