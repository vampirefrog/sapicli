#include "mp3_id3.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

#include <cstring>
#include <stdexcept>
#include <vector>

namespace sapicli {

namespace {

constexpr const char* kPrivOwner = "sapi.EVNT";

void put_synchsafe(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 21) & 0x7f));
    out.push_back(static_cast<uint8_t>((v >> 14) & 0x7f));
    out.push_back(static_cast<uint8_t>((v >>  7) & 0x7f));
    out.push_back(static_cast<uint8_t>( v        & 0x7f));
}

// Build an ID3v2.4 tag containing a single PRIV frame with the event blob.
// Returns the full tag bytes (10-byte header + frame).
std::vector<uint8_t> build_id3v2_priv(const std::vector<uint8_t>& event_blob) {
    std::vector<uint8_t> frame;
    // Frame body: owner\0 + binary
    auto owner_len = std::strlen(kPrivOwner);
    frame.insert(frame.end(), kPrivOwner, kPrivOwner + owner_len);
    frame.push_back(0);
    frame.insert(frame.end(), event_blob.begin(), event_blob.end());

    std::vector<uint8_t> out;
    // Tag header
    out.insert(out.end(), { 'I', 'D', '3' });
    out.push_back(0x04);  // version major
    out.push_back(0x00);  // version minor
    out.push_back(0x00);  // flags
    // Tag size (excluding header) = frame header (10) + frame body
    put_synchsafe(out, static_cast<uint32_t>(10 + frame.size()));
    // Frame header
    out.insert(out.end(), { 'P', 'R', 'I', 'V' });
    put_synchsafe(out, static_cast<uint32_t>(frame.size()));
    out.push_back(0x00);  // flags hi
    out.push_back(0x00);  // flags lo
    // Frame body
    out.insert(out.end(), frame.begin(), frame.end());
    return out;
}

}  // namespace

struct Mp3Id3Encoder::Impl {
    ByteSink sink;
    int sample_rate;
    int channels;
    int input_bits;

    AVCodecContext* cctx = nullptr;
    SwrContext* swr = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int frame_size = 0;
    int64_t samples_in = 0;

    std::vector<std::vector<float>> pending;
    std::vector<uint8_t> mp3_buf;
    std::vector<uint8_t> event_blob;

    void encode_and_buffer(AVFrame* in_frame) {
        int r = avcodec_send_frame(cctx, in_frame);
        if (r < 0 && r != AVERROR_EOF) throw std::runtime_error("mp3 avcodec_send_frame failed");
        for (;;) {
            r = avcodec_receive_packet(cctx, packet);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) throw std::runtime_error("mp3 avcodec_receive_packet failed");
            mp3_buf.insert(mp3_buf.end(), packet->data, packet->data + packet->size);
            av_packet_unref(packet);
        }
    }
};

Mp3Id3Encoder::Mp3Id3Encoder(const EncoderOptions& opts, ByteSink sink)
    : impl_(std::make_unique<Impl>()) {
    impl_->sink = std::move(sink);
    impl_->sample_rate = static_cast<int>(opts.audio.sample_rate);
    impl_->channels = opts.audio.channels;
    impl_->input_bits = opts.audio.bits_per_sample;

    if (impl_->channels < 1 || impl_->channels > 2) {
        throw std::runtime_error("mp3: only mono or stereo supported");
    }
    if (impl_->input_bits != 8 && impl_->input_bits != 16) {
        throw std::runtime_error("mp3: only 8 or 16 bit input PCM supported");
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("libmp3lame");
    if (!codec) throw std::runtime_error("libmp3lame encoder not available");

    impl_->cctx = avcodec_alloc_context3(codec);
    if (!impl_->cctx) throw std::runtime_error("avcodec_alloc_context3 failed");
    impl_->cctx->sample_rate = impl_->sample_rate;
    impl_->cctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&impl_->cctx->ch_layout, impl_->channels);
    impl_->cctx->bit_rate = 128000;

    if (avcodec_open2(impl_->cctx, codec, nullptr) < 0) {
        throw std::runtime_error("avcodec_open2 (libmp3lame) failed");
    }
    impl_->frame_size = impl_->cctx->frame_size;
    if (impl_->frame_size <= 0) impl_->frame_size = 1152;

    AVChannelLayout in_layout;
    av_channel_layout_default(&in_layout, impl_->channels);
    AVSampleFormat in_fmt = (impl_->input_bits == 16) ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_U8;
    if (swr_alloc_set_opts2(&impl_->swr,
                            &impl_->cctx->ch_layout, AV_SAMPLE_FMT_FLTP, impl_->sample_rate,
                            &in_layout,             in_fmt,              impl_->sample_rate,
                            0, nullptr) < 0) {
        throw std::runtime_error("swr_alloc_set_opts2 failed");
    }
    if (swr_init(impl_->swr) < 0) throw std::runtime_error("swr_init failed");

    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    if (!impl_->frame || !impl_->packet) throw std::runtime_error("frame/packet alloc failed");
    impl_->frame->format = AV_SAMPLE_FMT_FLTP;
    impl_->frame->sample_rate = impl_->sample_rate;
    av_channel_layout_copy(&impl_->frame->ch_layout, &impl_->cctx->ch_layout);
    impl_->frame->nb_samples = impl_->frame_size;
    if (av_frame_get_buffer(impl_->frame, 0) < 0) throw std::runtime_error("av_frame_get_buffer failed");

    impl_->pending.assign(impl_->channels, {});
}

Mp3Id3Encoder::~Mp3Id3Encoder() {
    if (impl_) {
        if (impl_->frame) av_frame_free(&impl_->frame);
        if (impl_->packet) av_packet_free(&impl_->packet);
        if (impl_->swr) swr_free(&impl_->swr);
        if (impl_->cctx) avcodec_free_context(&impl_->cctx);
    }
}

void Mp3Id3Encoder::write_audio(const void* pcm, std::size_t bytes) {
    if (bytes == 0) return;
    int bytes_per_sample = impl_->input_bits / 8;
    int in_samples = static_cast<int>(bytes / (bytes_per_sample * impl_->channels));
    if (in_samples == 0) return;

    AVFrame* tmp = av_frame_alloc();
    tmp->format = AV_SAMPLE_FMT_FLTP;
    tmp->sample_rate = impl_->sample_rate;
    av_channel_layout_copy(&tmp->ch_layout, &impl_->cctx->ch_layout);
    tmp->nb_samples = in_samples;
    if (av_frame_get_buffer(tmp, 0) < 0) {
        av_frame_free(&tmp);
        throw std::runtime_error("scratch frame alloc failed");
    }
    const uint8_t* in_planes[1] = { static_cast<const uint8_t*>(pcm) };
    int converted = swr_convert(impl_->swr, tmp->data, in_samples, in_planes, in_samples);
    if (converted < 0) {
        av_frame_free(&tmp);
        throw std::runtime_error("swr_convert failed");
    }
    for (int c = 0; c < impl_->channels; ++c) {
        const float* src = reinterpret_cast<const float*>(tmp->data[c]);
        impl_->pending[c].insert(impl_->pending[c].end(), src, src + converted);
    }
    av_frame_free(&tmp);

    while (static_cast<int>(impl_->pending[0].size()) >= impl_->frame_size) {
        if (av_frame_make_writable(impl_->frame) < 0)
            throw std::runtime_error("frame not writable");
        impl_->frame->nb_samples = impl_->frame_size;
        impl_->frame->pts = impl_->samples_in;
        for (int c = 0; c < impl_->channels; ++c) {
            std::memcpy(impl_->frame->data[c], impl_->pending[c].data(),
                        impl_->frame_size * sizeof(float));
            impl_->pending[c].erase(impl_->pending[c].begin(),
                                    impl_->pending[c].begin() + impl_->frame_size);
        }
        impl_->samples_in += impl_->frame_size;
        impl_->encode_and_buffer(impl_->frame);
    }
}

void Mp3Id3Encoder::write_event(const void* serialized_event, std::size_t bytes) {
    if (bytes == 0) return;
    auto* p = static_cast<const uint8_t*>(serialized_event);
    impl_->event_blob.insert(impl_->event_blob.end(), p, p + bytes);
}

void Mp3Id3Encoder::finish() {
    // Encode any pending tail.
    if (!impl_->pending.empty() && !impl_->pending[0].empty()) {
        int tail = static_cast<int>(impl_->pending[0].size());
        if (av_frame_make_writable(impl_->frame) < 0)
            throw std::runtime_error("frame not writable");
        impl_->frame->nb_samples = tail;
        impl_->frame->pts = impl_->samples_in;
        for (int c = 0; c < impl_->channels; ++c) {
            std::memcpy(impl_->frame->data[c], impl_->pending[c].data(), tail * sizeof(float));
            impl_->pending[c].clear();
        }
        impl_->samples_in += tail;
        impl_->encode_and_buffer(impl_->frame);
    }
    impl_->encode_and_buffer(nullptr);

    // Write [ID3v2 with PRIV(events)] + [mp3 audio]. Events as a lump.
    if (!impl_->event_blob.empty()) {
        auto tag = build_id3v2_priv(impl_->event_blob);
        impl_->sink(tag.data(), tag.size());
    }
    impl_->sink(impl_->mp3_buf.data(), impl_->mp3_buf.size());
}

}  // namespace sapicli
