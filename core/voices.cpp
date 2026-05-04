#include "voices.h"

#include <windows.h>
#include <atlbase.h>
#include <sapi.h>
#pragma warning(push)
#pragma warning(disable: 4996)
#include <sphelper.h>
#pragma warning(pop)

#include <stdexcept>

namespace sapicli {

namespace {

std::wstring take_string(WCHAR*& p) {
    std::wstring s = p ? p : L"";
    if (p) { CoTaskMemFree(p); p = nullptr; }
    return s;
}

}  // namespace

std::vector<VoiceInfo> enumerate_voices() {
    CComPtr<IEnumSpObjectTokens> voicesEnum;
    HRESULT hr = SpEnumTokens(SPCAT_VOICES, NULL, NULL, &voicesEnum);
    if (FAILED(hr)) throw std::runtime_error("SpEnumTokens failed");

    ULONG count = 0;
    hr = voicesEnum->GetCount(&count);
    if (FAILED(hr)) throw std::runtime_error("voicesEnum->GetCount failed");

    std::vector<VoiceInfo> out;
    out.reserve(count);

    for (ULONG i = 0; i < count; ++i) {
        CComPtr<ISpObjectToken> token;
        hr = voicesEnum->Next(1, &token, NULL);
        if (FAILED(hr)) throw std::runtime_error("voicesEnum->Next failed");

        VoiceInfo v;

        WCHAR* id_full = nullptr;
        token->GetId(&id_full);
        WCHAR* basename = id_full ? wcsrchr(id_full, L'\\') : nullptr;
        v.id = (basename && basename[1]) ? std::wstring(basename + 1)
             : (id_full ? std::wstring(id_full) : std::wstring());
        if (id_full) CoTaskMemFree(id_full);

        WCHAR* desc = nullptr;
        SpGetDescription(token, &desc);
        v.description = take_string(desc);

        CComPtr<ISpDataKey> attrs;
        if (SUCCEEDED(token->OpenKey(L"Attributes", &attrs))) {
            WCHAR* p = nullptr;
            if (SUCCEEDED(attrs->GetStringValue(L"Age", &p)))    v.age = take_string(p);
            if (SUCCEEDED(attrs->GetStringValue(L"Gender", &p))) v.gender = take_string(p);
            if (SUCCEEDED(attrs->GetStringValue(L"Language", &p))) {
                int langId = wcstol(p, nullptr, 16);
                WCHAR locale[LOCALE_NAME_MAX_LENGTH] = {0};
                if (LCIDToLocaleName(langId, locale, LOCALE_NAME_MAX_LENGTH, 0))
                    v.language = locale;
                else
                    v.language = p;
                CoTaskMemFree(p); p = nullptr;
            }
            if (SUCCEEDED(attrs->GetStringValue(L"Name", &p)))   v.name = take_string(p);
            if (SUCCEEDED(attrs->GetStringValue(L"Vendor", &p))) v.vendor = take_string(p);
        }

        out.push_back(std::move(v));
    }

    return out;
}

}  // namespace sapicli
