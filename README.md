# sapicli

[![build](https://github.com/vampirefrog/sapicli/actions/workflows/build.yml/badge.svg)](https://github.com/vampirefrog/sapicli/actions/workflows/build.yml)
[![release](https://img.shields.io/github/v/release/vampirefrog/sapicli?include_prereleases)](https://github.com/vampirefrog/sapicli/releases)

A Windows command-line frontend for [SAPI 5](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms720161(v=vs.85))
text-to-speech.

The release ship two small tools:

- **`sapicli`** — synthesize text into audio. Raw PCM, WAV, Ogg Vorbis, Ogg
  Opus, or MP3, with optional interleaved SAPI events (word boundary,
  viseme, phoneme, …).
- **`sapilex`** — edit the SAPI user pronunciation lexicon. Loads and dumps
  [W3C PLS 1.0](https://www.w3.org/TR/pronunciation-lexicon/) XML files.

Encoding is done by [muxaudio](https://github.com/vampirefrog/muxaudio); the
corresponding DLLs ship in the release zip so nothing else is needed on the
target machine.

---

## Install

Grab a build from **[Releases](https://github.com/vampirefrog/sapicli/releases)** —
`sapicli-<version>-x86.zip` (recommended, matches the 32-bit SAPI voice
registry most desktop TTS voices live in) or `-x64.zip`. Unzip anywhere;
`sapicli.exe`, `sapilex.exe`, the codec DLLs, and `lexemes.example.pls` all
sit next to each other.

## sapicli — synthesizing

```powershell
# List installed SAPI voices as JSON.
sapicli --list

# List codecs, their supported sample rates, and per-codec encoder params.
sapicli --list-codecs

# Speak into a .wav file (SAPI native RIFF; ext -> type autodetected).
sapicli -o hello.wav "Hello, world."

# Ogg Opus at 24 kHz, custom voice, bumped rate.
sapicli -v TTS_MS_EN-US_ZIRA_11.0 -T ogg+opus -s 24000 -r 3 -o out.ogg "Zippier."

# MP3, tuned bitrate. -P repeats for multiple encoder params.
sapicli -T mp3 -P bitrate=192 -P quality=2 -o out.mp3 "Higher bitrate mp3."

# Stream raw PCM to stdout (for piping to ffmpeg, aplay, etc.).
sapicli -T raw -s 22050 -c 1 -b 16 -o - "Piped" | ffplay -f s16le -ar 22050 -
```

Notable flags:

| Flag                   | Meaning                                                             |
|------------------------|---------------------------------------------------------------------|
| `-o, --output=FILE`    | Output file. `-` writes to stdout.                                  |
| `-T, --out-type=TYPE`  | `raw` \| `wav` \| `ogg` \| `ogg+opus` \| `mp3` \| `auto` (default). `auto` sniffs the extension. |
| `-v, --voice=ID`       | Voice token id (see `--list`, `id` field).                          |
| `-t, --type=TYPE`      | Input markup: `text` (default) \| `ssml` \| `sapi` \| `auto`.       |
| `-s / -c / -b`         | Sample rate / channels / bits per sample.                           |
| `-r / -V`              | Rate (−10..10) / volume (0..100).                                   |
| `-e, --events=MASK`    | Bitmask of `SPEI_*` events to emit; `all` = every TTS event.        |
| `-m, --multiplex`      | Interleave events with the audio inside the container.              |
| `-P, --param=KEY=VALUE`| Codec-specific encoder param (repeatable). See `--list-codecs`.     |

Unsupported sample rates on encoded formats are **refused up front** rather
than silently rewritten (muxaudio's `mux_sample_rate_supported`) — makes
voice/rate mismatches loud instead of pitch-shifted. `--list-codecs` tells
you what a codec accepts.

## sapilex — custom pronunciations

SAPI keeps a persistent user-lexicon in the registry (backed by a `.dat` file
under `%APPDATA%\Microsoft\Speech\Files\UserLexicons\`). `sapilex` reads and
writes it as W3C PLS XML.

```powershell
# Register everything in a PLS file with the user lexicon.
sapilex add    lexemes.example.pls

# Un-register them. Matches on (word, lang, phones) — the phones must match.
sapilex remove lexemes.example.pls

# Dump the whole user lexicon on stdout as PLS. Filter by language optionally.
sapilex list
sapilex list --lang en-US
```

Since `list` emits the same PLS shape `add` / `remove` consume, entries
round-trip through the file:

```powershell
sapilex list > my.pls        # snapshot
# edit my.pls, add / remove lexemes...
sapilex remove old.pls
sapilex add    my.pls        # rebase
```

The [`lexemes.example.pls`](lexemes.example.pls) file in the release zip has
the format spelled out in its header comment. Only the SAPI phoneme alphabet
(`alphabet="x-microsoft-sapi"`) is supported; phonemes get fed straight to
`ISpPhoneConverter::PhoneToId`. See
[American English Phoneme Table](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee431828(v=vs.85))
(and the per-language equivalents) for the SAPI phoneme names.

Entries persist across runs / reboots and are shared with every other SAPI 5
client on the machine, so `sapilex remove` is the correct un-do — deleting
the PLS file alone doesn't retract anything.

---

## Building from source

1. Install [Visual Studio 2022 Build Tools](https://aka.ms/vs/17/release/vs_BuildTools.exe).
   Under **Desktop development with C++**, tick **C++ ATL for latest v143
   build tools**. CMake / Testing / AddressSanitizer aren't needed.

   ![Build Tools](buildtools.png)

2. From a Developer Command Prompt for VS 2022, at the repo root:

   ```
   msbuild sapicli.sln -p:Configuration=Release -p:Platform=Win32
   ```

   For x64, use `-p:Platform=x64`.

The first build fetches the matching muxaudio release via
[`deploy/fetch-muxaudio.ps1`](deploy/fetch-muxaudio.ps1) into
`third_party\muxaudio\`. Subsequent builds skip the download if the files
are already there. `Win32\Release\` (or `x64\Release\`) then contains
`sapicli.exe`, `sapilex.exe`, and all the codec DLLs — copy that folder as
a self-contained distribution.

CI (GitHub Actions) builds both architectures on tag pushes matching `v*`
and publishes the zips to the corresponding [Release](https://github.com/vampirefrog/sapicli/releases).

---

## The EVNT chunk

`.wav` files produced with events enabled contain an `EVNT` chunk after the
`data` chunk — a list of
[`SPSERIALIZEDEVENT`](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee125137(v=vs.85))
records plus any strings referenced by them. The first byte identifies the
event type; most events are 24 bytes; wide-char strings that follow are
padded up to a 4-byte boundary. So a 126-byte string ends up stored as 128
bytes (last two zero).

![EVNT Chunk in RIFFPad](riffpad.png)

The size accounting from `sphelper.h`, for reference:

```cpp
template <class T>
inline ULONG SpSerializedEventSize(const T * pSerEvent)
{
    ULONG ulSize = sizeof(T);

    if( ( pSerEvent->elParamType == SPET_LPARAM_IS_POINTER ) && pSerEvent->SerializedlParam )
    {
        ulSize += ULONG(pSerEvent->SerializedwParam);
    }
    else if ((pSerEvent->elParamType == SPET_LPARAM_IS_STRING || pSerEvent->elParamType == SPET_LPARAM_IS_TOKEN) &&
             pSerEvent->SerializedlParam != NULL)
    {
        ulSize += ((ULONG)wcslen((WCHAR*)(pSerEvent + 1)) + 1) * sizeof( WCHAR );
    }
    // Round up to nearest DWORD
    ulSize += 3;
    ulSize -= ulSize % 4;
    return ulSize;
}
```

---

## References

- [SAPI 5 XML TTS Tutorial](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms717077(v=vs.85))
  — inline `<pron>`, `<voice>`, `<rate>`, `<volume>`, etc.
- [American English Phoneme Table](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee431828(v=vs.85))
  — the SAPI phoneme alphabet `sapilex` uses.
- [`SPEVENTENUM`](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee431845(v=vs.85))
  — event mask constants for `-e`.
- [`SPVISEMES`](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ms717289(v=vs.85))
  — viseme identifiers emitted alongside phoneme events.
- [W3C PLS 1.0](https://www.w3.org/TR/pronunciation-lexicon/) — lexicon file
  format `sapilex` reads and writes.
- [muxaudio](https://github.com/vampirefrog/muxaudio) — the multiplexing audio
  encoder library sapicli links against.
- [getoptW](https://github.com/bluebaroncanada/getoptW) — the getopt port
  sapicli uses for wide-char CLI parsing.
- [RIFFPad](https://www.menasoft.com/blog/?p=34) — handy for poking at
  `EVNT` chunks in generated WAVs.
