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
#include <msxml6.h>

extern "C" {
#include <mux.h>
}

#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "getoptw.h"

// mux_sink_fn thunk: writes muxed encoder output to the Windows HANDLE passed
// through the sink_user pointer. Returns non-zero on WriteFile failure so
// mux_encoder_encode()/finalize() propagate it back up.
static int mux_sink_write(void *user, const void *data, size_t size) {
	HANDLE h = static_cast<HANDLE>(user);
	const char *p = static_cast<const char*>(data);
	while(size > 0) {
		DWORD written = 0;
		DWORD chunk = size > (DWORD)-1 ? (DWORD)-1 : (DWORD)size;
		if(!WriteFile(h, p, chunk, &written, NULL) || written == 0) {
			fwprintf(stderr, L"WriteFile failed in mux sink: %lu\n", GetLastError());
			return 1;
		}
		p += written;
		size -= written;
	}
	return 0;
}

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

// ---------------------------------------------------------------------------
// SAPI helpers
// ---------------------------------------------------------------------------

namespace {

WAVEFORMATEX make_wfex(DWORD sample_rate, WORD channels, WORD bits_per_sample) {
	WAVEFORMATEX w{};
	w.wFormatTag = WAVE_FORMAT_PCM;
	w.nChannels = channels;
	w.nSamplesPerSec = sample_rate;
	w.wBitsPerSample = bits_per_sample;
	w.nBlockAlign = w.nChannels * w.wBitsPerSample / 8;
	w.nAvgBytesPerSec = w.nSamplesPerSec * w.nBlockAlign;
	w.cbSize = 0;
	return w;
}

// SAPI output stream that routes Write() to an audio callback and AddEvents()
// to an event callback. SAPI QueryInterface()'s the output stream for
// ISpEventSink and uses it for events when the interest mask is non-zero.
// Stack-allocated by speakToWav(); AddRef/Release are no-ops.
class SynthSink final : public ISpStream, public ISpEventSink {
public:
	using ByteSink = std::function<void(const void*, std::size_t)>;

	SynthSink(const WAVEFORMATEX& wfex, ULONGLONG event_interest,
	          ByteSink audio_sink, ByteSink event_sink)
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
	ByteSink audio_sink_;
	ByteSink event_sink_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Voice enumeration → JSON (SpEnumTokens fields piped straight to stdout;
// no intermediate struct).
// ---------------------------------------------------------------------------
int listVoices() {
	CComPtr<IEnumSpObjectTokens> voicesEnum;
	HRESULT hr = SpEnumTokens(SPCAT_VOICES, NULL, NULL, &voicesEnum);
	if(FAILED(hr)) {
		fwprintf(stderr, L"SpEnumTokens failed: 0x%08x\n", (unsigned)hr);
		return 1;
	}

	ULONG count = 0;
	voicesEnum->GetCount(&count);

	// Print a JSON pair from a CoTaskMemAlloc'd string, then free it.
	auto print_free = [](const WCHAR* key, WCHAR*& value, int last = 0) {
		printJsonKeyPair(key, value ? value : L"", last);
		if(value) { CoTaskMemFree(value); value = nullptr; }
	};

	wprintf(L"[\n");
	for(ULONG i = 0; i < count; i++) {
		CComPtr<ISpObjectToken> token;
		hr = voicesEnum->Next(1, &token, NULL);
		if(FAILED(hr)) break;

		wprintf(L"{\n");

		// id: basename of the registry token (e.g. TTS_MS_EN-US_DAVID_11.0).
		WCHAR* idFull = nullptr;
		token->GetId(&idFull);
		WCHAR* basename = idFull ? wcsrchr(idFull, L'\\') : nullptr;
		const WCHAR* id = (basename && basename[1]) ? basename + 1
		                 : (idFull ? idFull : L"");
		printJsonKeyPair(L"id", id);
		if(idFull) CoTaskMemFree(idFull);

		WCHAR* desc = nullptr;
		SpGetDescription(token, &desc);
		print_free(L"description", desc);

		WCHAR *age = nullptr, *gender = nullptr, *lang = nullptr;
		WCHAR *name = nullptr, *vendor = nullptr;
		WCHAR locale[LOCALE_NAME_MAX_LENGTH] = {0};
		CComPtr<ISpDataKey> attrs;
		if(SUCCEEDED(token->OpenKey(L"Attributes", &attrs))) {
			attrs->GetStringValue(L"Age", &age);
			attrs->GetStringValue(L"Gender", &gender);
			if(SUCCEEDED(attrs->GetStringValue(L"Language", &lang))) {
				// SAPI stores Language as a hex LANGID string ("409" = en-US);
				// prefer the BCP-47 locale name, fall back to the hex if it
				// doesn't resolve.
				int langId = wcstol(lang, nullptr, 16);
				LCIDToLocaleName(langId, locale, LOCALE_NAME_MAX_LENGTH, 0);
			}
			attrs->GetStringValue(L"Name", &name);
			attrs->GetStringValue(L"Vendor", &vendor);
		}
		print_free(L"age", age);
		print_free(L"gender", gender);
		printJsonKeyPair(L"language", locale[0] ? locale : (lang ? lang : L""));
		if(lang) CoTaskMemFree(lang);
		print_free(L"name", name);
		print_free(L"vendor", vendor, 1);

		wprintf(i + 1 < count ? L"},\n" : L"}\n");
	}
	wprintf(L"]\n");
	return 0;
}

// Print codec info (name, description, supported sample rates, encoder
// params) as JSON to stdout — a straight passthrough of muxaudio's own
// introspection tables.
int listCodecs() {
	const mux_codec_info* codecs = nullptr;
	int count = 0;
	if(mux_list_codecs(&codecs, &count) != MUX_OK || !codecs) {
		fwprintf(stderr, L"mux_list_codecs failed\n");
		return 1;
	}

	wprintf(L"[\n");
	for(int i = 0; i < count; i++) {
		wprintf(L"{\n");
		wprintf(L"\"name\": \"%hs\",\n", codecs[i].name);
		wprintf(L"\"description\": \"%hs\",\n", codecs[i].description);

		mux_sample_rate_list rates{};
		wprintf(L"\"sample_rates\": {");
		if(mux_get_supported_sample_rates(codecs[i].type, &rates) == MUX_OK) {
			wprintf(L"\"is_range\": %hs, \"values\": [", rates.is_range ? "true" : "false");
			for(int j = 0; j < rates.count; j++) {
				if(j) wprintf(L", ");
				wprintf(L"%d", rates.rates[j]);
			}
			wprintf(L"]");
		} else {
			wprintf(L"\"is_range\": false, \"values\": []");
		}
		wprintf(L"},\n");

		const mux_param_desc* pd = nullptr;
		int pd_count = 0;
		wprintf(L"\"params\": [");
		if(mux_get_encoder_params(codecs[i].type, &pd, &pd_count) == MUX_OK && pd) {
			for(int j = 0; j < pd_count; j++) {
				if(j) wprintf(L", ");
				wprintf(L"{\"name\": \"%hs\", \"description\": \"%hs\"", pd[j].name, pd[j].description);
				switch(pd[j].type) {
					case MUX_PARAM_TYPE_INT:
						wprintf(L", \"type\": \"int\", \"min\": %d, \"max\": %d, \"default\": %d",
						        pd[j].range.i.min, pd[j].range.i.max, pd[j].range.i.def);
						break;
					case MUX_PARAM_TYPE_FLOAT:
						wprintf(L", \"type\": \"float\", \"min\": %g, \"max\": %g, \"default\": %g",
						        pd[j].range.f.min, pd[j].range.f.max, pd[j].range.f.def);
						break;
					case MUX_PARAM_TYPE_BOOL:
						wprintf(L", \"type\": \"bool\", \"default\": %hs",
						        pd[j].range.b.def ? "true" : "false");
						break;
					case MUX_PARAM_TYPE_STRING:
						wprintf(L", \"type\": \"string\", \"default\": \"%hs\"",
						        pd[j].range.s.def ? pd[j].range.s.def : "");
						break;
				}
				wprintf(L"}");
			}
		}
		wprintf(L"]\n");
		wprintf(i + 1 < count ? L"},\n" : L"}\n");
	}
	wprintf(L"]\n");
	return 0;
}

// ---------------------------------------------------------------------------
// Lexemes: load custom SAPI pronunciations from a W3C PLS file
// (https://www.w3.org/TR/pronunciation-lexicon/), like:
//
//   <?xml version="1.0" encoding="UTF-8"?>
//   <lexicon version="1.0"
//            xmlns="http://www.w3.org/2005/01/pronunciation-lexicon"
//            alphabet="x-microsoft-sapi" xml:lang="en-US">
//     <lexeme>
//       <grapheme>zapfluk</grapheme>
//       <phoneme>p iy t z ax</phoneme>
//     </lexeme>
//   </lexicon>
//
// * The <lexicon> `xml:lang` attribute is the default language for lexemes;
//   individual <lexeme>s can override it with their own xml:lang.
// * Only alphabet="x-microsoft-sapi" is understood (phonemes get fed to
//   ISpPhoneConverter::PhoneToId as-is). IPA and other alphabets would need
//   an alphabet mapping we don't have.
// * Multiple <grapheme>s per <lexeme> = aliases -- each grapheme gets the
//   same pronunciation. If a lexeme has multiple <phoneme>s (PLS allows
//   pronunciation variants) we take the first, because SAPI's
//   ISpLexicon::AddPronunciation only accepts one per (word, part-of-speech).
//
// AddPronunciation writes to the user's compound lexicon, which is
// registry-backed and persistent -- entries survive across sapicli runs
// and across reboots, and are visible to every SAPI 5 consumer on this
// machine.
// ---------------------------------------------------------------------------

struct Lexeme {
	LANGID       langId;
	std::wstring word;
	std::wstring phone;   // space-separated SAPI phoneme tokens
};

// "en-US" -> LANGID via LocaleNameToLCID. "0x0409" -> LANGID via strtoul.
// Returns 0 on failure.
static LANGID parseLang(const WCHAR* s) {
	if(!s || !*s) return 0;
	if(s[0] == L'0' && (s[1] == L'x' || s[1] == L'X')) {
		return (LANGID)wcstoul(s, nullptr, 16);
	}
	LCID lcid = LocaleNameToLCID(s, 0);
	if(lcid == 0) return 0;
	return LANGIDFROMLCID(lcid);
}

// sapicli.exe's directory + "\lexemes.pls". Used when no --lexemes flag was
// given; missing file at that path means silently no lexemes are loaded.
static std::wstring defaultLexemesPath() {
	WCHAR path[MAX_PATH];
	DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
	if(n == 0 || n == MAX_PATH) return L"";
	WCHAR* slash = wcsrchr(path, L'\\');
	if(!slash) return L"";
	*(slash + 1) = 0;
	std::wstring out = path;
	out += L"lexemes.pls";
	return out;
}

// Trim leading + trailing whitespace (space, tab, CR, LF) in-place on a
// wstring. PLS DOM `get_text()` includes intra-element whitespace and
// newlines, which SAPI would otherwise treat as part of a phoneme token.
static void trim(std::wstring& s) {
	size_t b = s.find_first_not_of(L" \t\r\n");
	size_t e = s.find_last_not_of(L" \t\r\n");
	if(b == std::wstring::npos) { s.clear(); return; }
	s = s.substr(b, e - b + 1);
}

// Look up an attribute on an element; returns empty wstring if missing.
static std::wstring getAttr(IXMLDOMElement* el, const WCHAR* name) {
	if(!el) return L"";
	CComVariant v;
	if(FAILED(el->getAttribute(CComBSTR(name), &v)) || v.vt != VT_BSTR) return L"";
	return std::wstring(v.bstrVal ? v.bstrVal : L"");
}

// Parse a PLS file into a flat Lexeme list. Errors go to stderr; a totally
// unreadable file returns empty.
static std::vector<Lexeme> loadLexemesPls(const std::wstring& path) {
	std::vector<Lexeme> out;

	CComPtr<IXMLDOMDocument2> doc;
	HRESULT hr = doc.CoCreateInstance(__uuidof(DOMDocument60));
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not create MSXML DOM: 0x%08x\n", (unsigned)hr);
		return out;
	}
	doc->put_async(VARIANT_FALSE);
	doc->put_validateOnParse(VARIANT_FALSE);
	doc->put_resolveExternals(VARIANT_FALSE);

	// Register the PLS namespace prefix so `//pls:lexeme` XPath queries work
	// against files that declare the default xmlns as PLS (i.e. every
	// well-formed PLS file).
	doc->setProperty(CComBSTR(L"SelectionNamespaces"),
	                 CComVariant(L"xmlns:pls='http://www.w3.org/2005/01/pronunciation-lexicon'"));

	VARIANT_BOOL loaded = VARIANT_FALSE;
	hr = doc->load(CComVariant(path.c_str()), &loaded);
	if(FAILED(hr) || !loaded) {
		CComPtr<IXMLDOMParseError> perr;
		doc->get_parseError(&perr);
		long code = 0;
		CComBSTR reason;
		if(perr) { perr->get_errorCode(&code); perr->get_reason(&reason); }
		fwprintf(stderr, L"%s: PLS parse failed (code %ld) %s\n", path.c_str(),
		        code, reason ? (WCHAR*)reason : L"");
		return out;
	}

	CComPtr<IXMLDOMElement> root;
	doc->get_documentElement(&root);
	if(!root) return out;

	// Alphabet: we can only feed SAPI phonemes through PhoneToId.
	std::wstring alphabet = getAttr(root, L"alphabet");
	if(!alphabet.empty() && alphabet != L"x-microsoft-sapi") {
		fwprintf(stderr, L"%s: alphabet='%s' is unsupported; only 'x-microsoft-sapi' "
		                 L"is understood (see SAPI phoneme tables).\n",
		        path.c_str(), alphabet.c_str());
		return out;
	}

	LANGID rootLang = parseLang(getAttr(root, L"xml:lang").c_str());

	CComPtr<IXMLDOMNodeList> lexemes;
	if(FAILED(doc->selectNodes(CComBSTR(L"//pls:lexeme"), &lexemes)) || !lexemes) return out;
	long nlex = 0;
	lexemes->get_length(&nlex);

	for(long i = 0; i < nlex; i++) {
		CComPtr<IXMLDOMNode> node;
		lexemes->get_item(i, &node);
		CComQIPtr<IXMLDOMElement> el(node);
		if(!el) continue;

		LANGID lang = rootLang;
		LANGID overrideLang = parseLang(getAttr(el, L"xml:lang").c_str());
		if(overrideLang) lang = overrideLang;
		if(lang == 0) {
			fwprintf(stderr, L"%s: lexeme #%ld has no xml:lang and no root default\n",
			        path.c_str(), i + 1);
			continue;
		}

		// PLS allows several <phoneme>s per <lexeme> as pronunciation variants;
		// SAPI's AddPronunciation takes one, so pick the first.
		CComPtr<IXMLDOMNode> phonemeNode;
		el->selectSingleNode(CComBSTR(L".//pls:phoneme"), &phonemeNode);
		if(!phonemeNode) {
			fwprintf(stderr, L"%s: lexeme #%ld has no <phoneme>\n", path.c_str(), i + 1);
			continue;
		}
		CComBSTR phoneBstr;
		phonemeNode->get_text(&phoneBstr);
		std::wstring phone = phoneBstr ? (WCHAR*)phoneBstr : L"";
		trim(phone);
		if(phone.empty()) {
			fwprintf(stderr, L"%s: lexeme #%ld has empty <phoneme>\n", path.c_str(), i + 1);
			continue;
		}

		// Multiple <grapheme>s = aliases; each maps to the same phoneme.
		CComPtr<IXMLDOMNodeList> graphemes;
		el->selectNodes(CComBSTR(L".//pls:grapheme"), &graphemes);
		long ng = 0;
		if(graphemes) graphemes->get_length(&ng);
		if(ng == 0) {
			fwprintf(stderr, L"%s: lexeme #%ld has no <grapheme>\n", path.c_str(), i + 1);
			continue;
		}
		for(long g = 0; g < ng; g++) {
			CComPtr<IXMLDOMNode> gnode;
			graphemes->get_item(g, &gnode);
			CComBSTR wordBstr;
			gnode->get_text(&wordBstr);
			std::wstring word = wordBstr ? (WCHAR*)wordBstr : L"";
			trim(word);
			if(word.empty()) continue;
			out.push_back({ lang, word, phone });
		}
	}

	return out;
}

// Load lexemes and register them with the SAPI compound (user) lexicon.
// If explicitPath is empty, use the default (lexemes.txt next to sapicli.exe)
// and treat a missing file as "no lexemes"; if it's set, a missing file is
// an error.
int addLexemes(const std::wstring& explicitPath) {
	std::wstring path = explicitPath.empty() ? defaultLexemesPath() : explicitPath;
	if(path.empty()) return 0;

	if(_waccess(path.c_str(), 0) != 0) {
		if(!explicitPath.empty()) {
			fwprintf(stderr, L"Lexeme file not found: %s\n", path.c_str());
			return 1;
		}
		return 0;   // default path: silent no-op if user didn't ship the file
	}

	auto lexemes = loadLexemesPls(path);
	if(lexemes.empty()) return 0;

	CComPtr<ISpLexicon> cpLexicon;
	HRESULT hr = cpLexicon.CoCreateInstance(CLSID_SpLexicon);
	if(FAILED(hr)) {
		fwprintf(stderr, L"Could not instantiate lexicon: %d %s\n", hr, getErrorString(hr));
		return 1;
	}

	// Phone converters are per-language; cache them so a file with many
	// entries in the same language doesn't recreate one per line.
	std::map<LANGID, CComPtr<ISpPhoneConverter>> converters;

	for(const auto& lex : lexemes) {
		auto& conv = converters[lex.langId];
		if(!conv) {
			hr = SpCreatePhoneConverter(lex.langId, NULL, NULL, &conv);
			if(FAILED(hr)) {
				fwprintf(stderr, L"Could not create phoneme converter for lang 0x%04x: %d %s\n",
				        lex.langId, hr, getErrorString(hr));
				continue;
			}
		}

		SPPHONEID wszId[SP_MAX_PRON_LENGTH];
		hr = conv->PhoneToId(lex.phone.c_str(), wszId);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not convert phoneme '%s' -> id (0x%08x)\n",
			        lex.phone.c_str(), (unsigned)hr);
			continue;
		}

		hr = cpLexicon->AddPronunciation(lex.word.c_str(), lex.langId, SPPS_Noun, wszId);
		if(FAILED(hr)) {
			fwprintf(stderr, L"Could not add pronunciation for '%s': %d %s\n",
			        lex.word.c_str(), hr, getErrorString(hr));
			continue;
		}
	}

	return 0;
}

int speakToWav(WCHAR *text, WCHAR *voiceId, WCHAR *wavFilename, DWORD outType, int rate, int volume, DWORD speakFlags, DWORD samplesPerSec, WORD bitsPerSample, WORD nChannels, ULONGLONG ullEventInterest, BOOL multiplex, const std::vector<std::wstring>& codecParams, const std::wstring& lexemesPath) {
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
				if(!_wcsicmp(wavFilename + s - 4, L".wav"))      outType = 2;
				else if(!_wcsicmp(wavFilename + s - 4, L".ogg")) outType = 3;
				else if(!_wcsicmp(wavFilename + s - 4, L".mp3")) outType = 5;
			}
		}
	}

	if(addLexemes(lexemesPath)) return 1;

	bool isStdout = wavFilename && wavFilename[0] == L'-' && wavFilename[1] == 0;

	// Resolve outType → mux codec (except 1=raw and 2=wav which bypass muxaudio).
	mux_codec_type codec = MUX_CODEC_PCM;
	if(outType == 3)      codec = MUX_CODEC_VORBIS;
	else if(outType == 4) codec = MUX_CODEC_OPUS;
	else if(outType == 5) codec = MUX_CODEC_MP3;

	// For encoded outputs, refuse a sample rate the codec doesn't accept
	// instead of silently rewriting it — makes voice/rate mismatches loud.
	if(outType >= 3 && outType <= 5) {
		if(mux_sample_rate_supported(codec, (int)samplesPerSec) != MUX_OK) {
			fwprintf(stderr, L"Sample rate %lu Hz is not supported by codec '%hs'. "
			                 L"See --list-codecs for accepted rates.\n",
			        samplesPerSec, mux_codec_to_name(codec));
			return 1;
		}
	}

	try {
		// SAPI voice — one ISpVoice bound to this thread's COM apartment.
		CComPtr<ISpVoice> voice;
		HRESULT hr = voice.CoCreateInstance(CLSID_SpVoice);
		if(FAILED(hr)) {
			fwprintf(stderr, L"CoCreateInstance(SpVoice) failed: 0x%08x\n", (unsigned)hr);
			return 1;
		}
		if(voiceId && voiceId[0]) {
			WCHAR full[MAX_PATH];
			_snwprintf_s(full, MAX_PATH, _TRUNCATE,
			             L"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\%s",
			             voiceId);
			CComPtr<ISpObjectToken> token;
			hr = SpGetTokenFromId(full, &token);
			if(FAILED(hr)) throw std::runtime_error("SpGetTokenFromId failed");
			hr = voice->SetVoice(token);
			if(FAILED(hr)) throw std::runtime_error("ISpVoice::SetVoice failed");
		}
		voice->SetRate(rate);
		voice->SetVolume((USHORT)volume);

		WAVEFORMATEX wfex = make_wfex(samplesPerSec, nChannels, bitsPerSample);

		if(outType == 2) {
			// WAV: SAPI native ISpStream writes RIFF + EVNT chunks itself.
			CComPtr<ISpStream> stream;
			hr = ::CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&stream));
			if(FAILED(hr)) throw std::runtime_error("CoCreateInstance(SpStream) failed");
			hr = stream->BindToFile(wavFilename, SPFM_CREATE_ALWAYS,
			                        &SPDFID_WaveFormatEx, &wfex, ullEventInterest);
			if(FAILED(hr)) throw std::runtime_error("ISpStream::BindToFile failed");
			hr = voice->SetOutput(stream, FALSE);
			if(FAILED(hr)) throw std::runtime_error("ISpVoice::SetOutput failed");
			hr = voice->Speak(text, speakFlags, nullptr);
			voice->SetOutput(nullptr, FALSE);
			stream->Close();
			if(FAILED(hr)) throw std::runtime_error("ISpVoice::Speak failed");
			return 0;
		}

		HANDLE h = isStdout
			? GetStdHandle(STD_OUTPUT_HANDLE)
			: CreateFileW(wavFilename, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS,
			              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, 0);
		if(h == INVALID_HANDLE_VALUE) {
			DWORD e = GetLastError();
			WCHAR buf[MAX_PATH];
			FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, 0, e, 0, buf, sizeof(buf) / sizeof(buf[0]), 0);
			fwprintf(stderr, L"Could not open \"%s\" for writing: %d %s\n", wavFilename, e, buf);
			return 1;
		}

		bool encodes_events = multiplex && outType != 1;
		HANDLE eh = NULL;
		if(ullEventInterest && !encodes_events) {
			if(isStdout) {
				eh = (HANDLE)_get_osfhandle(3);
			} else {
				fwprintf(stderr, L"Cannot select events (0x%04llx) when output is not stdout and multiplexing is not enabled\n", ullEventInterest);
				if(!isStdout) CloseHandle(h);
				return 1;
			}
		}

		// mux_encoder is a C handle; unique_ptr with the destroy fn gives it RAII.
		std::unique_ptr<mux_encoder, decltype(&mux_encoder_destroy)>
			enc(nullptr, mux_encoder_destroy);

		SynthSink::ByteSink audio_sink;
		if(outType == 1) {
			// raw PCM: audio bytes go straight to the handle.
			audio_sink = [h](const void* data, std::size_t len) {
				DWORD written;
				WriteFile(h, data, (DWORD)len, &written, NULL);
			};
		} else {
			// Parse --param KEY=VALUE strings into a mux_param array. Both name
			// and value backings live in these vectors — we reserve to their
			// final size so subsequent push_back()s can't reallocate and
			// invalidate the c_str() pointers we hand to muxaudio.
			std::vector<std::string> param_names, param_values;
			std::vector<mux_param> params;
			param_names.reserve(codecParams.size());
			param_values.reserve(codecParams.size());
			params.reserve(codecParams.size());

			for(const auto& w_kv : codecParams) {
				int n = WideCharToMultiByte(CP_UTF8, 0, w_kv.c_str(), (int)w_kv.size(),
				                            nullptr, 0, nullptr, nullptr);
				std::string kv8(n, '\0');
				if(n > 0) WideCharToMultiByte(CP_UTF8, 0, w_kv.c_str(), (int)w_kv.size(),
				                              kv8.data(), n, nullptr, nullptr);
				auto eq = kv8.find('=');
				if(eq == std::string::npos) {
					fwprintf(stderr, L"Invalid --param '%s': expected KEY=VALUE\n", w_kv.c_str());
					if(!isStdout) CloseHandle(h);
					return 1;
				}
				param_names.push_back(kv8.substr(0, eq));
				param_values.push_back(kv8.substr(eq + 1));

				// Look the parameter up so we know how to coerce the value.
				const mux_param_desc* descs = nullptr;
				int desc_count = 0;
				mux_get_encoder_params(codec, &descs, &desc_count);
				const mux_param_desc* desc = nullptr;
				for(int j = 0; j < desc_count; j++) {
					if(param_names.back() == descs[j].name) { desc = &descs[j]; break; }
				}
				if(!desc) {
					fwprintf(stderr, L"Unknown --param '%hs' for codec '%hs'\n",
					        param_names.back().c_str(), mux_codec_to_name(codec));
					if(!isStdout) CloseHandle(h);
					return 1;
				}

				mux_param p{};
				p.name = param_names.back().c_str();
				switch(desc->type) {
					case MUX_PARAM_TYPE_INT:
					case MUX_PARAM_TYPE_BOOL:
						p.value.i = atoi(param_values.back().c_str());
						break;
					case MUX_PARAM_TYPE_FLOAT:
						p.value.f = (float)atof(param_values.back().c_str());
						break;
					case MUX_PARAM_TYPE_STRING:
						p.value.s = param_values.back().c_str();
						break;
				}
				params.push_back(p);
			}

			mux_encoder* raw = mux_encoder_new(codec, (int)samplesPerSec, nChannels,
			                                   encodes_events ? 2 : 1,
			                                   params.empty() ? nullptr : params.data(),
			                                   (int)params.size(),
			                                   &mux_sink_write, h);
			if(!raw) {
				fwprintf(stderr, L"mux_encoder_new failed for codec '%hs'\n",
				        mux_codec_to_name(codec));
				if(!isStdout) CloseHandle(h);
				return 1;
			}
			enc.reset(raw);

			mux_encoder* enc_ptr = enc.get();
			audio_sink = [enc_ptr](const void* data, std::size_t len) {
				int r = mux_encoder_encode(enc_ptr, data, len, MUX_STREAM_AUDIO);
				if(r != MUX_OK) throw std::runtime_error("mux_encoder_encode(audio) failed");
			};
		}

		mux_encoder* enc_ptr = enc.get();
		SynthSink::ByteSink event_sink = [enc_ptr, encodes_events, eh](const void* data, std::size_t len) {
			if(enc_ptr && encodes_events) {
				int r = mux_encoder_encode(enc_ptr, data, len, MUX_STREAM_SIDE_CHANNEL);
				if(r != MUX_OK) throw std::runtime_error("mux_encoder_encode(events) failed");
			} else if(eh) {
				DWORD written;
				WriteFile(eh, data, (DWORD)len, &written, NULL);
			}
		};

		SynthSink sink(wfex, ullEventInterest, std::move(audio_sink), std::move(event_sink));
		hr = voice->SetOutput(static_cast<ISpStreamFormat*>(&sink), FALSE);
		if(FAILED(hr)) throw std::runtime_error("ISpVoice::SetOutput failed");
		hr = voice->Speak(text, speakFlags, nullptr);
		// Release ISpVoice's reference to our stack-allocated sink BEFORE it
		// goes out of scope; otherwise the voice still points at a dead stack
		// object and crashes on next teardown. Do this even if Speak failed.
		voice->SetOutput(nullptr, FALSE);
		if(FAILED(hr)) throw std::runtime_error("ISpVoice::Speak failed");

		if(enc) {
			int r = mux_encoder_finalize(enc.get());
			if(r != MUX_OK) throw std::runtime_error("mux_encoder_finalize failed");
		}
		if(!isStdout) CloseHandle(h);
		return 0;
	} catch(const std::exception& e) {
		fwprintf(stderr, L"Synthesis failed: %hs\n", e.what());
		return 1;
	}
}

int wmain(int argc, WCHAR *argv[]) {
	// https://stackoverflow.com/questions/2492077/output-unicode-strings-in-windows-console-app
	(void)_setmode(_fileno(stdout), _O_U8TEXT);

	const struct option long_options[] = {
		{ L"help", no_argument, 0, L'h' },
		{ L"list", no_argument, 0, L'l' },
		{ L"list-codecs", no_argument, 0, L'C' },
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
		{ L"param", required_argument, 0, L'P' },
		{ L"lexemes", required_argument, 0, L'L' },
		{ 0, 0, 0, 0 },
	};

	int help = 0;
	int list = 0;
	int listCodecsFlag = 0;
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
	std::vector<std::wstring> codecParams;
	std::wstring lexemesPath;

	int option;
	int option_index = 0;
	while(1) {
		option = getoptW_long(argc, argv, L"hlCo:T:v:t:r:Vs:b:c:e:mP:L:", long_options, &option_index);
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
			case L'C':
				listCodecsFlag = 1;
				break;
			case L'P':
				codecParams.emplace_back(optarg);
				break;
			case L'L':
				lexemesPath = optarg;
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

	if(!list && !listCodecsFlag && optind >= argc)
		help = 1;

	if(help) {
		fwprintf(
			stderr,
			L"Usage: %s --list | --list-codecs | [options] <text>\n"
			L"  -h, --help                      Print this help.\n"
			L"  -l, --list                      List all voices.\n"
			L"  -C, --list-codecs               List built-in muxaudio codecs, their\n"
			L"                                  supported sample rates and encoder\n"
			L"                                  parameters (JSON).\n"
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
			L"                                  same output. See README.md for how this works.\n"
			L"  -P, --param=KEY=VALUE           Codec encoder parameter (may repeat). See\n"
			L"                                  --list-codecs for names and ranges.\n"
			L"                                  Example: -P bitrate=64 -P complexity=10\n"
			L"  -L, --lexemes=FILE              Load custom SAPI pronunciations from a W3C\n"
			L"                                  PLS XML file. See lexemes.example.pls for\n"
			L"                                  the format; only alphabet='x-microsoft-sapi'\n"
			L"                                  is understood. Default: lexemes.pls next to\n"
			L"                                  sapicli.exe if present; otherwise skipped.\n"
			L"                                  Entries are added to the SAPI user lexicon\n"
			L"                                  and persist across runs (registry-backed).\n",
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
	} else if(listCodecsFlag) {
		ret = listCodecs();
	} else {
		ret = speakToWav(argv[optind], voice, wavFilename, outType, rate, volume, speakFlags, samplesPerSec, bitsPerSample, nChannels, ullEventInterest, multiplex, codecParams, lexemesPath);
	}

	::CoUninitialize();

	return ret;
}
