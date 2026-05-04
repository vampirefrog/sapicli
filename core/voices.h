#pragma once

#include <string>
#include <vector>

namespace sapicli {

struct VoiceInfo {
    std::wstring id;          // basename of the registry token (e.g. TTS_MS_EN-US_DAVID_11.0)
    std::wstring description;
    std::wstring age;
    std::wstring gender;
    std::wstring language;    // BCP-47 / locale name
    std::wstring name;
    std::wstring vendor;
};

// Enumerates installed SAPI voices via SpEnumTokens(SPCAT_VOICES). Throws on failure.
std::vector<VoiceInfo> enumerate_voices();

}  // namespace sapicli
