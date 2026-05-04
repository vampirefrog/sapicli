#include "encoder.h"

extern "C" {
#include <mux.h>
}

#include <cstdint>
#include <stdexcept>
#include <string>

namespace sapicli {

namespace {

mux_codec_type to_mux_codec(Format f) {
    switch (f) {
        case Format::OggVorbis: return MUX_CODEC_VORBIS;
        case Format::OggOpus:   return MUX_CODEC_OPUS;
        case Format::Mp3:       return MUX_CODEC_MP3;
    }
    throw std::runtime_error("encoder: unknown Format");
}

class MuxAudioEncoder : public Encoder {
public:
    MuxAudioEncoder(const EncoderOptions& opts, ByteSink sink)
        : sink_(std::move(sink)) {
        // num_streams=2 enables the side-channel for events; per the muxaudio
        // support table this routes vorbis/opus through ogg-with-parallel-stream
        // and mp3/pcm through the leb128 mux protocol. num_streams=1 emits
        // plain audio (regular .ogg / .mp3) with no event channel.
        int num_streams = opts.multiplex_events ? 2 : 1;
        enc_ = mux_encoder_new(to_mux_codec(opts.format),
                               static_cast<int>(opts.audio.sample_rate),
                               opts.audio.channels,
                               num_streams,
                               nullptr, 0);
        if (!enc_) throw std::runtime_error("mux_encoder_new failed");
    }

    ~MuxAudioEncoder() override {
        if (enc_) mux_encoder_destroy(enc_);
    }

    void write_audio(const void* pcm, std::size_t bytes) override {
        push(pcm, bytes, MUX_STREAM_AUDIO);
    }

    void write_event(const void* data, std::size_t bytes) override {
        push(data, bytes, MUX_STREAM_SIDE_CHANNEL);
    }

    void finish() override {
        int r = mux_encoder_finalize(enc_);
        if (r < 0) throw_err("mux_encoder_finalize", r);
        drain();
    }

private:
    void push(const void* data, std::size_t bytes, int stream_type) {
        if (!data || bytes == 0) return;
        const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
        std::size_t remaining = bytes;
        while (remaining > 0) {
            std::size_t consumed = 0;
            int r = mux_encoder_encode(enc_, p, remaining, &consumed, stream_type);
            if (r == MUX_ERROR_AGAIN) {
                // Output buffer full — drain and retry.
                drain();
                continue;
            }
            if (r < 0) throw_err("mux_encoder_encode", r);
            drain();
            if (consumed == 0) break;  // avoid infinite loop on stalled codec
            p += consumed;
            remaining -= consumed;
        }
    }

    void drain() {
        char buf[8192];
        for (;;) {
            std::size_t got = 0;
            int r = mux_encoder_read(enc_, buf, sizeof(buf), &got);
            // AGAIN = no output ready (need more input). EOF = stream ended.
            // Both are normal terminations of the drain loop.
            if (r == MUX_ERROR_AGAIN || r == MUX_ERROR_EOF) {
                if (got > 0) sink_(buf, got);
                break;
            }
            if (r < 0) throw_err("mux_encoder_read", r);
            if (got == 0) break;
            sink_(buf, got);
        }
    }

    [[noreturn]] void throw_err(const char* what, int code) {
        const auto* info = mux_encoder_get_error(enc_);
        std::string msg = std::string(what) + " failed (code=" + std::to_string(code);
        if (info) {
            if (info->message)     { msg += ": "; msg += info->message; }
            if (info->library_msg) { msg += " ["; msg += info->library_name ? info->library_name : "lib";
                                     msg += ": "; msg += info->library_msg; msg += "]"; }
        }
        msg += ")";
        throw std::runtime_error(msg);
    }

    mux_encoder* enc_ = nullptr;
    ByteSink sink_;
};

}  // namespace

std::unique_ptr<Encoder> make_encoder(const EncoderOptions& opts, ByteSink sink) {
    return std::make_unique<MuxAudioEncoder>(opts, std::move(sink));
}

std::uint32_t snap_sample_rate(std::uint32_t requested, Format fmt) {
    if (fmt == Format::OggOpus) {
        constexpr std::uint32_t valid[] = { 8000, 12000, 16000, 24000, 48000 };
        for (auto v : valid) if (requested <= v) return v;
        return 48000;
    }
    return requested;
}

}  // namespace sapicli
