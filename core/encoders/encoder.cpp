#include "encoder.h"

#include "ogg_vorbis.h"
#include "ogg_opus.h"
#include "mp3_id3.h"

namespace sapicli {

std::unique_ptr<Encoder> make_encoder(const EncoderOptions& opts, ByteSink sink) {
    switch (opts.format) {
        case Format::OggVorbis:
            return std::make_unique<OggVorbisEncoder>(opts, std::move(sink));
        case Format::OggOpus:
            return std::make_unique<OggOpusEncoder>(opts, std::move(sink));
        case Format::Mp3:
            return std::make_unique<Mp3Id3Encoder>(opts, std::move(sink));
    }
    return nullptr;
}

}  // namespace sapicli
