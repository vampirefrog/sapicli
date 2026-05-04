# sapicli redesign plan

A working document tracking the redesign of `sapicli` from a monolithic CLI
into a reusable core + CLI + HTTP service, with FFmpeg-backed encoding and
automated deployment.

## Goals

1. Replace the in-tree `muxaudio` submodule with FFmpeg's `libavcodec` /
   `libavformat`. Keep the WAV-with-EVNT-RIFF path (SAPI's `SPBindToFile`)
   untouched.
2. Split reusable code (voice enumeration, synthesis, encoders) into a static
   library; keep the CLI thin; add an HTTP server using the Windows
   built-in HTTP API v2 backed by the same library.
3. Provide a one-shot remote deploy that uses Windows-native transports
   (PowerShell Remoting over WinRM).
4. Add a Gitea Actions workflow on a self-hosted Windows runner that builds
   on every push to `master` and deploys to prod on tag pushes.
5. Ship a browser client that decodes the streamed audio + events (native
   browser decoders preferred; WASM-FFmpeg fallback for codecs the browser
   lacks) and renders an animated puppet (visemes → mouth shapes) plus a
   speech bubble (word boundaries → words appearing in real time).

## Decisions

| Area | Decision |
|---|---|
| FFmpeg distribution | vcpkg manifest mode, triplet `x64-windows-static` |
| FFmpeg features | `avcodec`, `avformat`, `vorbis`, `opus`, `mp3lame` |
| WAV output | Unchanged — `ISpVoice::SetOutput` + `SPBindToFile`, RIFF EVNT chunk preserved |
| Ogg/Vorbis output | `libavformat` ogg muxer, optional secondary logical stream of binary `SPSERIALIZEDEVENT` blobs |
| Ogg/Opus output | Same as Vorbis with the Opus encoder |
| MP3 output | `libavformat` mp3 muxer; events bundled as ID3v2 PRIV frames written at end (lump, not streamed) |
| MKV output | Out of scope for now (deferred) |
| Event streaming | Server: events on a parallel container stream where the container supports it; CLI: events on fd 3 (matches today's behaviour at sapicli.cpp:348) |
| HTTP API | Windows HTTP Server API v2 (`httpapi.lib`), chunked transfer for streamed responses |
| Auth | API keys from `%ProgramData%\sapicli\keys.json`, per-key token-bucket rate limit, tiers |
| Public endpoint | Unauthenticated tier with strict per-IP token bucket (no captcha for now — see "Future" below for proof-of-work notes) |
| Concurrency | Thread pool, per-request COM apartment + `ISpVoice` instance |
| Service | Windows Service registered with SCM, runs as `NetworkService`; rolling structured logs to `%ProgramData%\sapicli\logs\sapisrv-YYYYMMDD.log`; `sapisrv logs --follow` subcommand to tail in real time |
| Deploy transport | PowerShell Remoting (WinRM) — `New-PSSession` → `Copy-Item -ToSession` → `Invoke-Command` |
| CI runner | Self-hosted Windows runner (`act_runner`) installed as a service on **this** machine (the dev box); prod is a separate Windows VM reached over WinRM |
| CI triggers | `push` to `master` → build + smoke; tag push → build + deploy to prod |
| Backwards compatibility | None required — output bytes may diverge from current `muxaudio` output |
| Web client transport | `fetch()` against `POST /synthesize` with chunked response; audio fed into Web Audio API (or `<audio>` via MSE) as bytes arrive |
| Container demux in browser | Lightweight JS Ogg-page demuxer splits audio packets from the secondary event stream — audio packets go to native decode, event packets are deserialized and timestamped |
| Audio decode in browser | Native first (`AudioContext.decodeAudioData` or MSE source buffers); WASM-FFmpeg fallback for browsers that lack the codec (notably Safari + Ogg/Opus) |
| WASM-FFmpeg build | Custom emcc build of `libavformat`/`libavcodec` with **only** Vorbis + Opus + MP3 demux/decode enabled (no encoders, no other muxers) — keeps the WASM blob small |
| Viseme rendering | Sprite/SVG keyed by SAPI viseme ID (21 shapes); driven by event timestamps converted from sample offsets |
| Word-boundary rendering | Per-word DOM nodes in a speech bubble, fade in on event, fade out after a short tail |
| Client source assets | **Graphics only** from `ssh://gitea@git.vampi.tech:2222/vampi/ttsservice.git` — `cat.png`, `mic.png`, the matching `.xcf` source files, `favicon.ico`. Animation, demux, decode, and player are written fresh |

## Project layout (target)

```
sapicli/
├── vcpkg.json                       (manifest: ffmpeg, etc.)
├── sapicli.sln
├── core/                            (static lib, reusable)
│   ├── voices.{h,cpp}
│   ├── synth.{h,cpp}                (ISpVoice driver, COM apt mgmt)
│   ├── events.{h,cpp}               (SPSERIALIZEDEVENT serialization)
│   └── encoders/
│       ├── encoder.h                (sink interface)
│       ├── ogg_vorbis.cpp
│       ├── ogg_opus.cpp
│       └── mp3_id3.cpp
├── cli/
│   └── main.cpp                     (sapicli.exe — thin)
├── server/
│   ├── service.cpp                  (SCM entry, console mode)
│   ├── http.cpp                     (Http API v2 loop)
│   ├── handlers.cpp                 (/voices, /synthesize)
│   ├── auth.cpp                     (keys + rate limit)
│   └── logs.cpp                     (rolling file + tail-follow)
├── deploy/
│   ├── Install-Service.ps1
│   ├── Deploy-Remote.ps1            (PSRemoting)
│   └── runner-setup.ps1             (Gitea runner install)
└── client/                          (browser SPA — served by sapisrv at /)
    ├── package.json
    ├── src/
    │   ├── index.html
    │   ├── app.ts                   (entry: fetch → demux → play → render)
    │   ├── demux/
    │   │   ├── ogg.ts               (page parser, splits audio + events)
    │   │   └── events.ts            (SPSERIALIZEDEVENT decoder)
    │   ├── decode/
    │   │   ├── native.ts            (Web Audio decode path)
    │   │   └── wasm.ts              (FFmpeg-WASM fallback loader)
    │   ├── render/
    │   │   ├── puppet.ts            (viseme → mouth shape sprite/SVG)
    │   │   └── bubble.ts            (word boundaries → speech bubble)
    │   └── assets/                  (sprites/SVGs from ttsservice)
    └── ffmpeg-wasm/                 (custom emcc build inputs/output)
        ├── build.sh                 (emcc invocation, codec allowlist)
        └── dist/                    (built .wasm + glue, gitignored)
```

## Sequencing

### Step 1 — FFmpeg swap (current)

- Add `vcpkg.json`.
- Create `core/encoders/encoder.h` defining the new sink interface.
- Implement Ogg/Vorbis, Ogg/Opus, and MP3 encoders against
  `libavformat` / `libavcodec`.
- Rewire `sapicli.cpp` to instantiate the new encoders.
- Update `sapicli.vcxproj`: enable vcpkg manifest integration, drop the
  `muxaudio\*.c` sources and the hard-coded lame/ogg/vorbis/opus
  `AdditionalDependencies` and `AdditionalIncludeDirectories`.
- Remove the `muxaudio` git submodule.
- Build under `ReleaseStatic | x64`. Verify CLI still produces bytes for
  WAV, Ogg/Vorbis, Ogg/Opus, MP3 and writes events to fd 3.

### Step 2 — refactor + HTTP server

- Move synthesis, voice enumeration, and event serialization out of
  `sapicli.cpp` into `core/`.
- Reduce `sapicli.cpp` to a thin CLI front-end over `core/`.
- Build `server/` as a separate exe:
  - `service.cpp`: SCM dispatcher; supports `--console` for foreground mode.
  - `http.cpp`: HTTP API v2 with URL group, request queue, completion
    callbacks, chunked response writer.
  - `handlers.cpp`:
    - `GET /voices` — JSON array.
    - `POST /synthesize` — body has `text`/`ssml`, query has `voice`,
      `format`, `multiplex`. Streams audio (chunked) with content type per
      format; events on a parallel container stream (Ogg) or as an ID3
      trailer (MP3).
  - `auth.cpp`: load keys from `%ProgramData%\sapicli\keys.json`, validate
    `Authorization: Bearer <key>` or `?api_key=...`, enforce per-key
    token-bucket limits; unauthenticated requests fall to public-tier
    per-IP bucket.
  - `logs.cpp`: rolling file logger with `tail --follow` subcommand
    semantics (poll + watch via `ReadDirectoryChangesW`).

### Step 3 — deploy

- `deploy/Install-Service.ps1` (run once per target):
  - Create `%ProgramData%\sapicli\` (logs, keys, config).
  - Reserve URL ACL: `netsh http add urlacl url=http://+:8080/ user="NT AUTHORITY\NetworkService"`.
  - Register service: `New-Service -Name sapisrv -BinaryPathName ... -StartupType Automatic`.
- `deploy/Deploy-Remote.ps1` (run by CI):
  - Open `New-PSSession -ComputerName $env:PROD_HOST -Credential $cred`.
  - `Copy-Item -ToSession` the `dist/` folder to a staging path.
  - `Invoke-Command`: stop service, swap binaries (atomic dir rename),
    start service, hit `GET /voices` for smoke.

### Step 4 — Gitea runner + workflow

- `deploy/runner-setup.ps1`: download `act_runner.exe`, register against
  the Gitea instance with a registration token, install as a Windows
  service (`act_runner.exe service install`) on the dev box (this
  machine).
- `.gitea/workflows/build.yml`:
  - Triggers: `push` on `master`, `push` on tags `v*`.
  - Job `build`: checkout, `vcpkg install` (cached), `msbuild
    sapicli.sln /p:Configuration=ReleaseStatic /p:Platform=x64`, archive
    `dist/`, smoke-run.
  - Job `deploy-prod` (if tag): use `deploy/Deploy-Remote.ps1` with
    secrets `PROD_HOST`, `PROD_USER`, `PROD_PASS_OR_KEY` to push to the
    separate prod Windows VM.

### Step 5 — Web client

Source assets and any reusable bits from
`ssh://gitea@git.vampi.tech:2222/vampi/ttsservice.git`. Pipeline:

1. **Fetch.** `fetch('/synthesize', {method: 'POST', body: ...})` — read
   `response.body` as a `ReadableStream<Uint8Array>`.
2. **Demux.** Feed the stream to a small JS Ogg-page parser that splits
   the audio logical bitstream from the events logical bitstream by
   serial number. Audio packets go to the decode path; event packets are
   `SPSERIALIZEDEVENT` blobs that we deserialize into JS objects with a
   stream offset (sample count) and a payload type (viseme, word
   boundary, sentence boundary, phoneme).
3. **Decode (audio).**
   - Native path: assemble the audio packets back into a self-contained
     Ogg stream slice, hand to an MSE source buffer or
     `decodeAudioData`. Preferred when supported.
   - Fallback path: instantiate the WASM-FFmpeg module and decode the
     packets directly to PCM. Triggered when feature-detection fails
     (e.g. Safari + Opus).
4. **Schedule.** As decoded audio buffers land, schedule them on a
   single `AudioContext` timeline. Convert each event's stream offset to
   a wall-clock playback time and enqueue UI callbacks on a per-frame
   `requestAnimationFrame` loop that fires events whose deadline has
   passed.
5. **Render.**
   - **Puppet**: a sprite sheet (or SVG `<g>` swap) keyed by SAPI viseme
     ID (21 visemes, `SPVISEMES` enum). On a viseme event, swap the
     mouth-shape layer; ease for a few frames for smoothness.
   - **Bubble**: word-boundary events spawn `<span>` per word, fading in
     at speak time and out after a short tail; sentence-boundary events
     can introduce a paragraph break.

WASM-FFmpeg build (`client/ffmpeg-wasm/build.sh`): `emcc` invocation
with `--disable-everything --enable-decoder=vorbis,opus,mp3
--enable-demuxer=ogg,mp3 --enable-parser=vorbis,opus,mp3
--enable-protocol=file` (and a memory FS shim). Output should land
under ~500KB gzipped.

## Out of scope (for now)

- MKV container output (deferred).
- Captcha or proof-of-work on the public endpoint.
- HTTPS termination at the service (terminate at a reverse proxy if
  needed).
- Backwards-byte-compat with the current `muxaudio` output.

## Future: proof-of-work for the public endpoint

If per-IP rate limiting proves insufficient against scrapers, a
client-side proof-of-work (PoW) puzzle is a lighter-touch alternative to
captchas. The shape:

1. The server issues a short-lived **challenge**: random bytes plus a
   difficulty `K` (number of leading zero bits required).
2. The client searches for a **nonce** `N` such that
   `SHA-256(challenge || N)` has at least `K` leading zero bits. Cost is
   exponential in `K`: at `K=20`, a few hundred milliseconds on a phone
   CPU; at `K=24`, a few seconds.
3. The client submits `N` along with the synth request; the server
   verifies in one hash (microseconds) and rejects on mismatch.

Why this works:

- Each successful synth request costs server CPU + SAPI time, so making
  the client pay even a small fixed CPU cost per request makes mass
  scraping uneconomical.
- Legit users see one short delay on first use; the challenge can be
  reused across N requests for some short window.
- No third-party dependency, no privacy implications, no UI puzzle.

Real-world implementations to mirror: **Anubis**, **mCaptcha**, and
**Friendly Captcha**'s model (commercial, but the protocol is
well-documented).

We will revisit if the public endpoint sees scraping.
