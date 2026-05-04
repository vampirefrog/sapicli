#include "ogg_opus.h"

#include <ogg/ogg.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
}

#include <cstring>
#include <random>
#include <stdexcept>
#include <vector>

namespace sapicli {

namespace {

constexpr int kOpusInternalRate = 48000;
constexpr int kEventStreamMagicLen = 12;
constexpr unsigned char kEventStreamMagic[kEventStreamMagicLen] = {
    0x80, 's', 'a', 'p', 'i', 'e', 'v', 'e', 'n', 't', 's', 0
};

int random_serialno() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    return static_cast<int>(rng());
}

std::vector<uint8_t> make_opus_tags(const char* vendor) {
    auto vlen = static_cast<uint32_t>(std::strlen(vendor));
    std::vector<uint8_t> out;
    const char magic[8] = { 'O','p','u','s','T','a','g','s' };
    out.insert(out.end(), magic, magic + 8);
    auto put_u32 = [&](uint32_t v) {
        out.push_back(static_cast<uint8_t>(v & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xff));
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xff));
    };
    put_u32(vlen);
    out.insert(out.end(), vendor, vendor + vlen);
    put_u32(0);  // 0 user comments
    return out;
}

}  // namespace

struct OggOpusEncoder::Impl {
    ByteSink sink;
    bool multiplex_events;
    int input_rate;
    int channels;
    int input_bits;

    AVCodecContext* cctx = nullptr;
    SwrContext* swr = nullptr;

    ogg_stream_state audio_os{};
    ogg_stream_state event_os{};
    bool event_os_inited = false;

    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    int frame_size = 0;          // samples per encoder frame at 48kHz
    int64_t samples_in = 0;      // samples consumed at 48kHz
    int64_t pre_skip = 0;
    int64_t packetno_audio = 2;  // 0 = OpusHead, 1 = OpusTags
    int64_t packetno_event = 1;

    std::vector<std::vector<float>> pending;  // planar float at 48kHz

    void emit_pages(ogg_stream_state* os, bool flush) {
        ogg_page page;
        for (;;) {
            int got = flush ? ogg_stream_flush(os, &page)
                            : ogg_stream_pageout(os, &page);
            if (!got) break;
            sink(page.header, page.header_len);
            sink(page.body, page.body_len);
        }
    }

    void submit_packet(ogg_stream_state* os, const void* data, long bytes,
                       int64_t granulepos, bool bos, bool eos, int64_t packetno) {
        ogg_packet op{};
        op.packet = const_cast<unsigned char*>(static_cast<const unsigned char*>(data));
        op.bytes = bytes;
        op.b_o_s = bos ? 1 : 0;
        op.e_o_s = eos ? 1 : 0;
        op.granulepos = granulepos;
        op.packetno = packetno;
        ogg_stream_packetin(os, &op);
    }

    void encode_and_mux(AVFrame* in_frame) {
        int r = avcodec_send_frame(cctx, in_frame);
        if (r < 0 && r != AVERROR_EOF) throw std::runtime_error("opus avcodec_send_frame failed");
        for (;;) {
            r = avcodec_receive_packet(cctx, packet);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) throw std::runtime_error("opus avcodec_receive_packet failed");
            // Opus granulepos is in 48kHz samples and includes pre-skip.
            int64_t granulepos = packet->pts + packet->duration + pre_skip;
            submit_packet(&audio_os, packet->data, packet->size, granulepos,
                          /*bos=*/false, /*eos=*/false, packetno_audio++);
            av_packet_unref(packet);
        }
        emit_pages(&audio_os, /*flush=*/false);
    }
};

OggOpusEncoder::OggOpusEncoder(const EncoderOptions& opts, ByteSink sink)
    : impl_(std::make_unique<Impl>()) {
    impl_->sink = std::move(sink);
    impl_->multiplex_events = opts.multiplex_events;
    impl_->input_rate = static_cast<int>(opts.audio.sample_rate);
    impl_->channels = opts.audio.channels;
    impl_->input_bits = opts.audio.bits_per_sample;

    if (impl_->channels < 1 || impl_->channels > 2) {
        throw std::runtime_error("ogg/opus: only mono or stereo supported");
    }
    if (impl_->input_bits != 8 && impl_->input_bits != 16) {
        throw std::runtime_error("ogg/opus: only 8 or 16 bit input PCM supported");
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("libopus");
    if (!codec) throw std::runtime_error("libopus encoder not available");

    impl_->cctx = avcodec_alloc_context3(codec);
    if (!impl_->cctx) throw std::runtime_error("avcodec_alloc_context3 failed");
    impl_->cctx->sample_rate = kOpusInternalRate;  // opus always 48k
    impl_->cctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&impl_->cctx->ch_layout, impl_->channels);
    impl_->cctx->bit_rate = (impl_->channels == 2) ? 96000 : 64000;

    if (avcodec_open2(impl_->cctx, codec, nullptr) < 0) {
        throw std::runtime_error("avcodec_open2 (libopus) failed");
    }
    impl_->frame_size = impl_->cctx->frame_size;
    if (impl_->frame_size <= 0) impl_->frame_size = 960;  // 20ms at 48kHz
    impl_->pre_skip = impl_->cctx->initial_padding;

    // Swresample: input PCM (s16/u8 interleaved at input_rate) → fltp at 48kHz.
    AVChannelLayout in_layout;
    av_channel_layout_default(&in_layout, impl_->channels);
    AVSampleFormat in_fmt = (impl_->input_bits == 16) ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_U8;
    if (swr_alloc_set_opts2(&impl_->swr,
                            &impl_->cctx->ch_layout, AV_SAMPLE_FMT_FLTP, kOpusInternalRate,
                            &in_layout,             in_fmt,              impl_->input_rate,
                            0, nullptr) < 0) {
        throw std::runtime_error("swr_alloc_set_opts2 failed");
    }
    if (swr_init(impl_->swr) < 0) throw std::runtime_error("swr_init failed");

    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    if (!impl_->frame || !impl_->packet) throw std::runtime_error("frame/packet alloc failed");
    impl_->frame->format = AV_SAMPLE_FMT_FLTP;
    impl_->frame->sample_rate = kOpusInternalRate;
    av_channel_layout_copy(&impl_->frame->ch_layout, &impl_->cctx->ch_layout);
    impl_->frame->nb_samples = impl_->frame_size;
    if (av_frame_get_buffer(impl_->frame, 0) < 0) throw std::runtime_error("av_frame_get_buffer failed");

    impl_->pending.assign(impl_->channels, {});

    // Init Ogg streams.
    if (ogg_stream_init(&impl_->audio_os, random_serialno()) != 0)
        throw std::runtime_error("ogg_stream_init (audio) failed");
    if (impl_->multiplex_events) {
        if (ogg_stream_init(&impl_->event_os, random_serialno()) != 0)
            throw std::runtime_error("ogg_stream_init (event) failed");
        impl_->event_os_inited = true;
    }

    // OpusHead is in extradata as a single packet (not Xiph-formatted).
    if (impl_->cctx->extradata_size <= 0)
        throw std::runtime_error("opus: no OpusHead in extradata");
    impl_->submit_packet(&impl_->audio_os,
                         impl_->cctx->extradata, impl_->cctx->extradata_size,
                         0, /*bos=*/true, /*eos=*/false, 0);
    impl_->emit_pages(&impl_->audio_os, /*flush=*/true);

    if (impl_->multiplex_events) {
        impl_->submit_packet(&impl_->event_os, kEventStreamMagic, kEventStreamMagicLen,
                             0, /*bos=*/true, /*eos=*/false, 0);
        impl_->emit_pages(&impl_->event_os, /*flush=*/true);
    }

    // OpusTags packet (we build it).
    auto tags = make_opus_tags("sapicli");
    impl_->submit_packet(&impl_->audio_os, tags.data(), static_cast<long>(tags.size()),
                         0, false, false, 1);
    impl_->emit_pages(&impl_->audio_os, /*flush=*/true);
}

OggOpusEncoder::~OggOpusEncoder() {
    if (impl_) {
        if (impl_->frame) av_frame_free(&impl_->frame);
        if (impl_->packet) av_packet_free(&impl_->packet);
        if (impl_->swr) swr_free(&impl_->swr);
        if (impl_->cctx) avcodec_free_context(&impl_->cctx);
        ogg_stream_clear(&impl_->audio_os);
        if (impl_->event_os_inited) ogg_stream_clear(&impl_->event_os);
    }
}

void OggOpusEncoder::write_audio(const void* pcm, std::size_t bytes) {
    if (bytes == 0) return;
    int bytes_per_sample = impl_->input_bits / 8;
    int in_samples = static_cast<int>(bytes / (bytes_per_sample * impl_->channels));
    if (in_samples == 0) return;

    // Estimate output sample count after rate conversion (input → 48kHz).
    int out_estimate = static_cast<int>(swr_get_out_samples(impl_->swr, in_samples));
    if (out_estimate < 0) out_estimate = in_samples;

    AVFrame* tmp = av_frame_alloc();
    tmp->format = AV_SAMPLE_FMT_FLTP;
    tmp->sample_rate = kOpusInternalRate;
    av_channel_layout_copy(&tmp->ch_layout, &impl_->cctx->ch_layout);
    tmp->nb_samples = out_estimate > 0 ? out_estimate : in_samples;
    if (av_frame_get_buffer(tmp, 0) < 0) {
        av_frame_free(&tmp);
        throw std::runtime_error("scratch frame alloc failed");
    }
    const uint8_t* in_planes[1] = { static_cast<const uint8_t*>(pcm) };
    int converted = swr_convert(impl_->swr, tmp->data, tmp->nb_samples, in_planes, in_samples);
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
        impl_->encode_and_mux(impl_->frame);
    }
}

void OggOpusEncoder::write_event(const void* serialized_event, std::size_t bytes) {
    if (!impl_->multiplex_events || bytes == 0) return;
    int64_t granulepos = impl_->samples_in + impl_->pre_skip;
    impl_->submit_packet(&impl_->event_os, serialized_event, static_cast<long>(bytes),
                         granulepos, /*bos=*/false, /*eos=*/false,
                         impl_->packetno_event++);
    // Flush so this event ends its own page; otherwise multiple events share
    // a page and only the last one's granulepos is observable.
    impl_->emit_pages(&impl_->event_os, /*flush=*/true);
}

void OggOpusEncoder::finish() {
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
        impl_->encode_and_mux(impl_->frame);
    }

    impl_->encode_and_mux(nullptr);

    int64_t final_gp = impl_->samples_in + impl_->pre_skip;
    impl_->submit_packet(&impl_->audio_os, "", 0, final_gp,
                         /*bos=*/false, /*eos=*/true, impl_->packetno_audio++);
    impl_->emit_pages(&impl_->audio_os, /*flush=*/true);

    if (impl_->multiplex_events) {
        impl_->submit_packet(&impl_->event_os, "", 0, final_gp,
                             /*bos=*/false, /*eos=*/true, impl_->packetno_event++);
        impl_->emit_pages(&impl_->event_os, /*flush=*/true);
    }
}

}  // namespace sapicli
