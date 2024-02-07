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

#include <vorbis/vorbisenc.h>
#include <opus.h>
#include <lame.h>

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

class BaseSpStream: public ISpStream, public ISpEventSink {
public:
	LPCWSTR filename;
	WAVEFORMATEX wfex;
	const GUID *formatId;
	ULONGLONG ullEventInterest;
	BOOL multiplex;
	HANDLE h;
	BOOL isStdout;
	HANDLE eh; // events file handle

	BaseSpStream(BOOL multiplex_): filename(0), wfex{0} , formatId(0), ullEventInterest(0), multiplex(multiplex_), h(0), isStdout(0), eh(0) {}

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
	STDMETHODIMP_(ULONG) AddRef(void) { return 1; }
	STDMETHODIMP_(ULONG) Release(void) { return 1; }
	STDMETHODIMP Read(void *, ULONG, ULONG *) { return 0; }
	STDMETHODIMP Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER *plibNewPosition) {
		if(plibNewPosition)
			plibNewPosition->QuadPart = dlibMove.QuadPart;
		return S_OK;
	}
	STDMETHODIMP SetSize(ULARGE_INTEGER) { return 0; }
	STDMETHODIMP CopyTo(IStream *, ULARGE_INTEGER, ULARGE_INTEGER *, ULARGE_INTEGER *) { return 0; }
	STDMETHODIMP Commit(DWORD) { return 0; }
	STDMETHODIMP Revert(void) { return 0; }
	STDMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) { return 0; }
	STDMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) { return 0; }
	STDMETHODIMP Stat(STATSTG *, DWORD) { return 0; }
	STDMETHODIMP Clone(IStream **) { return 0; }
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

	STDMETHODIMP SetBaseStream(IStream *pStream, REFGUID rguidFormat, const WAVEFORMATEX *pWaveFormatEx) { return S_OK; }

	STDMETHODIMP GetBaseStream(IStream **ppStream) { return S_OK; }

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

		if(ullEventInterest_) {
			if(multiplex) {
				eh = h;
			} else if(isStdout) {
				eh = (HANDLE)_get_osfhandle(3);
			} else {
				fwprintf(stderr, L"Cannot select events (0x%04llx) when output is not stdout and multiplexing is not enabled\n", ullEventInterest_);
				return E_INVALIDARG;
			}
		}

		return S_OK;
	}

	virtual STDMETHODIMP Close(void) {
		if(isStdout || !h) return S_OK;

		BOOL b = CloseHandle(h);
		if(b) return S_OK;

		DWORD e = GetLastError();
		WCHAR buf[MAX_PATH];
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, e, 0, buf, sizeof(buf) / sizeof(buf[0]), 0);
		fwprintf(stderr, L"Could not close \"%s\": %d (%s)", filename, e, buf);
		return HRESULT_FROM_WIN32(e);
	}

	STDMETHODIMP_(BOOL) MuxWrite(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten, BOOL isEventData) {
		if (!multiplex) return WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, 0);
		DWORD muxme = nNumberOfBytesToWrite << 1 | (isEventData ? 0x01 : 0x00);
		CHAR buf[4];
		int bufsize;
		for(bufsize = 0; muxme > 0; muxme >>= 7, bufsize++) {
			buf[bufsize] = muxme & 0x7f;
			if (bufsize > 0) buf[bufsize - 1] |= 0x80;
		}

		BOOL b = WriteFile(hFile, buf, bufsize, lpNumberOfBytesWritten, 0);
		if(!b) return b;
		return WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, 0);
	}

	virtual HRESULT STDMETHODCALLTYPE Write(const void *buf, ULONG size, ULONG *newPos) {
		BOOL r = MuxWrite(h, buf, size, newPos, FALSE);
		if(r) return S_OK;

		DWORD e = GetLastError();
		WCHAR errbuf[MAX_PATH];
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, e, 0, errbuf, sizeof(errbuf) / sizeof(errbuf[0]), 0);
		fwprintf(stderr, L"Could not write audio samples to %s: %d (%s)", isStdout ? L"stdout" : filename, e, errbuf);
		return HRESULT_FROM_WIN32(e);
	}

	virtual STDMETHODIMP writeEventData(void *buf, size_t sz) {
		if(!eh) return E_FAIL;
		BOOL b = MuxWrite(eh, buf, (ULONG)sz, 0, TRUE);
		if(b) return S_OK;

		DWORD e = GetLastError();
		WCHAR errbuf[MAX_PATH];
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, e, 0, errbuf, sizeof(errbuf) / sizeof(errbuf[0]), 0);
		fwprintf(stderr, L"Could not write event data: %d (%s)", e, errbuf);
		return HRESULT_FROM_WIN32(e);
	}
};

class RawSpStream: public BaseSpStream {
public:
	RawSpStream(BOOL multiplex): BaseSpStream(multiplex) {}
};

class OggSpStream: public BaseSpStream {
public:
	ogg_stream_state ogg_voice_st;
	ogg_stream_state ogg_events_st;
	ogg_int64_t granulepos;
	ogg_int64_t packetNo, eventpacketNo;

	OggSpStream(BOOL multiplex) : BaseSpStream(multiplex), ogg_voice_st{0}, ogg_events_st{0}, granulepos(0), packetNo(0), eventpacketNo(0) {}

	virtual STDMETHODIMP BindToFile(LPCWSTR filename_, SPFILEMODE eMode, const GUID *pFormatId, const WAVEFORMATEX *pWaveFormatEx, ULONGLONG ullEventInterest_) {
		if(ogg_stream_init(&ogg_voice_st, 1)) {
			fwprintf(stderr, L"Could not initialize ogg stream\n");
			return E_FAIL;
		}

		if(ogg_stream_init(&ogg_events_st, 2)) {
			fwprintf(stderr, L"Could not initialize ogg stream\n");
			return E_FAIL;
		}

		granulepos = packetNo = eventpacketNo = 0;

		HRESULT hr = BaseSpStream::BindToFile(filename_, eMode, pFormatId, pWaveFormatEx, ullEventInterest_);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not bind to file %s: %d %s\n", filename_, hr, getErrorString(hr));
			return hr;
		}

		return S_OK;
	}

	STDMETHODIMP writeEventHead() {
		if(ullEventInterest == 0) return S_OK;
		unsigned char evntHead[8] = { 'S', 'A', 'P', 'I', 'E', 'V', 'N', 'T' };

		ogg_packet p;
		p.packet = evntHead;
		p.bytes = 8;
		p.b_o_s = 1;
		p.e_o_s = 0;
		p.granulepos = 0;
		p.packetno = eventpacketNo++;
		if(ogg_stream_packetin(&ogg_events_st, &p)) {
			fwprintf(stderr, L"Could not add the header packet to the events stream\n");
			return E_FAIL;
		}

		return flushStream(&ogg_events_st);
	}

	STDMETHODIMP writeEventData(void *buf, size_t sz) {
		if(ullEventInterest == 0) return S_OK;
		if(!multiplex) return BaseSpStream::writeEventData(buf, sz);
		ogg_packet p;
		p.packet = (unsigned char *)buf;
		p.bytes = (ULONG)sz;
		p.e_o_s = 0;
		p.b_o_s = 0;
		p.granulepos = granulepos;
		p.packetno = eventpacketNo++;
		if(ogg_stream_packetin(&ogg_events_st, &p)) {
			fwprintf(stderr, L"Could not add an event data packet of length %lu to the events stream\n", (ULONG)sz);
			return E_FAIL;
		}

		return pageoutStream(&ogg_events_st);
	}

	STDMETHODIMP flushEventStream(void) {
		if(ullEventInterest == 0) return S_OK;
		if(!multiplex) return S_OK;

		ogg_packet p;
		p.packet = 0;
		p.bytes = 0;
		p.b_o_s = 0;
		p.e_o_s = 1;
		p.granulepos = granulepos;
		p.packetno = eventpacketNo++;
		if(ogg_stream_packetin(&ogg_events_st, &p)) {
			fwprintf(stderr, L"Could not add the final packet to the events stream\n");
			return E_FAIL;
		}

		return flushStream(&ogg_events_st);
	}

	virtual STDMETHODIMP Write(const void *buf, ULONG size, ULONG *newPos) = 0;

	HRESULT STDMETHODCALLTYPE Close() {
		flushEventStream();

		if(ogg_stream_clear(&ogg_voice_st)) {
			fwprintf(stderr, L"Could not clear voice stream\n");
			return E_FAIL;
		}
		if(ogg_stream_clear(&ogg_events_st)) {
			fwprintf(stderr, L"Could not clear events stream\n");
			return E_FAIL;
		}

		return BaseSpStream::Close();
	}

	STDMETHODIMP writePage(ogg_page *p) {
		BOOL r = WriteFile(h, p->header, p->header_len, 0, 0);
		if(r) r = WriteFile(h, p->body, p->body_len, 0, 0);
		if(r) return S_OK;

		DWORD e = GetLastError();
		WCHAR buf[MAX_PATH];
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, e, 0, buf, sizeof(buf) / sizeof(buf[0]), 0);
		fwprintf(stderr, L"Could not write to %s: %d (%s)", isStdout ? L"stdout" : filename, e, buf);
		return HRESULT_FROM_WIN32(e);
	}

	STDMETHODIMP pageoutStream(ogg_stream_state *os) {
		ogg_page p;
		while(ogg_stream_pageout(os, &p)) {
			HRESULT hr = writePage(&p);
			if(hr != S_OK) {
				fwprintf(stderr, L"Could not write page: %d %s\n", hr, getErrorString(hr));
				return hr;
			}
		}

		return S_OK;
	}

	STDMETHODIMP flushStream(ogg_stream_state *os) {
		ogg_page p;
		while(ogg_stream_flush(os, &p)) {
			HRESULT hr = writePage(&p);
			if(hr != S_OK) {
				fwprintf(stderr, L"Could not write page: %d %s\n", hr, getErrorString(hr));
				return hr;
			}
		}

		return S_OK;
	}
};

class OggVorbisSpStream: public OggSpStream {
public:
	vorbis_info vi;
	vorbis_comment vc;
	vorbis_dsp_state vd;
	vorbis_block vb;

	OggVorbisSpStream(BOOL multiplex): OggSpStream(multiplex), vi{0}, vc{0}, vd{0}, vb{0} {}

	const WCHAR *getVorbisErrorString(int r) {
		switch(r) {
			case OV_EFAULT: return L"Internal logic fault; indicates a bug or heap / stack corruption.";
			case OV_EINVAL: return L"Invalid setup request, eg, out of range argument.";
			case OV_EIMPL: return L"Unimplemented mode; unable to comply with quality level request.";
		}

		return L"Unknown error";
	}

	HRESULT vorbisToHresult(int r) {
		switch(r) {
			case OV_EFAULT: return E_FAIL;
			case OV_EINVAL: return E_INVALIDARG;
			case OV_EIMPL: return E_NOTIMPL;
		}

		return E_FAIL;
	}

	STDMETHODIMP BindToFile(LPCWSTR filename_, SPFILEMODE eMode, const GUID *pFormatId, const WAVEFORMATEX *pWaveFormatEx, ULONGLONG ullEventInterest_) {
		vorbis_info_init(&vi);
		int r = vorbis_encode_init_vbr(&vi, pWaveFormatEx->nChannels, pWaveFormatEx->nSamplesPerSec, 0.1f);
		if(r) {
			fwprintf(stderr, L"Could not initialize vorbis encoder: %d %s\n", r, getVorbisErrorString(r));
			return vorbisToHresult(r);
		}
		vorbis_comment_init(&vc);
		vorbis_comment_add_tag(&vc, "ENCODER", "sapicli");
		if(vorbis_analysis_init(&vd, &vi)) {
			fwprintf(stderr, L"Could not initialize vorbis encoder's analysis state\n");
			return E_FAIL;
		}
		if(vorbis_block_init(&vd, &vb)) {
			fwprintf(stderr, L"Could not initialize vorbis_block structure\n");
			return E_FAIL;
		}

		HRESULT hr = OggSpStream::BindToFile(filename_, eMode, pFormatId, pWaveFormatEx, ullEventInterest_);
		if(FAILED(hr)) return hr;

		ogg_packet header;
		ogg_packet header_comm;
		ogg_packet header_code;

		r = vorbis_analysis_headerout(&vd, &vc, &header, &header_comm, &header_code);
		if(r) {
			fwprintf(stderr, L"Could not initialize vorbis encoder: %d %s\n", r, getVorbisErrorString(r));
			return vorbisToHresult(r);
		}

		/* automatically placed in its own page */
		if(ogg_stream_packetin(&ogg_voice_st, &header)) {
			fwprintf(stderr, L"Could not add vorbis header packet to voice stream\n");
			return E_FAIL;
		}
		hr = flushStream(&ogg_voice_st);
		if(hr != S_OK) return hr;

		hr = writeEventHead();
		if(hr != S_OK) return hr;

		if(ogg_stream_packetin(&ogg_voice_st, &header_comm)) {
			fwprintf(stderr, L"Could not add vorbis comment header to voice stream\n");
			return E_FAIL;
		}
		if(ogg_stream_packetin(&ogg_voice_st, &header_code)) {
			fwprintf(stderr, L"Could not add vorbis code header to voice stream\n");
			return E_FAIL;
		}
		return flushStream(&ogg_voice_st);
	}

	HRESULT STDMETHODCALLTYPE Write(const void *buf, ULONG size, ULONG *newPos) {
		int r;
		if(size == 0) {
			r = vorbis_analysis_wrote(&vd, 0);
			if(r) {
				fwprintf(stderr, L"Could not set wrote 0 samples on vorbis analyzer: %d %s\n", r, getVorbisErrorString(r));
				return vorbisToHresult(r);
			}
		} else {
			int nSamples = size * 8 / wfex.wBitsPerSample / wfex.nChannels;
			granulepos += nSamples;
			float **buffer = vorbis_analysis_buffer(&vd, nSamples);

			/* Optimized copy for common combination of bit depths and numbers of channels */
			if(wfex.wBitsPerSample == 8 && wfex.nChannels == 1) {
				unsigned char *srcSample = (unsigned char *)buf;
				float *sample0 = buffer[0];
				for(int i = 0; i < nSamples; i++) {
					*(sample0++) = (*(srcSample++) - 128.f) / 128.f;
				}
			} else if(wfex.wBitsPerSample == 8 && wfex.nChannels == 2) {
				unsigned char *srcSample = (unsigned char *)buf;
				float *sample0 = buffer[0];
				float *sample1 = buffer[1];
				for(int i = 0; i < nSamples; i++) {
					*(sample0++) = (*(srcSample++) - 128.f) / 128.f;
					*(sample1++) = (*(srcSample++) - 128.f) / 128.f;
				}
			} else if(wfex.wBitsPerSample == 16 && wfex.nChannels == 1) {
				short *srcSample = (short *)buf;
				float *sample0 = buffer[0];
				for(int i = 0; i < nSamples; i++) {
					*(sample0++) = *(srcSample++) / 32768.f;
				}
			} else if(wfex.wBitsPerSample == 16 && wfex.nChannels == 2) {
				short *srcSample = (short *)buf;
				float *sample0 = buffer[0];
				float *sample1 = buffer[1];
				for(int i = 0; i < nSamples; i++) {
					*(sample0++) = *(srcSample++) / 32768.f;
					*(sample1++) = *(srcSample++) / 32768.f;
				}
			} else {
				/* Generic, rarely used, slow method */
				int bytesPerSample = (wfex.wBitsPerSample + 7) >> 3;
				float divisor = (float)(1 << (wfex.wBitsPerSample - 1));
				char *startSrcSample = (char *)buf;
				int strideSkip = bytesPerSample * wfex.nChannels;
				for(int j = 0; j < wfex.nChannels; j++) {
					float *sample = buffer[j];
					char *srcSample = startSrcSample;
					for(int i = 0; i < nSamples; i++) {
						LONGLONG srcSampleAccum = (srcSample[bytesPerSample - 1] < 0) ? -1 : 0;
						memcpy((void *)&srcSampleAccum, srcSample, bytesPerSample);
						srcSample += strideSkip;
						*(sample++) = (float)srcSampleAccum / divisor;
					}
					startSrcSample += bytesPerSample;
				}
			}

			r = vorbis_analysis_wrote(&vd, nSamples);
			if(r) {
				fwprintf(stderr, L"Could not set wrote %d samples on vorbis analyzer: %d %s\n", nSamples, r, getVorbisErrorString(r));
				return vorbisToHresult(r);
			}
		}

		int eos = 0;
		while(vorbis_analysis_blockout(&vd, &vb) == 1) {
			r = vorbis_analysis(&vb, NULL);
			if(r) {
				fwprintf(stderr, L"Could not run vorbis analysis: %d %s\n", r, getVorbisErrorString(r));
				return vorbisToHresult(r);
			}

			r = vorbis_bitrate_addblock(&vb);
			if(r) {
				fwprintf(stderr, L"Could not submit block to vorbis bitrate management engine: %d %s\n", r, getVorbisErrorString(r));
				return vorbisToHresult(r);
			}

			ogg_packet p;
			while(vorbis_bitrate_flushpacket(&vd, &p)) {
				if(ogg_stream_packetin(&ogg_voice_st, &p)) {
					fwprintf(stderr, L"Could not add vorbis packet to voice stream\n");
					return E_FAIL;
				}

				while(!eos) {
					ogg_page p;
					int result = ogg_stream_pageout(&ogg_voice_st, &p);
					if(result == 0) break;
					HRESULT hr = writePage(&p);
					if(FAILED(hr)) return hr;

					if(ogg_page_eos(&p)) eos = 1;
				}
			}
		}

		if(newPos) *newPos += size;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE Close() {
		Write(0, 0, 0);
		vorbis_block_clear(&vb);
		vorbis_dsp_clear(&vd);
		vorbis_comment_clear(&vc);
		vorbis_info_clear(&vi);
		return OggSpStream::Close();
	}
};

class OggOpusSpStream: public OggSpStream {
public:
	OpusEncoder *enc;
	opus_int16 frame[2880 * 2]; // max frame size times two channels
	int framepos;
	int framesize;

	OggOpusSpStream(BOOL multiplex): OggSpStream(multiplex), enc(0), frame{0}, framepos(0), framesize(960) {}

	const WCHAR *getOpusErrorString(int err) {
		switch(err) {
			case OPUS_OK: return L"No error";
			case OPUS_BAD_ARG: return L"One or more invalid / out of range arguments.";
			case OPUS_BUFFER_TOO_SMALL: return L"Not enough bytes allocated in the buffer.";
			case OPUS_INTERNAL_ERROR: return L"An internal error was detected.";
			case OPUS_INVALID_PACKET: return L"The compressed data passed is corrupted.";
			case OPUS_UNIMPLEMENTED: return L"Invalid / unsupported request number.";
			case OPUS_INVALID_STATE: return L"An encoder or decoder structure is invalid or already freed.";
			case OPUS_ALLOC_FAIL: return L"Memory allocation has failed.";
		}

		return L"Unknown error";
	}

	HRESULT opusToHresult(int err) {
		switch(err) {
			case OPUS_OK: return S_OK;
			case OPUS_BAD_ARG: return E_INVALIDARG;
			case OPUS_BUFFER_TOO_SMALL: return E_NOT_SUFFICIENT_BUFFER;
			case OPUS_INTERNAL_ERROR: return E_FAIL;
			case OPUS_INVALID_PACKET: return E_INVALID_PROTOCOL_FORMAT;
			case OPUS_UNIMPLEMENTED: return E_NOTIMPL;
			case OPUS_INVALID_STATE: return E_FAIL;
			case OPUS_ALLOC_FAIL: return E_FAIL;
		}

		return E_FAIL;
	}

	STDMETHODIMP BindToFile(LPCWSTR filename_, SPFILEMODE eMode, const GUID *pFormatId, const WAVEFORMATEX *pWaveFormatEx, ULONGLONG ullEventInterest_) {
		int err;
		if(pWaveFormatEx->wBitsPerSample != 16 && pWaveFormatEx->wBitsPerSample != 8) {
			fwprintf(stderr, L"Only 8 and 16 bit depth is supported for opus\n");
			return E_INVALIDARG;
		}
		if(pWaveFormatEx->nChannels != 1 && pWaveFormatEx->nChannels != 2) {
			fwprintf(stderr, L"Only 1 or 2 channels are supported for opus\n");
			return E_INVALIDARG;
		}
		enc = opus_encoder_create(pWaveFormatEx->nSamplesPerSec, pWaveFormatEx->nChannels, OPUS_APPLICATION_VOIP, &err);
		if(err != OPUS_OK) {
			fwprintf(stderr, L"Error creating encoder: %d %s\n", err, getOpusErrorString(err));
			return opusToHresult(err);
		}

		// open file only after some sanity checks above
		HRESULT hr = OggSpStream::BindToFile(filename_, eMode, pFormatId, pWaveFormatEx, ullEventInterest_);
		if(FAILED(hr)) return hr;

		opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
		opus_encoder_ctl(enc, OPUS_SET_BITRATE(10000));
		framesize = pWaveFormatEx->nSamplesPerSec * 20 / 1000;
		ogg_packet header;
		int lookahead = 3840;
		opus_encoder_ctl(enc, OPUS_GET_LOOKAHEAD(&lookahead));
		unsigned char opusHeader[19] = {
			'O', 'p', 'u', 's',
			'H', 'e', 'a', 'd',
			1,
			(unsigned char)pWaveFormatEx->nChannels,
			(unsigned char)(lookahead >> 0),
			(unsigned char)(lookahead >> 8),
			(unsigned char)(pWaveFormatEx->nSamplesPerSec >> 0),
			(unsigned char)(pWaveFormatEx->nSamplesPerSec >> 8),
			(unsigned char)(pWaveFormatEx->nSamplesPerSec >> 16),
			(unsigned char)(pWaveFormatEx->nSamplesPerSec >> 24),
			0x00, 0x00,
			0
		};
		header.packet = opusHeader;
		header.bytes = sizeof(opusHeader);
		header.b_o_s = 1;
		header.e_o_s = 0;
		header.granulepos = 0;
		header.packetno = packetNo++;
		if(ogg_stream_packetin(&ogg_voice_st, &header)) {
			fwprintf(stderr, L"Could not add OpusHead packet to voice stream\n");
			return E_FAIL;
		}
		hr = flushStream(&ogg_voice_st);
		if(FAILED(hr)) return hr;

		hr = writeEventHead();
		if(FAILED(hr)) return hr;

		unsigned char opusTags[42] = {
			'O', 'p', 'u', 's',
			'T', 'a', 'g', 's',
			7, 0, 0, 0,
			's', 'a', 'p', 'i', 'c', 'l', 'i',
			1, 0, 0, 0,
			15, 0, 0, 0,
			'E', 'N', 'C', 'O', 'D', 'E', 'R', '=',
			's', 'a', 'p', 'i', 'c', 'l', 'i'
		};
		header.packet = opusTags;
		header.bytes = sizeof(opusTags);
		header.b_o_s = 0;
		header.e_o_s = 0;
		header.granulepos = 0;
		header.packetno = packetNo++;
		if(ogg_stream_packetin(&ogg_voice_st, &header)) {
			fwprintf(stderr, L"Could not add OpusTags packet to voice stream\n");
			return E_FAIL;
		}
		return flushStream(&ogg_voice_st);
	}

	STDMETHODIMP Write(const void *buf, ULONG size, ULONG *newPos) {
		int nSamples = size * 8 / wfex.wBitsPerSample / wfex.nChannels;
		unsigned char encbuf[4096];
		if(wfex.wBitsPerSample != 16 && wfex.wBitsPerSample != 8 || wfex.nChannels != 1 && wfex.nChannels != 2)
			return E_INVALIDARG;
		short *samples16 = (short *)buf;
		unsigned char *samples8 = (unsigned char *)buf;
		opus_int16 *frameptr = &frame[framepos * wfex.nChannels];
		for(int x = 0; x < nSamples; x++) {
			if(wfex.wBitsPerSample == 16) {
				*(frameptr++) = *(samples16++);
				if(wfex.nChannels == 2)
					*(frameptr++) = *(samples16++);
			} else {
				*(frameptr++) = (*samples8 - 128) << 8 | *(samples8++);
				if(wfex.nChannels == 2)
					*(frameptr++) = (*samples8 - 128) << 8 | *(samples8++);
			}

			granulepos++;

			framepos++;
			if(framepos < framesize)
				continue;

			framepos = 0;
			frameptr = frame;

			int encoded = opus_encode(enc, frame, framesize, encbuf, sizeof(encbuf));
			if(encoded < 0) {
				fwprintf(stderr, L"Could not encode %d samples of opus data %d %s\n", framesize, encoded, getOpusErrorString(encoded));
				return opusToHresult(encoded);
			}

			if(encoded <= 2)
				continue;

			ogg_packet p;
			p.packet = encbuf;
			p.bytes = encoded;
			p.b_o_s = p.e_o_s = 0;
			p.granulepos = granulepos * 48000 / wfex.nSamplesPerSec;
			p.packetno = packetNo++;
			if(ogg_stream_packetin(&ogg_voice_st, &p)) {
				fwprintf(stderr, L"Could not write opus voice packet of length %d to ogg stream\n", p.bytes);
				return E_FAIL;
			}

			HRESULT hr;
			if(granulepos % (framesize * 50))
				hr = pageoutStream(&ogg_voice_st);
			else
				hr = flushStream(&ogg_voice_st);
			if(hr != S_OK) return hr;
		}

		if(newPos) *newPos += size;

		return S_OK;
	}

	STDMETHODIMP Close() {
		unsigned char encbuf[4096];
		memset(frame + framepos * wfex.nChannels, 0, (framesize - framepos) * wfex.nChannels);
		int encoded = opus_encode(enc, frame, framesize, encbuf, sizeof(encbuf));
		if(encoded < 0) {
			fwprintf(stderr, L"Could not encode final %d (%d) samples of opus data %d %s\n", framesize, framepos, encoded, getOpusErrorString(encoded));
			return E_FAIL;
		}

		ogg_packet p;
		p.packet = encbuf;
		p.bytes = encoded > 2 ? encoded : 0;
		p.b_o_s = 0;
		p.e_o_s = 1;
		granulepos += framepos;
		p.granulepos = granulepos * 48000 / wfex.nSamplesPerSec;
		p.packetno = packetNo++;
		if(ogg_stream_packetin(&ogg_voice_st, &p)) {
			fwprintf(stderr, L"Could not add final packet to voice stream\n");
			return E_FAIL;
		}
		HRESULT hr = flushStream(&ogg_voice_st);
		if(hr != S_OK) return hr;

		opus_encoder_destroy(enc);
		enc = 0;

		return OggSpStream::Close();
	}
};

class Mp3SpStream: public BaseSpStream {
public:
	lame_global_flags *gfp;
	unsigned char encodebuf[16384];

	Mp3SpStream(BOOL multiplex): BaseSpStream(multiplex), gfp(0), encodebuf{0} {}

	virtual STDMETHODIMP BindToFile(LPCWSTR filename_, SPFILEMODE eMode, const GUID *pFormatId, const WAVEFORMATEX *pWaveFormatEx, ULONGLONG ullEventInterest_) {
		if(pWaveFormatEx->wBitsPerSample != 16) {
			fwprintf(stderr, L"Only 16 bit samples are supported for mp3, got %d\n", pWaveFormatEx->wBitsPerSample);
			return E_INVALIDARG;
		}
		gfp = lame_init();
		if(!gfp) {
			fwprintf(stderr, L"Could not init lame encoder\n");
			return E_OUTOFMEMORY;
		}
		if(lame_set_num_channels(gfp, pWaveFormatEx->nChannels)) {
			fwprintf(stderr, L"Could not set lame encoder number of channels to %d\n", pWaveFormatEx->nChannels);
			return E_INVALIDARG;
		}
		if(lame_set_in_samplerate(gfp, pWaveFormatEx->nSamplesPerSec)) {
			fwprintf(stderr, L"Could not set lame encoder sample rate to %d\n", pWaveFormatEx->nSamplesPerSec);
			return E_INVALIDARG;
		}
		if(lame_set_brate(gfp, 128)) {
			fwprintf(stderr, L"Could not set lame encoder bit rate to %d\n", 128);
			return E_INVALIDARG;
		}
		if(lame_set_mode(gfp, pWaveFormatEx->nChannels == 2 ? STEREO : MONO)) {
			fwprintf(stderr, L"Could not set lame encoder mode\n");
			return E_INVALIDARG;
		}
		/* 2=high  5 = medium  7=low */
		if(lame_set_quality(gfp, 5)) {
			fwprintf(stderr, L"Could not set lame encoder quality\n");
			return E_INVALIDARG;
		}
		if(lame_set_bWriteVbrTag(gfp, 0)) {
			fwprintf(stderr, L"Could not disable writing VBR tag\n");
			return E_INVALIDARG;
		}
		lame_mp3_tags_fid(gfp, 0);
		if(lame_init_params(gfp)) {
			fwprintf(stderr, L"Could not init lame params\n");
			return E_FAIL;
		}

		return BaseSpStream::BindToFile(filename_, eMode, pFormatId, pWaveFormatEx, ullEventInterest_);
	}

	STDMETHODIMP Close() {
		int r = lame_encode_flush(gfp, encodebuf, sizeof(encodebuf));
		if(r < 0) return E_FAIL;
		if(r == 0) return S_OK;
		if(MuxWrite(h, encodebuf, r, 0, FALSE)) return S_OK;

		DWORD e = GetLastError();
		WCHAR errbuf[MAX_PATH];
		FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, e, 0, errbuf, sizeof(errbuf) / sizeof(errbuf[0]), 0);
		fwprintf(stderr, L"Could not write audio samples to %s: %d (%s)", isStdout ? L"stdout" : filename, e, errbuf);
		return HRESULT_FROM_WIN32(e);

		lame_close(gfp);
		return BaseSpStream::Close();
	}

	HRESULT STDMETHODCALLTYPE Write(const void *buf, ULONG size, ULONG *newPos) {
		if(size >= 16777216) {
			return E_INVALIDARG;
		}
		ULONG nSamples = size * 8 / wfex.wBitsPerSample / wfex.nChannels;
		ULONG framesize = 1152;
		for(ULONG s = 0; s < nSamples; s += framesize) {
			ULONG samples = nSamples - s;
			if(samples > framesize) samples = framesize;
			int r;
			if(wfex.nChannels > 1)
				r = lame_encode_buffer_interleaved(gfp, ((short *)buf) + s * wfex.nChannels, samples, encodebuf, sizeof(encodebuf));
			else
				r = lame_encode_buffer(gfp, ((short *)buf) + s, 0, samples, encodebuf, sizeof(encodebuf));
			if(r < 0) return E_FAIL;

			if(r == 0) continue;
			if(MuxWrite(h, encodebuf, r, newPos, FALSE)) continue;

			DWORD e = GetLastError();
			WCHAR errbuf[MAX_PATH];
			FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, e, 0, errbuf, sizeof(errbuf) / sizeof(errbuf[0]), 0);
			fwprintf(stderr, L"Could not write audio samples to %s: %d (%s)", isStdout ? L"stdout" : filename, e, errbuf);
			return HRESULT_FROM_WIN32(e);
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
	if(outType == 1) {
		outputStream = new RawSpStream(multiplex);
	} else if(outType == 2) {
		HRESULT hr = ::CoCreateInstance(CLSID_SpStream, NULL, CLSCTX_ALL, __uuidof(outputStream), (void **)&outputStream);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not instantiate SpStream: %d %s\n", hr, getErrorString(hr));
			return 1;
		}
	} else if(outType == 3) {
		outputStream = new OggVorbisSpStream(multiplex);
	} else if(outType == 4) {
		outputStream = new OggOpusSpStream(multiplex);
	} else if(outType == 5) {
		outputStream = new Mp3SpStream(multiplex);
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
		fwprintf(stderr, L"Could not speak: %d %s\n", hr, getErrorString(hr));
		return 1;
	}

	// Release here so the destructor doesn't do it after we've closed the output file
	voice.Release();

	hr = outputStream->Close();
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not close %s: %d %s\n", wavFilename, hr, getErrorString(hr));
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
