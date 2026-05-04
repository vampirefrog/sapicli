#pragma once

#include "encoder.h"

namespace sapicli {

class OggOpusEncoder : public Encoder {
public:
    OggOpusEncoder(const EncoderOptions& opts, ByteSink sink);
    ~OggOpusEncoder() override;

    void write_audio(const void* pcm, std::size_t bytes) override;
    void write_event(const void* serialized_event, std::size_t bytes) override;
    void finish() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sapicli
