#include "handlers.h"

#include "core/encoders/encoder.h"
#include "core/synth.h"

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace sapisrv {

namespace {

// URL-decode a wide-char query value into a UTF-8-then-UTF-16 string.
// The query string is ASCII bytes (with %XX escapes for non-ASCII UTF-8 bytes);
// after decoding, we treat the bytes as UTF-8 and widen to UTF-16.
std::wstring url_decode(const wchar_t* s, size_t len) {
    std::string utf8;
    utf8.reserve(len);
    auto hex = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        if (c >= L'a' && c <= L'f') return c - L'a' + 10;
        if (c >= L'A' && c <= L'F') return c - L'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < len; ++i) {
        wchar_t wc = s[i];
        if (wc == L'+') {
            utf8.push_back(' ');
        } else if (wc == L'%' && i + 2 < len) {
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                utf8.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                utf8.push_back(static_cast<char>(wc));
            }
        } else if (wc < 128) {
            utf8.push_back(static_cast<char>(wc));
        }
    }
    int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                     nullptr, 0);
    std::wstring result(needed, L'\0');
    if (needed > 0) {
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                            result.data(), needed);
    }
    return result;
}

std::map<std::wstring, std::wstring> parse_query(const wchar_t* s, size_t len) {
    std::map<std::wstring, std::wstring> out;
    if (!s || len == 0) return out;
    if (s[0] == L'?') { ++s; --len; }
    size_t i = 0;
    while (i < len) {
        size_t key_end = i;
        while (key_end < len && s[key_end] != L'=' && s[key_end] != L'&') ++key_end;
        size_t val_begin = key_end;
        size_t val_end = key_end;
        if (key_end < len && s[key_end] == L'=') {
            val_begin = key_end + 1;
            val_end = val_begin;
            while (val_end < len && s[val_end] != L'&') ++val_end;
        }
        std::wstring key = url_decode(s + i, key_end - i);
        std::wstring val = (val_begin < val_end) ? url_decode(s + val_begin, val_end - val_begin)
                                                  : std::wstring();
        if (!key.empty()) out[std::move(key)] = std::move(val);
        i = val_end + 1;
    }
    return out;
}

const std::wstring* find(const std::map<std::wstring, std::wstring>& m, const wchar_t* k) {
    auto it = m.find(k);
    return it == m.end() ? nullptr : &it->second;
}

int int_or(const std::map<std::wstring, std::wstring>& m, const wchar_t* k, int def) {
    auto* v = find(m, k);
    if (!v || v->empty()) return def;
    return _wtoi(v->c_str());
}

bool bool_or(const std::map<std::wstring, std::wstring>& m, const wchar_t* k, bool def) {
    auto* v = find(m, k);
    if (!v || v->empty()) return def;
    return *v == L"1" || *v == L"true" || *v == L"yes" || *v == L"on";
}

uint64_t parse_events(const std::wstring& v) {
    if (v.empty()) return 0;
    if (v == L"all") return 0xfffe;
    return static_cast<uint64_t>(_wcstoui64(v.c_str(), nullptr, 0));
}

sapicli::SpeakMode parse_mode(const std::wstring& v) {
    if (v == L"ssml") return sapicli::SpeakMode::Ssml;
    if (v == L"sapi") return sapicli::SpeakMode::Sapi;
    if (v == L"auto") return sapicli::SpeakMode::Auto;
    return sapicli::SpeakMode::Text;
}

struct FormatInfo {
    sapicli::Format encoder_format;
    bool is_raw;
    const char* content_type;
};

bool parse_format(const std::wstring& v, FormatInfo& out) {
    if (v == L"raw") { out = { {}, true, "audio/L16" }; return true; }
    if (v == L"ogg" || v == L"ogg+vorbis") {
        out = { sapicli::Format::OggVorbis, false, "audio/ogg" }; return true;
    }
    if (v == L"ogg+opus") {
        out = { sapicli::Format::OggOpus, false, "audio/ogg" }; return true;
    }
    if (v == L"mp3") {
        out = { sapicli::Format::Mp3, false, "audio/mpeg" }; return true;
    }
    return false;
}

void send_error(StreamWriter& out, int status, const char* status_text, const char* msg) {
    std::string body = std::string("{\"error\":\"") + msg + "\"}";
    out.start(status, status_text, "application/json; charset=utf-8");
    out.write(body.data(), body.size());
    out.finish();
}

}  // namespace

void handle_synthesize(const std::wstring& query_string, StreamWriter& out) {
    auto params = parse_query(query_string.c_str(), query_string.size());

    auto* text = find(params, L"text");
    if (!text || text->empty()) {
        send_error(out, 400, "Bad Request", "missing required parameter: text");
        return;
    }

    auto* fmt = find(params, L"format");
    if (!fmt || fmt->empty()) {
        send_error(out, 400, "Bad Request", "missing required parameter: format");
        return;
    }
    FormatInfo finfo;
    if (!parse_format(*fmt, finfo)) {
        send_error(out, 400, "Bad Request", "unknown format (use raw, ogg, ogg+opus, or mp3)");
        return;
    }

    std::wstring voice = find(params, L"voice") ? *find(params, L"voice") : std::wstring();
    int rate = int_or(params, L"rate", 0);
    int volume = int_or(params, L"volume", 100);
    uint32_t sample_rate = static_cast<uint32_t>(int_or(params, L"sample_rate", 22050));
    uint16_t channels = static_cast<uint16_t>(int_or(params, L"channels", 1));
    uint16_t bits = static_cast<uint16_t>(int_or(params, L"bits", 16));
    if (!finfo.is_raw) sample_rate = sapicli::snap_sample_rate(sample_rate, finfo.encoder_format);
    uint64_t events = find(params, L"events") ? parse_events(*find(params, L"events")) : 0;
    bool multiplex = bool_or(params, L"multiplex", events != 0 && !finfo.is_raw);
    sapicli::SpeakMode mode = find(params, L"type") ? parse_mode(*find(params, L"type"))
                                                     : sapicli::SpeakMode::Text;

    try {
        sapicli::Synthesizer synth;
        if (!voice.empty()) synth.set_voice(voice);
        synth.set_rate(rate);
        synth.set_volume(volume);
        synth.set_format({ sample_rate, channels, bits });
        synth.set_event_interest(events);

        out.start(200, "OK", finfo.content_type);

        std::unique_ptr<sapicli::Encoder> encoder;
        if (finfo.is_raw) {
            synth.set_audio_sink([&out](const void* data, std::size_t len) {
                out.write(data, len);
            });
        } else {
            sapicli::EncoderOptions opts{};
            opts.format = finfo.encoder_format;
            opts.audio = { sample_rate, channels, bits };
            opts.multiplex_events = multiplex;
            encoder = sapicli::make_encoder(opts, [&out](const void* data, std::size_t len) {
                out.write(data, len);
            });
            sapicli::Encoder* enc = encoder.get();
            synth.set_audio_sink([enc](const void* data, std::size_t len) {
                enc->write_audio(data, len);
            });
        }

        // Events: if multiplex, route into the encoder; otherwise drop (no fd 3 in the server).
        if (events && multiplex && encoder) {
            sapicli::Encoder* enc = encoder.get();
            synth.set_event_sink([enc](const void* data, std::size_t len) {
                enc->write_event(data, len);
            });
        }

        synth.speak(*text, mode);
        if (encoder) encoder->finish();
        out.finish();
    } catch (const std::exception& e) {
        // We may have already sent headers — best-effort: log + close the stream.
        // The client will see truncated audio if synthesis blew up mid-stream.
        fwprintf(stderr, L"synthesize failed: %hs\n", e.what());
        out.finish();
    }
}

}  // namespace sapisrv
