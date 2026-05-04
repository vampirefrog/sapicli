#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sapicli {

struct SynthFormat {
    std::uint32_t sample_rate;
    std::uint16_t channels;
    std::uint16_t bits_per_sample;
};

enum class SpeakMode {
    Text,
    Ssml,
    Sapi,
    Auto,
};

// SAPI text-to-speech driver. One instance owns one ISpVoice and is bound to
// the COM apartment of the thread that constructed it. A new instance per
// thread; do not share across threads.
//
// For non-WAV formats: configure the audio + event sinks then call speak().
// SAPI will call audio_sink with raw PCM bytes and event_sink with serialized
// SPSERIALIZEDEVENT blobs as synthesis progresses.
//
// For WAV: call speak_to_wav_file(). This uses SAPI's native CSpStream which
// writes RIFF + EVNT chunks directly; the configured sinks are not invoked.
class Synthesizer {
public:
    using AudioSink = std::function<void(const void*, std::size_t)>;
    using EventSink = std::function<void(const void*, std::size_t)>;

    Synthesizer();
    ~Synthesizer();

    void set_voice(const std::wstring& voice_id);   // basename, e.g. TTS_MS_EN-US_DAVID_11.0
    void set_rate(int rate);                         // -10..10
    void set_volume(int volume);                     // 0..100
    void set_format(const SynthFormat& fmt);
    void set_event_interest(std::uint64_t mask);     // ULONGLONG mask of SPEI_* bits
    void set_audio_sink(AudioSink sink);
    void set_event_sink(EventSink sink);

    // Speak via the configured callback sinks. Blocks until synthesis completes.
    void speak(const std::wstring& text, SpeakMode mode);

    // Speak directly to a WAV file via SAPI's native CSpStream. RIFF + EVNT
    // are written by SAPI. Audio/event sinks are not invoked. ullEventInterest
    // applies (controls what SAPI emits into the EVNT chunk).
    void speak_to_wav_file(const std::wstring& filename, const std::wstring& text, SpeakMode mode);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sapicli
