#pragma once

#include <cstddef>
#include <string>

namespace sapisrv {

// Output sink for response handlers. Implementations buffer or stream
// to the underlying transport (HTTP API v2 in production).
class StreamWriter {
public:
    virtual ~StreamWriter() = default;

    // Begin response with status + content-type. Must be called once before write().
    virtual void start(int status, const char* status_text, const char* content_type) = 0;

    // Append a chunk of body bytes. Streams to the client as soon as possible.
    virtual void write(const void* data, std::size_t len) = 0;

    // Close response. After this no further start/write/finish.
    virtual void finish() = 0;
};

// GET /voices  → JSON array of available SAPI voices (buffered, single chunk).
void handle_voices(StreamWriter& out);

// GET /codecs  → JSON array of muxaudio codecs compiled into this build,
// each with its supported sample rates and encoder parameters (introspected
// via mux_list_codecs / mux_get_supported_sample_rates / mux_get_encoder_params).
void handle_codecs(StreamWriter& out);

// GET /health  → 200 with JSON {"status":"ok","pid":N,"uptime_s":N}.
// Cheap and unauthenticated — meant for liveness probes / load balancers.
void handle_health(StreamWriter& out);

// GET /api/default-key → JSON {"api_key":"<key>"} or {"api_key":""}.
// Unauthenticated; used by the bundled web client to pick up whatever
// public-trial key the installer wrote into keys.json.
void handle_default_key(StreamWriter& out);

// GET /synthesize?text=...&voice=...&format=...&...
// Streams encoded audio (and optional events for ogg) as bytes are produced.
void handle_synthesize(const std::wstring& query_string, StreamWriter& out);

}  // namespace sapisrv
