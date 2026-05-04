#include <windows.h>        // System includes
#include <atlbase.h>        // ATL
#include <atlcom.h>
#include <windowsx.h>
#include <wchar.h>
#include <tchar.h>
#include <sapi.h>           // SAPI includes
#pragma warning(push)       // Disable warning C4996: 'GetVersionExA': was declared deprecated (sphelper.h:1319)
#pragma warning(disable: 4996)
#include <sphelper.h>
#include <spddkhlp.h>
#pragma warning(pop)
#include <initguid.h>
#include <io.h>
#include <fcntl.h>

#include "core/encoders/encoder.h"
#include <stdexcept>

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#include "getoptw.h"

const WCHAR *getErrorString(HRESULT r) {
	switch(r) {
#include "sapierr.h"
	}
	return L"Unknown";
}

void printJsonString(const WCHAR *in) {
	if(!in) {
		fwprintf(stdout, L"null");
		return;
	}

	size_t l = wcslen(in);
	fputwc(L'"', stdout);
	for(size_t i = 0; i < l; i++) {
		if(in[i] == L'"')
			fputwc(L'\\', stdout);
		else if(in[i] == L'\\')
			fputwc(L'\\', stdout);
		fputwc(in[i], stdout);
	}
	fputwc(L'"', stdout);
}

void printJsonKeyPair(const WCHAR *key, const WCHAR *value, int skipComma = 0) {
	printJsonString(key);
	wprintf(L": ");
	printJsonString(value);
	if(skipComma)
		wprintf(L"\n");
	else
		wprintf(L",\n");
}

int listVoices() {
	HRESULT hr = 0L;

	CComPtr<IEnumSpObjectTokens> voicesEnum;
	hr = SpEnumTokens(SPCAT_VOICES, NULL, NULL, &voicesEnum);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not enumerate tokens: %d %s\n", hr, getErrorString(hr));
		return 1;
	}

	ULONG ulCount = 0;
	hr = voicesEnum->GetCount(&ulCount);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not get token count: %d %s\n", hr, getErrorString(hr));
		return 1;
	}

	wprintf(L"[\n");

	while(ulCount--) {
		CComPtr<ISpObjectToken> cpVoiceToken;
		hr = voicesEnum->Next(1, &cpVoiceToken, NULL);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not iterate voice token: %d %s\n", hr, getErrorString(hr));
			return 1;
		}

		WCHAR *idString = 0L;
		hr = cpVoiceToken->GetId(&idString);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not get token ID: %d %s\n", hr, getErrorString(hr));
			return 1;
		}
		wprintf(L"{\n");
		WCHAR *idBasename = 0L;
		idBasename = wcsrchr(idString, '\\');
		printJsonKeyPair(L"id", idBasename && idBasename[0] ? idBasename + 1 : idString);

		WCHAR *descriptionString = 0L;
		hr = SpGetDescription(cpVoiceToken, &descriptionString);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not get token description: %d %s\n", hr, getErrorString(hr));
			return 1;
		}
		printJsonKeyPair(L"description", descriptionString);

		CComPtr<ISpDataKey> cpSpAttributesKey;
		hr = cpVoiceToken->OpenKey(L"Attributes", &cpSpAttributesKey);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not open attributes key: %d %s\n", hr, getErrorString(hr));
			return 1;
		}

		WCHAR *age;
		cpSpAttributesKey->GetStringValue(L"Age", &age);
		printJsonKeyPair(L"age", age);

		WCHAR *gender;
		cpSpAttributesKey->GetStringValue(L"Gender", &gender);
		printJsonKeyPair(L"gender", gender);

		WCHAR *language;
		cpSpAttributesKey->GetStringValue(L"Language", &language);
		WCHAR strNameBuffer[LOCALE_NAME_MAX_LENGTH] = { 0 };
		int langId = wcstol(language, NULL, 16);
		LCIDToLocaleName(langId, strNameBuffer, LOCALE_NAME_MAX_LENGTH, 0);
		printJsonKeyPair(L"language", strNameBuffer);

		WCHAR *name;
		cpSpAttributesKey->GetStringValue(L"Name", &name);
		printJsonKeyPair(L"name", name);

		WCHAR *vendor;
		cpSpAttributesKey->GetStringValue(L"Vendor", &vendor);
		printJsonKeyPair(L"vendor", vendor, 1);

		if(ulCount > 0)
			wprintf(L"},\n");
		else
			wprintf(L"}\n");
	}

	wprintf(L"]\n");

	return 0;
}

int addLexemes() {
	HRESULT hr;

	CComPtr<ISpLexicon> cpLexicon;
	hr = cpLexicon.CoCreateInstance(CLSID_SpLexicon);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not instantiate lexicon: %d %s\n", hr, getErrorString(hr));
		return 1;
	}

	LANGID langidUS = MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
	CComPtr<ISpPhoneConverter> cpPhoneConv;
	hr = SpCreatePhoneConverter(langidUS, NULL, NULL, &cpPhoneConv);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not instantiate phoneme converter: %d %s\n", hr, getErrorString(hr));
		return 1;
	}

	const struct {
		LANGID langId;
		const WCHAR *word;
		const WCHAR *phone;
	} lexemes[] = {
		{ langidUS, L"cum", L"k uw m" },
		{ langidUS, L"poo", L"p uw" },
		{ langidUS, L"lol", L"l uh l" },
		{ langidUS, L"lolol", L"l uh l uh l" },
		{ langidUS, L"deez", L"d iy z" },
		{ langidUS, L"nutz", L"n ah t s" },
		{ langidUS, L"nasim", L"n ah s iy m" },
	};

	for(int i = 0; i < sizeof(lexemes) / sizeof(lexemes[0]); i++) {
		SPPHONEID wszId[SP_MAX_PRON_LENGTH];
		hr = cpPhoneConv->PhoneToId(lexemes[i].phone, wszId);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not convert phoneme \"%s\" to id: ", lexemes[i].phone);
			if(hr == E_INVALIDARG)
				fwprintf(stderr, L"Invalid argument");
			else if(hr == SPERR_UNINITIALIZED)
				fwprintf(stderr, L"Uninitialized");
			else if(hr == E_FAIL)
				fwprintf(stderr, L"Failed");
			else fwprintf(stderr, L"%d", hr);
			fwprintf(stderr, L"\n");
			continue;
		}

		hr = cpLexicon->AddPronunciation(lexemes[i].word, lexemes[i].langId, SPPS_Noun, wszId);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not add pronounciation for word \"%s\": %d %s\n", lexemes[i].word, hr, getErrorString(hr));
			continue;
		}
	}

	return 0;
}

class MuxSpStream: public ISpStream, public ISpEventSink {
public:
	LPCWSTR filename;
	WAVEFORMATEX wfex;
	const GUID *formatId;
	ULONGLONG ullEventInterest;
	LONG format;
	BOOL multiplex;
	HANDLE h;
	BOOL isStdout;
	HANDLE eh; // events file handle (used when not multiplexing into the encoder)
	std::unique_ptr<sapicli::Encoder> encoder;

	MuxSpStream(LONG format_, BOOL multiplex_): filename(0), wfex{ 0 }, formatId(0), ullEventInterest(0), format(format_), multiplex(multiplex_), h(0), isStdout(0), eh(0) {}

	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) {
		if(ppv == NULL) return E_INVALIDARG;
		*ppv = NULL;
		if(riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream || riid == IID_ISpStreamFormat || riid == IID_ISpStream)
			*ppv = static_cast<ISpStreamFormat *>(this);
		else if(riid == IID_ISpEventSink)
			*ppv = static_cast<ISpEventSink *>(this);
		else return E_NOINTERFACE;
		return S_OK;
	}
	STDMETHODIMP_(ULONG) AddRef(void) {
		return 1;
	}
	STDMETHODIMP_(ULONG) Release(void) {
		return 1;
	}
	STDMETHODIMP Read(void *, ULONG, ULONG *) {
		return 0;
	}
	STDMETHODIMP Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) {
		if(plibNewPosition)
			plibNewPosition->QuadPart = dlibMove.QuadPart;
		return S_OK;
	}
	STDMETHODIMP SetSize(ULARGE_INTEGER) {
		return 0;
	}
	STDMETHODIMP CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *) {
		return 0;
	}
	STDMETHODIMP Commit(DWORD) {
		return 0;
	}
	STDMETHODIMP Revert(void) {
		return 0;
	}
	STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) {
		return 0;
	}
	STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) {
		return 0;
	}
	STDMETHODIMP Stat(STATSTG *, DWORD) {
		return 0;
	}
	STDMETHODIMP Clone(IStream **) {
		return 0;
	}
	STDMETHODIMP GetFormat(GUID *pguidFormatId, WAVEFORMATEX **format) {
		*pguidFormatId = *formatId;
		WAVEFORMATEX *pwfex = (WAVEFORMATEX *)::CoTaskMemAlloc(sizeof(WAVEFORMATEX));
		if(!pwfex) return E_OUTOFMEMORY;
		CopyMemory(pwfex, &wfex, sizeof(WAVEFORMATEX));
		*format = pwfex;
		return S_OK;
	}

	// FIXME: optimize by not allocating every time
	STDMETHODIMP writeSpEvent(const SPEVENT *ev) {
		CSpEvent cspev;
		cspev.CopyFrom(ev);
		ULONG sz = cspev.SerializeSize<SPSERIALIZEDEVENT>();
		BYTE *buf = new BYTE[sz];
		cspev.Serialize<SPSERIALIZEDEVENT>((SPSERIALIZEDEVENT *)buf);
		writeEventData(buf, sz);
		delete[] buf;
		return S_OK;
	}

	STDMETHODIMP AddEvents(const SPEVENT *pEventArray, ULONG ulCount) {
		for(ULONG i = 0; i < ulCount; i++) {
			const SPEVENT *ev = &pEventArray[i];
			writeSpEvent(ev);
		}
		return S_OK;
	}

	STDMETHODIMP GetEventInterest(ULONGLONG *pullEventInterest) {
		*pullEventInterest = ullEventInterest;
		return S_OK;
	}

	STDMETHODIMP SetBaseStream(IStream *pStream, REFGUID rguidFormat, const WAVEFORMATEX *pWaveFormatEx) {
		return S_OK;
	}

	STDMETHODIMP GetBaseStream(IStream **ppStream) {
		return S_OK;
	}

	virtual STDMETHODIMP BindToFile(LPCWSTR filename_, SPFILEMODE eMode, const GUID *pFormatId, const WAVEFORMATEX *pWaveFormatEx, ULONGLONG ullEventInterest_) {
		if(SP_IS_BAD_STRING_PTR(filename_) || eMode >= SPFM_NUM_MODES || SP_IS_BAD_OPTIONAL_READ_PTR(pFormatId))
			return E_INVALIDARG;

		filename = filename_;
		ullEventInterest = ullEventInterest_;
		formatId = pFormatId;
		CopyMemory(&wfex, pWaveFormatEx, sizeof(WAVEFORMATEX));

		isStdout = filename_ && filename_[0] == '-' && filename_[1] == 0;
		if(isStdout) {
			h = GetStdHandle(STD_OUTPUT_HANDLE);
		} else {
			h = CreateFileW(filename_, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
			if(h == INVALID_HANDLE_VALUE) {
				DWORD e = GetLastError();
				WCHAR buf[MAX_PATH];
				FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, e, 0, buf, sizeof(buf) / sizeof(buf[0]), 0);
				fwprintf(stderr, L"Could not open \"%s\" for writing: %d %s\n", filename_, e, buf);
				return HRESULT_FROM_WIN32(e);
			}
		}

		bool encodes_events = multiplex && format != 1;
		if(ullEventInterest_) {
			if(encodes_events) {
				eh = NULL;  // events handled by the encoder
			} else if(isStdout) {
				eh = (HANDLE)_get_osfhandle(3);
			} else {
				fwprintf(stderr, L"Cannot select events (0x%04llx) when output is not stdout and multiplexing is not enabled\n", ullEventInterest_);
				return E_INVALIDARG;
			}
		}

		if(format == 1) {
			// raw PCM: no encoder, Write() goes directly to the handle.
			return S_OK;
		}

		sapicli::EncoderOptions opts{};
		opts.audio.sample_rate = pWaveFormatEx->nSamplesPerSec;
		opts.audio.channels = pWaveFormatEx->nChannels;
		opts.audio.bits_per_sample = pWaveFormatEx->wBitsPerSample;
		opts.multiplex_events = !!encodes_events;
		switch(format) {
			case 3: opts.format = sapicli::Format::OggVorbis; break;
			case 4: opts.format = sapicli::Format::OggOpus; break;
			case 5: opts.format = sapicli::Format::Mp3; break;
			default:
				fwprintf(stderr, L"Invalid format %d\n", format);
				return E_INVALIDARG;
		}
		HANDLE audio_h = h;
		try {
			encoder = sapicli::make_encoder(opts, [audio_h](const void* data, std::size_t len) {
				DWORD written;
				WriteFile(audio_h, data, (DWORD)len, &written, NULL);
			});
		} catch(const std::exception& e) {
			fwprintf(stderr, L"Could not init encoder: %hs\n", e.what());
			return E_FAIL;
		}

		return S_OK;
	}

	virtual STDMETHODIMP Close(void) {
		if(encoder) {
			try {
				encoder->finish();
			} catch(const std::exception& e) {
				fwprintf(stderr, L"Could not finish encoder: %hs\n", e.what());
				return E_FAIL;
			}
			encoder.reset();
		}

		if(isStdout || !h) return S_OK;

		BOOL b = CloseHandle(h);
		if(b) return S_OK;

		DWORD e = GetLastError();
		WCHAR buf[MAX_PATH];
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, e, 0, buf, sizeof(buf) / sizeof(buf[0]), 0);
		fwprintf(stderr, L"Could not close \"%s\": 0x%08x (%s)", filename, e, buf);
		return HRESULT_FROM_WIN32(e);
	}

	virtual HRESULT STDMETHODCALLTYPE Write(const void *buf, ULONG size, ULONG *newPos) {
		if(encoder) {
			try {
				encoder->write_audio(buf, size);
			} catch(const std::exception& e) {
				fwprintf(stderr, L"write_audio failed: %hs\n", e.what());
				return E_FAIL;
			}
			if(newPos) *newPos = size;
			return S_OK;
		}
		// raw PCM: pass through directly
		DWORD written;
		BOOL b = WriteFile(h, buf, size, &written, NULL);
		if(newPos) *newPos = written;
		return b ? S_OK : E_FAIL;
	}

	virtual STDMETHODIMP writeEventData(void *buf, size_t size) {
		if(encoder && multiplex) {
			try {
				encoder->write_event(buf, size);
			} catch(const std::exception& e) {
				fwprintf(stderr, L"write_event failed: %hs\n", e.what());
				return E_FAIL;
			}
			return S_OK;
		}
		if(eh) {
			DWORD written;
			BOOL b = WriteFile(eh, buf, (DWORD)size, &written, NULL);
			return (b && written == size) ? S_OK : E_FAIL;
		}
		return S_OK;
	}
};

int speakToWav(WCHAR *text, WCHAR *voiceId, WCHAR *wavFilename, DWORD outType, int rate, int volume, DWORD speakFlags, DWORD samplesPerSec, WORD bitsPerSample, WORD nChannels, ULONGLONG ullEventInterest, BOOL multiplex) {
	HRESULT hr;

	if(SP_IS_BAD_STRING_PTR(wavFilename)) {
		fwprintf(stderr, L"Invalid filename\n");
		return 1;
	}

	if(SP_IS_BAD_STRING_PTR(text)) {
		fwprintf(stderr, L"Invalid text\n");
		return 1;
	}

	// detect output type by file extension
	if(outType == 0) {
		outType = 1;
		if(wavFilename && wavFilename[0]) {
			size_t s = wcslen(wavFilename);
			if(s >= 4) {
				if(!_wcsicmp(wavFilename + s - 4, L".wav"))
					outType = 2;
				else if(!_wcsicmp(wavFilename + s - 4, L".ogg"))
					outType = 3;
				else if(!_wcsicmp(wavFilename + s - 4, L".mp3"))
					outType = 5;
			}
		}
	}

	if(addLexemes())
		return 1;

	CComPtr<ISpVoice> voice;
	hr = voice.CoCreateInstance(CLSID_SpVoice);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not create voice instance: %d %s\n", hr, getErrorString(hr));
		return 1;
	}

	CComPtr<ISpObjectToken> voiceToken;
	if(voiceId && voiceId[0]) {
		WCHAR fullVoiceId[MAX_PATH];
		_snwprintf_s(fullVoiceId, MAX_PATH, _TRUNCATE, L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\%s", voiceId);

		hr = SpGetTokenFromId(fullVoiceId, &voiceToken);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not get token for voice \"%s\": %d %s\n", voiceId, hr, getErrorString(hr));
			return 1;
		}

		hr = voice->SetVoice(voiceToken);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not set voice: %d %s\n", hr, getErrorString(hr));
			return 1;
		}
	}

	hr = voice->SetRate(rate);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not set rate to %d: %d %s\n", rate, hr, getErrorString(hr));
		return 1;
	}

	hr = voice->SetVolume(volume);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not set volume to %d: %d %s\n", volume, hr, getErrorString(hr));
		return 1;
	}

	ISpStream *outputStream = 0;
	if(outType == 2) {
		HRESULT hr = ::CoCreateInstance(CLSID_SpStream, NULL, CLSCTX_ALL, __uuidof(outputStream), (void **)&outputStream);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not instantiate SpStream: %d %s\n", hr, getErrorString(hr));
			return 1;
		}
	} else if(outType == 1 || outType == 3 || outType == 4 || outType == 5) {
		outputStream = new MuxSpStream(outType, multiplex);
	} else {
		fwprintf(stderr, L"Invalid output type %d\n", outType);
		return E_INVALIDARG;
	}

	if(!outputStream) {
		fwprintf(stderr, L"Could not initialize output stream\n");
		return E_FAIL;
	}

	WAVEFORMATEX wfex;
	wfex.wFormatTag = WAVE_FORMAT_PCM;
	wfex.nChannels = nChannels;
	wfex.nSamplesPerSec = samplesPerSec;
	wfex.wBitsPerSample = bitsPerSample;
	wfex.nBlockAlign = wfex.nChannels * wfex.wBitsPerSample / 8;
	wfex.nAvgBytesPerSec = wfex.nSamplesPerSec * wfex.nBlockAlign;
	wfex.cbSize = 0;
	hr = outputStream->BindToFile(wavFilename, SPFM_CREATE_ALWAYS, &SPDFID_WaveFormatEx, &wfex, ullEventInterest);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not bind to file %s: %d %s\n", wavFilename, hr, getErrorString(hr));
		outputStream->Release();
		return 1;
	}

	hr = voice->SetOutput(outputStream, FALSE);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not set output: %d %s\n", hr, getErrorString(hr));
		return 1;
	}

	hr = voice->Speak(text, speakFlags, 0);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not speak: %x %s\n", hr, getErrorString(hr));
		return 1;
	}

	// Release here so the destructor doesn't do it after we've closed the output file
	voice.Release();

	hr = outputStream->Close();
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not close output stream \"%s\": 0x%08x %s\n", wavFilename, hr, getErrorString(hr));
		return 1;
	}

	if(voiceId && voiceId[0])
		voiceToken.Release();

	return 0;
}

int wmain(int argc, WCHAR *argv[]) {
	// https://stackoverflow.com/questions/2492077/output-unicode-strings-in-windows-console-app
	(void)_setmode(_fileno(stdout), _O_U8TEXT);

	const struct option long_options[] = {
		{ L"help", no_argument, 0, L'h' },
		{ L"list", no_argument, 0, L'l' },
		{ L"output", required_argument, 0, L'o' },
		{ L"out-type", required_argument, 0, L'T' },
		{ L"voice", required_argument, 0, L'v' },
		{ L"type", required_argument, 0, L't' },
		{ L"rate", required_argument, 0, L'r' },
		{ L"volume", required_argument, 0, L'V' },
		{ L"sample-rate", required_argument, 0, L's' },
		{ L"bits", required_argument, 0, L'b' },
		{ L"channels", required_argument, 0, L'c' },
		{ L"events", required_argument, 0, L'e' },
		{ L"multiplex", no_argument, 0, L'm' },
		{ 0, 0, 0, 0 },
	};

	int help = 0;
	int list = 0;
	WCHAR *voice = 0;
	WCHAR *wavFilename = 0;
	DWORD speakFlags = 0;
	int rate = 0;
	int volume = 100;
	DWORD samplesPerSec = 22050;
	WORD bitsPerSample = 16, nChannels = 1;
	ULONGLONG ullEventInterest = 0;
	DWORD outType = 0;
	BOOL multiplex = FALSE;

	int option;
	int option_index = 0;
	while(1) {
		option = getoptW_long(argc, argv, L"hlo:T:v:t:r:Vs:b:c:e:m", long_options, &option_index);
		if(option == L'?') {
			return 1;
		}

		if(option == -1) break;

		switch(option) {
			case L'h':
				help = 1;
				break;
			case L'l':
				list = 1;
				break;
			case L'o':
				wavFilename = optarg;
				break;
			case L'T':
				if(!_wcsicmp(optarg, L"auto"))
					outType = 0;
				else if(!_wcsicmp(optarg, L"raw"))
					outType = 1;
				else if(!_wcsicmp(optarg, L"wav"))
					outType = 2;
				else if(!_wcsicmp(optarg, L"ogg") || !_wcsicmp(optarg, L"ogg+vorbis"))
					outType = 3;
				else if(!_wcsicmp(optarg, L"ogg+opus"))
					outType = 4;
				else if(!_wcsicmp(optarg, L"mp3"))
					outType = 5;
				else
					help = 1;
				break;
			case L'v':
				voice = optarg;
				break;
			case L't':
				if(!_wcsicmp(optarg, L"ssml"))
					speakFlags = SPF_IS_XML | SPF_PARSE_SSML;
				else if(!_wcsicmp(optarg, L"sapi"))
					speakFlags = SPF_IS_XML | SPF_PARSE_SAPI;
				else if(!_wcsicmp(optarg, L"auto"))
					speakFlags = SPF_IS_XML | SPF_PARSE_AUTODETECT;
				else if(!_wcsicmp(optarg, L"text"))
					speakFlags = SPF_IS_NOT_XML;
				else
					help = 1;
				break;
			case L'r':
				rate = wcstol(optarg, 0, 10);
				break;
			case L'V':
				volume = wcstol(optarg, 0, 10);
				break;
			case L's':
				samplesPerSec = wcstol(optarg, 0, 10);
				break;
			case L'b':
				bitsPerSample = (WORD)wcstol(optarg, 0, 10);
				break;
			case L'c':
				nChannels = (WORD)wcstol(optarg, 0, 10);
				break;
			case L'e':
				if(!_wcsicmp(optarg, L"all"))
					ullEventInterest = 0xfffe;
				else
					ullEventInterest = wcstol(optarg, 0, 0);
				break;
			case L'm':
				multiplex = TRUE;
				break;
		}
	}

	if(!list && optind >= argc)
		help = 1;

	if(help) {
		fwprintf(
			stderr,
			L"Usage: %s --list | [options] <text>\n"
			L"  -h, --help                      Print this help.\n"
			L"  -l, --list                      List all voices.\n"
			L"  -o, --output=FILE               Output file. Default is `output.wav`\n"
			L"                                  Use `-' for stdout.\n"
			L"  -T, --out-type=TYPE             Output file type. Default is `auto'\n"
			L"                                  `wav' for RIFF .wav\n"
			L"                                  `ogg' or `ogg+vorbis' for Ogg Vorbis\n"
			L"                                  `ogg+opus' for Ogg Opus\n"
			L"                                  `mp3' for MP3\n"
			L"                                  `raw' for raw PCM samples\n"
			L"                                  `auto' to autodetect from file extension\n"
			L"  -v, --voice=VOICE               Select voice.\n"
			L"  -r, --rate=RATE                 Rate (speed) of speech, from -10 to 10.\n"
			L"  -t, --type=TYPE                 Input text type (PLAIN,SSML,SAPI,AUTO).\n"
			L"  -V, --volume=VOL                Volume of speech, from 0 to 100.\n"
			L"  -s, --sample-rate=HZ            Sample rate of output. Default 22050.\n"
			L"  -b, --bits=BITS                 Bit depth of output. Default 16.\n"
			L"  -c, --channels=CHANNELS         Number of audio channels of output. Default 1.\n"
			L"  -e, --events=MASK               Select events that are output.\n"
			L"                                  Possible values, bitwise ORed:\n"
			L"                                  Stream start      2\n"
			L"                                  Stream end        4\n"
			L"                                  Voice change      8\n"
			L"                                  Bookmark          16\n"
			L"                                  Word boundary     32\n"
			L"                                  Phoneme           64\n"
			L"                                  Sentence boundary 128\n"
			L"                                  Viseme            256\n"
			L"                                  Audio level       512\n"
			L"                                  All TTS events    65534 or `all'\n"
			L"                                  By default, events are logged into the\n"
			L"                                  output stream if it is a .wav or an .ogg.\n"
			L"                                  If output is stdout (`-'), and the event mask\n"
			L"                                  is non zero, events are output on\n"
			L"                                  file descriptor 3.\n"
			L"  -m, --multiplex                 Multiplex audio and speech event data into the\n"
			L"                                  same output. See README.md for how this works.\n",
			argv[0]
		);
		return 1;
	}

	HRESULT hr = ::CoInitialize(NULL);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not initialize COM: %x %s\n", hr, getErrorString(hr));
		return 1;
	}

	int ret = 0;
	if(list) {
		ret = listVoices();
	} else {
		ret = speakToWav(argv[optind], voice, wavFilename, outType, rate, volume, speakFlags, samplesPerSec, bitsPerSample, nChannels, ullEventInterest, multiplex);
	}

	::CoUninitialize();

	return ret;
}
