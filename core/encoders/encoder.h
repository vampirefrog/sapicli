#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace sapicli {

using ByteSink = std::function<void(const void* data, std::size_t len)>;

struct AudioFormat {
    std::uint32_t sample_rate;
    std::uint16_t channels;
    std::uint16_t bits_per_sample;
};

// Sink for one synthesis run. SAPI emits PCM via write_audio() and
// SPSERIALIZEDEVENT blobs via write_event(). finish() flushes and
// finalizes the container. Destruction without finish() is undefined.
//
// Encoders that cannot stream events (e.g. mp3 with ID3 PRIV trailer)
// buffer events internally and emit them during finish().
class Encoder {
public:
    virtual ~Encoder() = default;
    virtual void write_audio(const void* pcm, std::size_t bytes) = 0;
    virtual void write_event(const void* serialized_event, std::size_t bytes) = 0;
    virtual void finish() = 0;
};

enum class Format {
    OggVorbis,
    OggOpus,
    Mp3,
};

struct EncoderOptions {
    Format format;
    AudioFormat audio;
    bool multiplex_events;  // embed events in the container where supported
};

std::unique_ptr<Encoder> make_encoder(const EncoderOptions& opts, ByteSink sink);

// Snap the requested sample rate to one the encoder can actually accept.
// libopus only accepts 8/12/16/24/48 kHz; libvorbis and lame are flexible.
// The caller MUST configure SAPI to emit at the returned rate so the encoder
// and the synthesizer agree.
std::uint32_t snap_sample_rate(std::uint32_t requested, Format fmt);

}  // namespace sapicli
