#include "ogg_vorbis.h"

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

constexpr int kEventStreamMagicLen = 12;
constexpr unsigned char kEventStreamMagic[kEventStreamMagicLen] = {
    0x80, 's', 'a', 'p', 'i', 'e', 'v', 'e', 'n', 't', 's', 0
};

int random_serialno() {
    static thread_local std::mt19937 rng{std::random_device{}()};
    return static_cast<int>(rng());
}

// Decode the Xiph/extradata format used by libavcodec for codecs with
// multiple header packets (vorbis: 3, opus: 2). Returns the packets in
// order; throws on malformed input.
std::vector<std::vector<uint8_t>> split_xiph_headers(const uint8_t* data, int size) {
    if (size < 1) throw std::runtime_error("vorbis extradata too short");
    int n = data[0] + 1;
    int p = 1;
    std::vector<int> sizes(n);
    for (int i = 0; i < n - 1; ++i) {
        int s = 0;
        while (p < size && data[p] == 0xFF) { s += 0xFF; ++p; }
        if (p >= size) throw std::runtime_error("vorbis extradata truncated");
        s += data[p++];
        sizes[i] = s;
    }
    int rest = size - p;
    for (int i = 0; i < n - 1; ++i) rest -= sizes[i];
    if (rest < 0) throw std::runtime_error("vorbis extradata sizes overflow");
    sizes[n - 1] = rest;

    std::vector<std::vector<uint8_t>> out(n);
    for (int i = 0; i < n; ++i) {
        out[i].assign(data + p, data + p + sizes[i]);
        p += sizes[i];
    }
    return out;
}

}  // namespace

struct OggVorbisEncoder::Impl {
    ByteSink sink;
    bool multiplex_events;
    int sample_rate;
    int channels;
    int input_bits;

    AVCodecContext* cctx = nullptr;
    SwrContext* swr = nullptr;

    ogg_stream_state audio_os{};
    ogg_stream_state event_os{};
    bool event_os_inited = false;

    AVFrame* frame = nullptr;     // reusable input frame, frame_size samples
    AVPacket* packet = nullptr;
    int frame_size = 0;
    int64_t samples_in = 0;       // total input samples consumed by encoder
    int64_t packetno_audio = 2;   // 0..2 are vorbis headers; data starts at 3
    int64_t packetno_event = 1;   // 0 is event stream BOS magic

    // Pending PCM samples (planar float, per channel)
    std::vector<std::vector<float>> pending;

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
        if (r < 0 && r != AVERROR_EOF) throw std::runtime_error("avcodec_send_frame failed");
        for (;;) {
            r = avcodec_receive_packet(cctx, packet);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) break;
            if (r < 0) throw std::runtime_error("avcodec_receive_packet failed");
            // libavcodec sets pts/duration in encoder-timebase units (1/sample_rate).
            // Vorbis granulepos = sample count of LAST sample in packet.
            int64_t granulepos = packet->pts + packet->duration;
            bool eos = (in_frame == nullptr) && (cctx->internal == nullptr);  // best-effort; finish() also flushes
            (void)eos;
            submit_packet(&audio_os, packet->data, packet->size, granulepos,
                          /*bos=*/false, /*eos=*/false, packetno_audio++);
            av_packet_unref(packet);
        }
        emit_pages(&audio_os, /*flush=*/false);
    }
};

OggVorbisEncoder::OggVorbisEncoder(const EncoderOptions& opts, ByteSink sink)
    : impl_(std::make_unique<Impl>()) {
    impl_->sink = std::move(sink);
    impl_->multiplex_events = opts.multiplex_events;
    impl_->sample_rate = static_cast<int>(opts.audio.sample_rate);
    impl_->channels = opts.audio.channels;
    impl_->input_bits = opts.audio.bits_per_sample;

    if (impl_->channels < 1 || impl_->channels > 2) {
        throw std::runtime_error("ogg/vorbis: only mono or stereo supported");
    }
    if (impl_->input_bits != 8 && impl_->input_bits != 16) {
        throw std::runtime_error("ogg/vorbis: only 8 or 16 bit input PCM supported");
    }

    const AVCodec* codec = avcodec_find_encoder_by_name("libvorbis");
    if (!codec) throw std::runtime_error("libvorbis encoder not available");

    impl_->cctx = avcodec_alloc_context3(codec);
    if (!impl_->cctx) throw std::runtime_error("avcodec_alloc_context3 failed");
    impl_->cctx->sample_rate = impl_->sample_rate;
    impl_->cctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&impl_->cctx->ch_layout, impl_->channels);
    // VBR quality target ~4 (range -1..10), about 128 kbps for stereo 44.1kHz.
    av_opt_set_double(impl_->cctx->priv_data, "quality", 4.0, 0);

    if (avcodec_open2(impl_->cctx, codec, nullptr) < 0) {
        throw std::runtime_error("avcodec_open2 (libvorbis) failed");
    }
    impl_->frame_size = impl_->cctx->frame_size;
    if (impl_->frame_size <= 0) impl_->frame_size = 1024;

    // Swresample: input PCM (s16 or u8 interleaved) → fltp at same rate.
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

    // Init Ogg streams.
    if (ogg_stream_init(&impl_->audio_os, random_serialno()) != 0)
        throw std::runtime_error("ogg_stream_init (audio) failed");
    if (impl_->multiplex_events) {
        if (ogg_stream_init(&impl_->event_os, random_serialno()) != 0)
            throw std::runtime_error("ogg_stream_init (event) failed");
        impl_->event_os_inited = true;
    }

    // Vorbis 3 headers from Xiph extradata.
    auto headers = split_xiph_headers(impl_->cctx->extradata, impl_->cctx->extradata_size);
    if (headers.size() != 3) throw std::runtime_error("expected 3 vorbis headers");

    // Audio BOS = vorbis identification packet. Flush so it gets its own page.
    impl_->submit_packet(&impl_->audio_os, headers[0].data(), (long)headers[0].size(),
                         0, /*bos=*/true, /*eos=*/false, 0);
    impl_->emit_pages(&impl_->audio_os, /*flush=*/true);

    // Event stream BOS magic must come right after audio BOS, before any data.
    if (impl_->multiplex_events) {
        impl_->submit_packet(&impl_->event_os, kEventStreamMagic, kEventStreamMagicLen,
                             0, /*bos=*/true, /*eos=*/false, 0);
        impl_->emit_pages(&impl_->event_os, /*flush=*/true);
    }

    // Vorbis comment + setup headers.
    impl_->submit_packet(&impl_->audio_os, headers[1].data(), (long)headers[1].size(),
                         0, false, false, 1);
    impl_->submit_packet(&impl_->audio_os, headers[2].data(), (long)headers[2].size(),
                         0, false, false, 2);
    impl_->emit_pages(&impl_->audio_os, /*flush=*/true);
}

OggVorbisEncoder::~OggVorbisEncoder() {
    if (impl_) {
        if (impl_->frame) av_frame_free(&impl_->frame);
        if (impl_->packet) av_packet_free(&impl_->packet);
        if (impl_->swr) swr_free(&impl_->swr);
        if (impl_->cctx) avcodec_free_context(&impl_->cctx);
        ogg_stream_clear(&impl_->audio_os);
        if (impl_->event_os_inited) ogg_stream_clear(&impl_->event_os);
    }
}

void OggVorbisEncoder::write_audio(const void* pcm, std::size_t bytes) {
    if (bytes == 0) return;
    int bytes_per_sample = impl_->input_bits / 8;
    int in_samples = static_cast<int>(bytes / (bytes_per_sample * impl_->channels));
    if (in_samples == 0) return;

    // Convert input to fltp into a scratch frame, then append to pending channels.
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

    // Drain pending in frame_size chunks.
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

void OggVorbisEncoder::write_event(const void* serialized_event, std::size_t bytes) {
    if (!impl_->multiplex_events || bytes == 0) return;
    impl_->submit_packet(&impl_->event_os, serialized_event, static_cast<long>(bytes),
                         impl_->samples_in, /*bos=*/false, /*eos=*/false,
                         impl_->packetno_event++);
    impl_->emit_pages(&impl_->event_os, /*flush=*/false);
}

void OggVorbisEncoder::finish() {
    // Encode any pending tail (zero-padded if not a full frame).
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

    // Drain encoder.
    impl_->encode_and_mux(nullptr);

    // EOS for both streams. Vorbis: empty packet with e_o_s=1 + final granulepos.
    impl_->submit_packet(&impl_->audio_os, "", 0, impl_->samples_in,
                         /*bos=*/false, /*eos=*/true, impl_->packetno_audio++);
    impl_->emit_pages(&impl_->audio_os, /*flush=*/true);

    if (impl_->multiplex_events) {
        impl_->submit_packet(&impl_->event_os, "", 0, impl_->samples_in,
                             /*bos=*/false, /*eos=*/true, impl_->packetno_event++);
        impl_->emit_pages(&impl_->event_os, /*flush=*/true);
    }
}

}  // namespace sapicli
