// sapilex — SAPI user-lexicon editor.
//
//   sapilex add     FILE.pls        register the file's pronunciations
//   sapilex remove  FILE.pls        un-register them again (matched on
//                                   (word, lang, phones))
//   sapilex list    [--lang LANG]   dump the user lexicon as PLS to stdout
//
// The user "compound" lexicon is registry-backed:
//   HKCU\Software\Microsoft\Speech\CurrentUserLexicon\{...}\Files
//     Datafile=%APPDATA%\Microsoft\Speech\Files\UserLexicons\SP_*.dat
// and shared across every SAPI 5 client, so anything registered here
// persists across runs and across reboots until explicitly removed.

#include <windows.h>
#include <atlbase.h>
#include <atlcom.h>
#include <sapi.h>
#pragma warning(push)   // sphelper.h uses GetVersionExA (deprecated)
#pragma warning(disable: 4996)
#include <sphelper.h>
#pragma warning(pop)
#include <initguid.h>
#include <io.h>
#include <fcntl.h>
#include <msxml6.h>

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// PLS I/O
// ---------------------------------------------------------------------------

struct Lexeme {
	LANGID       langId;
	std::wstring word;
	std::wstring phone;   // space-separated SAPI phoneme tokens
};

// "en-US" -> LANGID via LocaleNameToLCID. "0x0409" -> LANGID via strtoul.
static LANGID parseLang(const WCHAR* s) {
	if(!s || !*s) return 0;
	if(s[0] == L'0' && (s[1] == L'x' || s[1] == L'X'))
		return (LANGID)wcstoul(s, nullptr, 16);
	LCID lcid = LocaleNameToLCID(s, 0);
	return lcid ? LANGIDFROMLCID(lcid) : 0;
}

static void trim(std::wstring& s) {
	size_t b = s.find_first_not_of(L" \t\r\n");
	size_t e = s.find_last_not_of(L" \t\r\n");
	if(b == std::wstring::npos) { s.clear(); return; }
	s = s.substr(b, e - b + 1);
}

static std::wstring getAttr(IXMLDOMElement* el, const WCHAR* name) {
	if(!el) return L"";
	CComVariant v;
	if(FAILED(el->getAttribute(CComBSTR(name), &v)) || v.vt != VT_BSTR) return L"";
	return std::wstring(v.bstrVal ? v.bstrVal : L"");
}

// XML text-node escaping. Phoneme tokens are already ASCII SAPI phoneme names
// (no metacharacters), so this only really matters for graphemes.
static std::wstring xmlEscape(const std::wstring& s) {
	std::wstring out;
	out.reserve(s.size());
	for(WCHAR c : s) {
		switch(c) {
			case L'&':  out += L"&amp;";  break;
			case L'<':  out += L"&lt;";   break;
			case L'>':  out += L"&gt;";   break;
			case L'"':  out += L"&quot;"; break;
			case L'\'': out += L"&apos;"; break;
			default:    out += c; break;
		}
	}
	return out;
}

// Parse a W3C PLS 1.0 file via MSXML6.
static std::vector<Lexeme> loadPls(const std::wstring& path) {
	std::vector<Lexeme> out;

	CComPtr<IXMLDOMDocument2> doc;
	HRESULT hr = doc.CoCreateInstance(__uuidof(DOMDocument60));
	if(FAILED(hr)) {
		fwprintf(stderr, L"MSXML DOM create failed: 0x%08x\n", (unsigned)hr);
		return out;
	}
	doc->put_async(VARIANT_FALSE);
	doc->put_validateOnParse(VARIANT_FALSE);
	doc->put_resolveExternals(VARIANT_FALSE);
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

	// Only alphabet="x-microsoft-sapi" is understood; phonemes get fed to
	// ISpPhoneConverter::PhoneToId as-is.
	std::wstring alphabet = getAttr(root, L"alphabet");
	if(!alphabet.empty() && alphabet != L"x-microsoft-sapi") {
		fwprintf(stderr, L"%s: alphabet='%s' unsupported; only 'x-microsoft-sapi' works.\n",
		        path.c_str(), alphabet.c_str());
		return out;
	}

	LANGID rootLang = parseLang(getAttr(root, L"xml:lang").c_str());

	CComPtr<IXMLDOMNodeList> lexemes;
	doc->selectNodes(CComBSTR(L"//pls:lexeme"), &lexemes);
	if(!lexemes) return out;
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

		// PLS allows multiple <phoneme>s (pronunciation variants) but SAPI's
		// AddPronunciation takes one per (word, part-of-speech), so use the first.
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

// LANGID -> "en-US" (BCP-47) if the OS knows it; else "0xNNNN".
static std::wstring langToName(LANGID lang) {
	WCHAR locale[LOCALE_NAME_MAX_LENGTH] = L"";
	if(LCIDToLocaleName(MAKELCID(lang, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0) > 0)
		return locale;
	WCHAR buf[8];
	swprintf_s(buf, L"0x%04x", lang);
	return buf;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

// Feed a PLS file's lexemes through ISpLexicon::AddPronunciation. Returns
// non-zero on any per-entry failure (still tries the rest).
static int cmdAdd(const WCHAR* path, ISpLexicon* lexicon) {
	auto lex = loadPls(path);
	if(lex.empty()) { fwprintf(stderr, L"no lexemes to add\n"); return 0; }
	std::map<LANGID, CComPtr<ISpPhoneConverter>> converters;
	int added = 0, failed = 0;
	for(const auto& l : lex) {
		auto& conv = converters[l.langId];
		if(!conv) {
			HRESULT hr = SpCreatePhoneConverter(l.langId, NULL, NULL, &conv);
			if(FAILED(hr)) {
				fwprintf(stderr, L"phone converter (lang 0x%04x) failed: 0x%08x\n",
				        l.langId, (unsigned)hr);
				++failed; continue;
			}
		}
		SPPHONEID id[SP_MAX_PRON_LENGTH];
		HRESULT hr = conv->PhoneToId(l.phone.c_str(), id);
		if(FAILED(hr)) {
			fwprintf(stderr, L"phone '%s' -> id failed (lang 0x%04x): 0x%08x\n",
			        l.phone.c_str(), l.langId, (unsigned)hr);
			++failed; continue;
		}
		hr = lexicon->AddPronunciation(l.word.c_str(), l.langId, SPPS_Noun, id);
		if(FAILED(hr)) {
			fwprintf(stderr, L"add '%s' failed: 0x%08x\n", l.word.c_str(), (unsigned)hr);
			++failed; continue;
		}
		++added;
	}
	fwprintf(stderr, L"added %d, failed %d\n", added, failed);
	return failed == 0 ? 0 : 1;
}

// Un-register the entries a PLS file registered. SAPI matches on
// (word, langId, part-of-speech, phone-id sequence) so the phones in the file
// must match exactly for the removal to hit.
static int cmdRemove(const WCHAR* path, ISpLexicon* lexicon) {
	auto lex = loadPls(path);
	if(lex.empty()) { fwprintf(stderr, L"no lexemes to remove\n"); return 0; }
	std::map<LANGID, CComPtr<ISpPhoneConverter>> converters;
	int removed = 0, failed = 0;
	for(const auto& l : lex) {
		auto& conv = converters[l.langId];
		if(!conv) {
			HRESULT hr = SpCreatePhoneConverter(l.langId, NULL, NULL, &conv);
			if(FAILED(hr)) { ++failed; continue; }
		}
		SPPHONEID id[SP_MAX_PRON_LENGTH];
		HRESULT hr = conv->PhoneToId(l.phone.c_str(), id);
		if(FAILED(hr)) { ++failed; continue; }
		hr = lexicon->RemovePronunciation(l.word.c_str(), l.langId, SPPS_Noun, id);
		if(FAILED(hr)) {
			fwprintf(stderr, L"remove '%s' failed: 0x%08x\n", l.word.c_str(), (unsigned)hr);
			++failed; continue;
		}
		++removed;
	}
	fwprintf(stderr, L"removed %d, failed %d\n", removed, failed);
	return failed == 0 ? 0 : 1;
}

// Emit every user-lexicon entry as a PLS file on stdout.
//
// GetWords(eLEXTYPE_USER, ...) is paged via `cookie`; keep calling with the
// updated cookie until GetWords returns nothing new. Each SPWORD carries a
// linked list of SPWORDPRONUNCIATIONs; we filter those to eLEXTYPE_USER too
// (the vendor / letter-to-sound engines can add their own pronunciations
// that come through the same iteration).
static int cmdList(ISpLexicon* lexicon, LANGID langFilter) {
	std::map<LANGID, CComPtr<ISpPhoneConverter>> converters;

	wprintf(L"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	wprintf(L"<lexicon version=\"1.0\" xmlns=\"http://www.w3.org/2005/01/pronunciation-lexicon\" alphabet=\"x-microsoft-sapi\">\n");

	DWORD generation = 0;
	DWORD cookie     = 0;
	for(;;) {
		DWORD cookieBefore = cookie;
		SPWORDLIST wl = {};
		HRESULT hr = lexicon->GetWords(eLEXTYPE_USER, &generation, &cookie, &wl);
		if(hr == SPERR_NOT_FOUND) break;
		if(FAILED(hr)) {
			fwprintf(stderr, L"GetWords failed: 0x%08x\n", (unsigned)hr);
			break;
		}
		if(!wl.pFirstWord) break;

		for(SPWORD* w = wl.pFirstWord; w; w = w->pNextWord) {
			for(SPWORDPRONUNCIATION* p = w->pFirstWordPronunciation; p; p = p->pNextWordPronunciation) {
				if(!(p->eLexiconType & eLEXTYPE_USER)) continue;
				LANGID lang = (LANGID)p->LangID;
				if(langFilter && lang != langFilter) continue;

				auto& conv = converters[lang];
				if(!conv) {
					if(FAILED(SpCreatePhoneConverter(lang, NULL, NULL, &conv))) continue;
				}
				WCHAR phone[512] = {};
				if(FAILED(conv->IdToPhone(p->szPronunciation, phone))) continue;

				std::wstring localeName = langToName(lang);
				wprintf(L"  <lexeme xml:lang=\"%s\"><grapheme>%s</grapheme><phoneme>%s</phoneme></lexeme>\n",
				        localeName.c_str(),
				        xmlEscape(w->pszWord).c_str(),
				        phone);
			}
		}

		// GetWords allocated wl.pvBuffer for every SPWORD / SPWORDPRONUNCIATION
		// in this chunk; free it as one block.
		if(wl.pvBuffer) ::CoTaskMemFree(wl.pvBuffer);

		// GetWords leaves the cookie unchanged when there's nothing left to
		// enumerate. Break to avoid re-reading the same page forever.
		if(cookie == cookieBefore) break;
	}

	wprintf(L"</lexicon>\n");
	return 0;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void usage(const WCHAR* argv0) {
	fwprintf(stderr,
	    L"Usage:\n"
	    L"  %s add     FILE.pls          Register the file's pronunciations with\n"
	    L"                               the SAPI user lexicon (persistent).\n"
	    L"  %s remove  FILE.pls          Un-register them again. Matches on\n"
	    L"                               (word, lang, phones); the phones in the\n"
	    L"                               file must match what was added.\n"
	    L"  %s list    [--lang LANG]     Dump the user lexicon as PLS to stdout.\n"
	    L"                               --lang filters to one language (BCP-47\n"
	    L"                               or 0xLANGID).\n"
	    L"\n"
	    L"PLS file format: W3C PLS 1.0, alphabet=\"x-microsoft-sapi\". See\n"
	    L"https://www.w3.org/TR/pronunciation-lexicon/.\n",
	    argv0, argv0, argv0);
}

int wmain(int argc, WCHAR* argv[]) {
	// Emit UTF-8 to stdout so the PLS output (which contains wide chars for
	// non-ASCII graphemes) round-trips through pipes / redirects cleanly.
	(void)_setmode(_fileno(stdout), _O_U8TEXT);

	if(argc < 2 || !_wcsicmp(argv[1], L"-h") || !_wcsicmp(argv[1], L"--help")) {
		usage(argv[0]);
		return argc < 2 ? 1 : 0;
	}

	HRESULT hr = ::CoInitialize(NULL);
	if(FAILED(hr)) {
		fwprintf(stderr, L"CoInitialize failed: 0x%08x\n", (unsigned)hr);
		return 1;
	}

	// COM objects go out of scope BEFORE CoUninitialize -- otherwise their
	// destructors call Release() on already-shut-down COM interfaces, which
	// crashes the runtime.
	int ret = 0;
	{
		CComPtr<ISpLexicon> lexicon;
		hr = lexicon.CoCreateInstance(CLSID_SpLexicon);
		if(FAILED(hr)) {
			fwprintf(stderr, L"CoCreateInstance(SpLexicon) failed: 0x%08x\n", (unsigned)hr);
			::CoUninitialize();
			return 1;
		}

		if(!_wcsicmp(argv[1], L"add")) {
			if(argc < 3) { usage(argv[0]); ret = 1; }
			else         { ret = cmdAdd(argv[2], lexicon); }
		}
		else if(!_wcsicmp(argv[1], L"remove")) {
			if(argc < 3) { usage(argv[0]); ret = 1; }
			else         { ret = cmdRemove(argv[2], lexicon); }
		}
		else if(!_wcsicmp(argv[1], L"list")) {
			LANGID filter = 0;
			for(int i = 2; i < argc; i++) {
				if(!_wcsicmp(argv[i], L"--lang") && i + 1 < argc) {
					filter = parseLang(argv[++i]);
					if(filter == 0) {
						fwprintf(stderr, L"invalid --lang '%s'\n", argv[i]);
						ret = 1;
						break;
					}
				} else {
					fwprintf(stderr, L"unknown 'list' argument: %s\n", argv[i]);
					ret = 1;
					break;
				}
			}
			if(ret == 0) ret = cmdList(lexicon, filter);
		}
		else {
			fwprintf(stderr, L"unknown command: %s\n\n", argv[1]);
			usage(argv[0]);
			ret = 1;
		}
	}

	::CoUninitialize();
	return ret;
}
