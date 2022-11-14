#include <windows.h>        // System includes
#include <atlbase.h>		// ATL
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

#include "getoptw.h"

const WCHAR *getErrorString(HRESULT r) {
	switch(r) {
#include "sapierr.h"
	}
	return L"Unknown";
}

void printJsonString(const WCHAR* in) {
	if (!in) {
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

void printJsonKeyPair(const WCHAR* key, const WCHAR* value, int skipComma = 0) {
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

		WCHAR* idString = 0L;
		hr = cpVoiceToken->GetId(&idString);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not get token ID: %d %s\n", hr, getErrorString(hr));
			return 1;
		}
		wprintf(L"{\n");
		WCHAR* idBasename = 0L;
		idBasename = wcsrchr(idString, '\\');
		printJsonKeyPair(L"id", idBasename && idBasename[0] ? idBasename + 1 : idString);

		WCHAR* descriptionString = 0L;
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

		WCHAR* age;
		cpSpAttributesKey->GetStringValue(L"Age", &age);
		printJsonKeyPair(L"age", age);

		WCHAR* gender;
		cpSpAttributesKey->GetStringValue(L"Gender", &gender);
		printJsonKeyPair(L"gender", gender);

		WCHAR* language;
		cpSpAttributesKey->GetStringValue(L"Language", &language);
		WCHAR strNameBuffer[LOCALE_NAME_MAX_LENGTH] = {0};
		int langId = wcstol(language, NULL, 16);
		LCIDToLocaleName(langId, strNameBuffer, LOCALE_NAME_MAX_LENGTH, 0);
		printJsonKeyPair(L"language", strNameBuffer);

		WCHAR* name;
		cpSpAttributesKey->GetStringValue(L"Name", &name);
		printJsonKeyPair(L"name", name);

		WCHAR* vendor;
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
		const WCHAR* word;
		const WCHAR* phone;
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

class PooSpStream: public ISpStreamFormat, public ISpEventSink {
public:
	CComPtr<ISpEventSink> sink;
	HANDLE h;
	PooSpStream() {
		h = GetStdHandle(STD_OUTPUT_HANDLE);
	}

	STDMETHODIMP QueryInterface(REFIID riid, void** ppv) {
		if (ppv == NULL) return E_INVALIDARG;
		*ppv = NULL;
		if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream || riid == IID_ISpStreamFormat)
			*ppv = static_cast<ISpStreamFormat*>(this);
		else if (riid == IID_ISpEventSink)
			*ppv = static_cast<ISpEventSink*>(this);
		else return E_NOINTERFACE;
		return S_OK;
	}
	STDMETHODIMP_(ULONG) AddRef(void) { return 1; }
	STDMETHODIMP_(ULONG) Release(void) { return 1; }
	HRESULT STDMETHODCALLTYPE Read(void*, ULONG, ULONG*) { wprintf(L"Read\n"); return 0; }
	HRESULT STDMETHODCALLTYPE Write(const void* buf, ULONG size, ULONG* newPos) {
		WriteFile(h, buf, size, newPos, NULL);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) {
		if(plibNewPosition)
			plibNewPosition->QuadPart = dlibMove.QuadPart;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) { return 0; }
	HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) { return 0; }
	HRESULT STDMETHODCALLTYPE Commit(DWORD) { return 0; }
	HRESULT STDMETHODCALLTYPE Revert(void) { return 0; }
	HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) { return 0; }
	HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) { return 0; }
	HRESULT STDMETHODCALLTYPE Stat(STATSTG*, DWORD) { return 0; }
	HRESULT STDMETHODCALLTYPE Clone(IStream**) { return 0; }
	HRESULT STDMETHODCALLTYPE GetFormat(GUID* pguidFormatId, WAVEFORMATEX**format) {
		return SpConvertStreamFormatEnum(SPSF_16kHz16BitMono, pguidFormatId, format);
	}
	HRESULT STDMETHODCALLTYPE AddEvents(const SPEVENT* pEventArray, ULONG ulCount) {
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetEventInterest(ULONGLONG* pullEventInterest) {
		*pullEventInterest = 0xFFFFFFFF;
		return S_OK;
	}
};

int speakToWav(WCHAR *text, WCHAR *voiceId, WCHAR *wavFilename, int rate, int volume, DWORD speakFlags, ULONGLONG ullEventInterest) {
	HRESULT hr;

	if(addLexemes())
		return 1;

	CComPtr<ISpVoice> voice;
	hr = voice.CoCreateInstance(CLSID_SpVoice);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not create voice instance: %d %s\n", hr, getErrorString(hr));
		return 1;
	}

	WCHAR fullVoiceId[MAX_PATH];
	_snwprintf_s(fullVoiceId, MAX_PATH, _TRUNCATE, L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\%s", voiceId);

	CComPtr<ISpObjectToken> voiceToken;
	hr = SpGetTokenFromId(fullVoiceId, &voiceToken);
	if (FAILED(hr)) {
		fwprintf(stderr, L"Could not get token for voice \"%s\": %d %s\n", voiceId, hr, getErrorString(hr));
		return 1;
	}

	hr = voice->SetVoice(voiceToken);
	if (FAILED(hr)) {
		fwprintf(stderr, L"Could not set voice: %d %s\n", hr, getErrorString(hr));
		return 1;
	}

	PooSpStream poo;
	hr = voice->SetOutput(static_cast<IUnknown*>(static_cast<ISpStreamFormat*>(&poo)), FALSE);
	if (FAILED(hr)) {
		fwprintf(stderr, L"Could not set output: %d %s\n", hr, getErrorString(hr));
		return 1;
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

	hr = voice->Speak(text, speakFlags, 0);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not speak: %d %s\n", hr, getErrorString(hr));
		return 1;
	}

	voiceToken.Release();

	return 0;
}

int wmain(int argc, WCHAR* argv[]) {
	// https://stackoverflow.com/questions/2492077/output-unicode-strings-in-windows-console-app
	(void)_setmode(_fileno(stdout), _O_U8TEXT);

	const struct option long_options[] = {
		{L"help",                     no_argument,       0, L'h'},
		{L"list",                     no_argument,       0, L'l'},
		{L"output",                   required_argument, 0, L'o'},
		{L"voice",                    required_argument, 0, L'v'},
		{L"type",                     required_argument, 0, L't'},
		{L"rate",                     required_argument, 0, L'r'},
		{L"volume",                   required_argument, 0, L'V'},
		{L"all-events",               no_argument,       0, L'a'},
		{L"start-input-stream-event", no_argument,       0, L'S'},
		{L"end-input-stream-event",   no_argument,       0, L'E'},
		{L"voice-change-event",       no_argument,       0, L'C'},
		{L"bookmark-event",           no_argument,       0, L'B'},
		{L"word-boundary-event",      no_argument,       0, L'W'},
		{L"phoneme-event",            no_argument,       0, L'F'},
		{L"sentence-boundary-event",  no_argument,       0, L'N'},
		{L"viseme-event",             no_argument,       0, L'I'},
		{L"audio-level-event",        no_argument,       0, L'L'},
		{0, 0, 0, 0},
	};

	int help = 0;
	int list = 0;
	WCHAR *voice = 0;
	WCHAR *wavFilename = 0;
	DWORD speakFlags = 0;
	int rate = 0;
	int volume = 100;
	ULONGLONG ullEventInterest = 0;

	int option;
	int option_index = 0;
	while(1) {
		option = getoptW_long(argc, argv, L"hlo:v:t:r:VaSECBWFNIL", long_options, &option_index);

		if(option == -1) break;

		switch (option) {
			case L'h':
				help = 1;
				break;
			case L'l':
				list = 1;
				break;
			case L'o':
				wavFilename = optarg;
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
			case L'a':
				ullEventInterest = SPFEI_ALL_EVENTS;
				break;
			case L'S':
				ullEventInterest |= SPEI_START_INPUT_STREAM;
				break;
			case L'E':
				ullEventInterest |= SPEI_END_INPUT_STREAM;
				break;
			case L'C':
				ullEventInterest |= SPEI_VOICE_CHANGE;
				break;
			case L'B':
				ullEventInterest |= SPEI_TTS_BOOKMARK;
				break;
			case L'W':
				ullEventInterest |= SPEI_WORD_BOUNDARY;
				break;
			case L'F':
				ullEventInterest |= SPEI_PHONEME;
				break;
			case L'N':
				ullEventInterest |= SPEI_SENTENCE_BOUNDARY;
				break;
			case L'I':
				ullEventInterest |= SPEI_VISEME;
				break;
			case L'L':
				ullEventInterest |= SPEI_TTS_AUDIO_LEVEL;
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
			L"  -o, --output=FILE               Output WAV file.\n"
			L"  -v, --voice=VOICE               Select voice.\n"
			L"  -t, --type=TYPE                 Input text type (PLAIN,SSML,SAPI,AUTO).\n"
			L"  -r, --rate=RATE                 Rate (speed) of speech, from -10 to 10.\n"
			L"  -V, --volume=VOL                Volume of speech, from 0 to 100.\n"
			L"  -a, --all-events                Log all events in the EVNT RIFF chunk.\n"
			L"  -S, --start-input-stream-event  Log start input stream events.\n"
			L"  -E, --end-input-stream-event    Log end input stream events.\n"
			L"  -C, --voice-change-event        Log voice change events.\n"
			L"  -B, --bookmark-event            Log bookmark events.\n"
			L"  -W, --word-boundary-event       Log word boundary events.\n"
			L"  -F, --phoneme-event             Log phoneme events.\n"
			L"  -N, --sentence-boundary-event   Log sentence boundary events.\n"
			L"  -I, --viseme-event              Log viseme events.\n"
			L"  -L, --audio-level-event         Log audio level events.\n",
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
		ret = speakToWav(argv[optind], voice, wavFilename, rate, volume, speakFlags, ullEventInterest);
	}

	::CoUninitialize();

	return ret;
}
