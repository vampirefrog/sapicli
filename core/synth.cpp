#include "synth.h"

#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include <sapi.h>
#pragma warning(push)
#pragma warning(disable: 4996)
#include <sphelper.h>
#include <spddkhlp.h>
#pragma warning(pop)

#include <stdexcept>
#include <vector>

namespace sapicli {

namespace {

DWORD flags_for(SpeakMode m) {
    switch (m) {
        case SpeakMode::Text: return SPF_IS_NOT_XML;
        case SpeakMode::Ssml: return SPF_IS_XML | SPF_PARSE_SSML;
        case SpeakMode::Sapi: return SPF_IS_XML | SPF_PARSE_SAPI;
        case SpeakMode::Auto: return SPF_IS_XML | SPF_PARSE_AUTODETECT;
    }
    return SPF_IS_NOT_XML;
}

void throw_hr(const char* what, HRESULT hr) {
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s (hr=0x%08x)", what, (unsigned)hr);
    throw std::runtime_error(buf);
}

// SAPI output stream that routes Write() to an audio callback and AddEvents()
// to an event callback. SAPI QueryInterface()'s the output stream for
// ISpEventSink and uses it for events when the interest mask is non-zero.
class SynthSink final : public ISpStream, public ISpEventSink {
public:
    SynthSink(const WAVEFORMATEX& wfex, ULONGLONG event_interest,
              Synthesizer::AudioSink audio_sink, Synthesizer::EventSink event_sink)
        : wfex_(wfex), event_interest_(event_interest),
          audio_sink_(std::move(audio_sink)), event_sink_(std::move(event_sink)) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream
                || riid == IID_ISpStreamFormat || riid == IID_ISpStream) {
            *ppv = static_cast<ISpStreamFormat*>(this);
        } else if (riid == IID_ISpEventSink) {
            *ppv = static_cast<ISpEventSink*>(this);
        } else {
            return E_NOINTERFACE;
        }
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 1; }
    STDMETHODIMP_(ULONG) Release() override { return 1; }

    // ISequentialStream / IStream stubs
    STDMETHODIMP Read(void*, ULONG, ULONG*) override { return S_OK; }
    STDMETHODIMP Seek(LARGE_INTEGER move, DWORD, ULARGE_INTEGER* newpos) override {
        if (newpos) newpos->QuadPart = move.QuadPart;
        return S_OK;
    }
    STDMETHODIMP SetSize(ULARGE_INTEGER) override { return S_OK; }
    STDMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return S_OK; }
    STDMETHODIMP Commit(DWORD) override { return S_OK; }
    STDMETHODIMP Revert() override { return S_OK; }
    STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return S_OK; }
    STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return S_OK; }
    STDMETHODIMP Stat(STATSTG*, DWORD) override { return S_OK; }
    STDMETHODIMP Clone(IStream**) override { return S_OK; }

    STDMETHODIMP Write(const void* buf, ULONG size, ULONG* written) override {
        if (audio_sink_) audio_sink_(buf, size);
        if (written) *written = size;
        return S_OK;
    }

    // ISpStreamFormat
    STDMETHODIMP GetFormat(GUID* format_id, WAVEFORMATEX** fmt) override {
        if (format_id) *format_id = SPDFID_WaveFormatEx;
        if (fmt) {
            *fmt = (WAVEFORMATEX*)::CoTaskMemAlloc(sizeof(WAVEFORMATEX));
            if (!*fmt) return E_OUTOFMEMORY;
            CopyMemory(*fmt, &wfex_, sizeof(WAVEFORMATEX));
        }
        return S_OK;
    }

    // ISpStream
    STDMETHODIMP SetBaseStream(IStream*, REFGUID, const WAVEFORMATEX*) override { return S_OK; }
    STDMETHODIMP GetBaseStream(IStream**) override { return S_OK; }
    STDMETHODIMP BindToFile(LPCWSTR, SPFILEMODE, const GUID*, const WAVEFORMATEX*, ULONGLONG) override {
        return S_OK;
    }
    STDMETHODIMP Close() override { return S_OK; }

    // ISpEventSink
    STDMETHODIMP AddEvents(const SPEVENT* events, ULONG count) override {
        if (!event_sink_) return S_OK;
        for (ULONG i = 0; i < count; ++i) {
            CSpEvent ev;
            ev.CopyFrom(&events[i]);
            ULONG sz = ev.SerializeSize<SPSERIALIZEDEVENT>();
            std::vector<BYTE> buf(sz);
            ev.Serialize<SPSERIALIZEDEVENT>(reinterpret_cast<SPSERIALIZEDEVENT*>(buf.data()));
            event_sink_(buf.data(), sz);
        }
        return S_OK;
    }
    STDMETHODIMP GetEventInterest(ULONGLONG* mask) override {
        if (mask) *mask = event_interest_;
        return S_OK;
    }

private:
    WAVEFORMATEX wfex_;
    ULONGLONG event_interest_;
    Synthesizer::AudioSink audio_sink_;
    Synthesizer::EventSink event_sink_;
};

WAVEFORMATEX make_wfex(const SynthFormat& fmt) {
    WAVEFORMATEX w{};
    w.wFormatTag = WAVE_FORMAT_PCM;
    w.nChannels = fmt.channels;
    w.nSamplesPerSec = fmt.sample_rate;
    w.wBitsPerSample = fmt.bits_per_sample;
    w.nBlockAlign = w.nChannels * w.wBitsPerSample / 8;
    w.nAvgBytesPerSec = w.nSamplesPerSec * w.nBlockAlign;
    w.cbSize = 0;
    return w;
}

}  // namespace

struct Synthesizer::Impl {
    CComPtr<ISpVoice> voice;
    SynthFormat fmt{ 22050, 1, 16 };
    ULONGLONG event_interest = 0;
    AudioSink audio_sink;
    EventSink event_sink;
};

Synthesizer::Synthesizer() : impl_(std::make_unique<Impl>()) {
    HRESULT hr = impl_->voice.CoCreateInstance(CLSID_SpVoice);
    if (FAILED(hr)) throw_hr("CoCreateInstance(SpVoice)", hr);
}

Synthesizer::~Synthesizer() = default;

void Synthesizer::set_voice(const std::wstring& voice_id) {
    if (voice_id.empty()) return;
    WCHAR full[MAX_PATH];
    _snwprintf_s(full, MAX_PATH, _TRUNCATE,
                 L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\%s",
                 voice_id.c_str());
    CComPtr<ISpObjectToken> token;
    HRESULT hr = SpGetTokenFromId(full, &token);
    if (FAILED(hr)) throw_hr("SpGetTokenFromId", hr);
    hr = impl_->voice->SetVoice(token);
    if (FAILED(hr)) throw_hr("ISpVoice::SetVoice", hr);
}

void Synthesizer::set_rate(int rate) {
    HRESULT hr = impl_->voice->SetRate(rate);
    if (FAILED(hr)) throw_hr("ISpVoice::SetRate", hr);
}

void Synthesizer::set_volume(int volume) {
    HRESULT hr = impl_->voice->SetVolume(static_cast<USHORT>(volume));
    if (FAILED(hr)) throw_hr("ISpVoice::SetVolume", hr);
}

void Synthesizer::set_format(const SynthFormat& fmt) { impl_->fmt = fmt; }
void Synthesizer::set_event_interest(std::uint64_t mask) { impl_->event_interest = mask; }
void Synthesizer::set_audio_sink(AudioSink sink) { impl_->audio_sink = std::move(sink); }
void Synthesizer::set_event_sink(EventSink sink) { impl_->event_sink = std::move(sink); }

void Synthesizer::speak(const std::wstring& text, SpeakMode mode) {
    WAVEFORMATEX wfex = make_wfex(impl_->fmt);
    SynthSink sink(wfex, impl_->event_interest, impl_->audio_sink, impl_->event_sink);
    HRESULT hr = impl_->voice->SetOutput(static_cast<ISpStreamFormat*>(&sink), FALSE);
    if (FAILED(hr)) throw_hr("ISpVoice::SetOutput", hr);
    hr = impl_->voice->Speak(text.c_str(), flags_for(mode), nullptr);
    if (FAILED(hr)) throw_hr("ISpVoice::Speak", hr);
}

void Synthesizer::speak_to_wav_file(const std::wstring& filename, const std::wstring& text, SpeakMode mode) {
    CComPtr<ISpStream> stream;
    HRESULT hr = ::CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&stream));
    if (FAILED(hr)) throw_hr("CoCreateInstance(SpStream)", hr);
    WAVEFORMATEX wfex = make_wfex(impl_->fmt);
    hr = stream->BindToFile(filename.c_str(), SPFM_CREATE_ALWAYS,
                            &SPDFID_WaveFormatEx, &wfex, impl_->event_interest);
    if (FAILED(hr)) throw_hr("ISpStream::BindToFile", hr);
    hr = impl_->voice->SetOutput(stream, FALSE);
    if (FAILED(hr)) throw_hr("ISpVoice::SetOutput", hr);
    hr = impl_->voice->Speak(text.c_str(), flags_for(mode), nullptr);
    if (FAILED(hr)) throw_hr("ISpVoice::Speak", hr);
    stream->Close();
}

}  // namespace sapicli
