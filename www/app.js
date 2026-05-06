// sapicli web demo — fetches /synthesize, decodes audio with the browser's
// native Web Audio decoder, parses the parallel Ogg event bitstream that our
// server multiplexes in, and drives a viseme puppet + word-boundary bubble.

// ---------------------------------------------------------------------------
// SAPI event constants
// ---------------------------------------------------------------------------
const SPEI_WORD_BOUNDARY     = 5;
const SPEI_PHONEME           = 6;
const SPEI_SENTENCE_BOUNDARY = 7;
const SPEI_VISEME            = 8;

// All TTS events bitmask (matches sapi.h SPF_ALL_TTS_EVENTS = 0xfffe).
const ALL_EVENTS = 0xfffe;

const EVENT_NAMES = {
  1: 'StartInputStream', 2: 'EndInputStream', 3: 'VoiceChange',
  4: 'TtsBookmark', 5: 'WordBoundary', 6: 'Phoneme',
  7: 'SentenceBoundary', 8: 'Viseme', 9: 'TtsAudioLevel',
  15: 'TtsPrivate',
};

// ---------------------------------------------------------------------------
// Characters: sprite sheets + viseme map.
// Reused from the older ttsservice — same artwork, fresh code.
// ---------------------------------------------------------------------------
const CHARACTERS = {
  cat: {
    img: '/cat.png',
    width: 177, height: 300,
    visemeMap: [1, 11, 11, 11, 10, 11, 10, 2, 12, 9, 11, 11, 9, 3, 6, 7, 7, 5, 4, 7, 7, 1],
  },
  mic: {
    img: '/mic.png',
    width: 128, height: 128,
    visemeMap: [0, 11, 11, 11, 10, 11, 9, 1, 2, 9, 12, 11, 9, 3, 6, 7, 8, 5, 4, 7, 9, 0],
  },
};

// ---------------------------------------------------------------------------
// muxaudio WASM decoder
// ---------------------------------------------------------------------------
// We hand the entire response to muxaudio's mux_decoder which knows how to
// demux ogg + side-channel for vorbis/opus. It returns audio packets as
// Int16Array PCM and side-channel packets as raw Uint8Array (one
// SPSERIALIZEDEVENT per packet, exactly what the server submitted).
const MUX_CODEC = { pcm: 0, opus: 1, vorbis: 2, mp3: 4 };
const MUX_STREAM_AUDIO = 0;
const MUX_STREAM_SIDE  = 1;

let muxModule = null;
async function getMuxModule() {
  if (muxModule) return muxModule;
  if (typeof window.createMuxAudioWasm !== 'function') {
    throw new Error('muxaudio.js not loaded (createMuxAudioWasm undefined)');
  }
  muxModule = await window.createMuxAudioWasm();
  return muxModule;
}

// Decode a complete muxed response in one shot. Returns
// { audio: Uint8Array, events: Uint8Array[] }.
// Audio bytes are PCM int16 little-endian for vorbis/opus (interleaved if
// stereo), or raw MP3 frames in mp3 passthrough mode. Caller decides how to
// interpret based on the codec.
async function decodeWithMuxaudio(codec, bytes) {
  const m = await getMuxModule();

  const _new      = m.cwrap('mux_decoder_new',      'number', ['number','number','number','number']);
  const _decode   = m.cwrap('mux_decoder_decode',   'number', ['number','number','number','number']);
  const _read     = m.cwrap('mux_decoder_read',     'number', ['number','number','number','number','number']);
  const _finalize = m.cwrap('mux_decoder_finalize', 'number', ['number']);
  const _destroy  = m.cwrap('mux_decoder_destroy',  null,     ['number']);

  const dec = _new(MUX_CODEC[codec], 2 /* num_streams: audio + side */, 0, 0);
  if (!dec) throw new Error('mux_decoder_new failed');

  const audioChunks = [];
  const eventBlobs  = [];
  const READ_BUF = 256 * 1024;
  const outPtr     = m._malloc(READ_BUF);
  const writtenPtr = m._malloc(8);
  const stPtr      = m._malloc(4);

  function drain() {
    for (;;) {
      const r = _read(dec, outPtr, READ_BUF, writtenPtr, stPtr);
      // 32-bit read of size_t low half is safe — no single drain returns >4GB.
      const written = m.HEAPU32[writtenPtr >> 2];
      if (written > 0) {
        const st = m.HEAP32[stPtr >> 2];
        // slice() detaches from the WASM HEAP into a JS-owned buffer.
        const chunk = m.HEAPU8.slice(outPtr, outPtr + written);
        if (st === MUX_STREAM_AUDIO) audioChunks.push(chunk);
        else                          eventBlobs.push(chunk);
      }
      // -4 AGAIN: nothing more right now. -6 EOF: end of stream. else error.
      if (r === -4 || r === -6) return r;
      if (r < 0) throw new Error(`mux_decoder_read failed: ${r}`);
      if (written === 0) return 0;
    }
  }

  // Feed all input. The wrapper's max chunk is 4MB; our responses are well under.
  const inPtr      = m._malloc(bytes.length);
  m.HEAPU8.set(bytes, inPtr);
  const consumedPtr = m._malloc(8);
  const dr = _decode(dec, inPtr, bytes.length, consumedPtr);
  if (dr < 0 && dr !== -6) {
    m._free(inPtr); m._free(consumedPtr); m._free(outPtr); m._free(writtenPtr); m._free(stPtr);
    _destroy(dec);
    throw new Error(`mux_decoder_decode failed: ${dr}`);
  }
  m._free(inPtr); m._free(consumedPtr);
  drain();

  _finalize(dec);
  drain();

  m._free(outPtr); m._free(writtenPtr); m._free(stPtr);
  _destroy(dec);

  // Concatenate audio chunks into a single byte buffer.
  const totalBytes = audioChunks.reduce((s, c) => s + c.length, 0);
  const audio = new Uint8Array(totalBytes);
  let off = 0;
  for (const c of audioChunks) { audio.set(c, off); off += c.length; }
  return { audio, events: eventBlobs };
}

// SPSERIALIZEDEVENT: 24-byte header
//   eEventId (i16) elParamType (i16) ulStreamNum (u32)
//   ullAudioStreamOffset (u64, low half used) wParam (u32) lParam (i32)
// followed by a payload whose size depends on elParamType:
//   POINTER (3) with lParam!=0: payload = wParam bytes
//   STRING (4) / TOKEN (1) with lParam!=0: payload = wParam bytes (wide string + null)
//   otherwise: no payload
// Total size is rounded up to a DWORD (4-byte) boundary.
const SPET_LPARAM_IS_TOKEN   = 1;
const SPET_LPARAM_IS_POINTER = 3;
const SPET_LPARAM_IS_STRING  = 4;

function decodeEvent(packet) {
  if (packet.length < 24) return null;
  const v = new DataView(packet.buffer, packet.byteOffset, 24);
  return {
    eventId:          v.getInt16(0, true),
    paramType:        v.getInt16(2, true),
    streamNum:        v.getUint32(4, true),
    audioOffsetBytes: v.getUint32(8, true),
    wParam:           v.getUint32(16, true),
    lParam:           v.getInt32(20, true),
  };
}

// Compute the encoded size of one event starting at `off` in `blob`.
function eventSize(blob, off) {
  if (off + 24 > blob.length) return 0;
  const v = new DataView(blob.buffer, blob.byteOffset + off, 24);
  const elParamType = v.getInt16(2, true);
  const wParam = v.getUint32(16, true);
  const lParam = v.getInt32(20, true);
  let extra = 0;
  if (lParam !== 0 && (elParamType === SPET_LPARAM_IS_POINTER
                    || elParamType === SPET_LPARAM_IS_STRING
                    || elParamType === SPET_LPARAM_IS_TOKEN)) {
    extra = wParam;
  }
  return Math.ceil((24 + extra) / 4) * 4;
}

// ---------------------------------------------------------------------------
// Puppet renderer
// ---------------------------------------------------------------------------
class Puppet {
  constructor(faceEl, mouthEl) {
    this.face = faceEl;
    this.mouth = mouthEl;
    this.character = null;
  }
  setCharacter(name) {
    const c = CHARACTERS[name];
    if (!c) return;
    this.character = c;
    this.face.style.width = c.width + 'px';
    this.face.style.height = c.height + 'px';
    this.face.style.backgroundImage = `url('${c.img}')`;
    this.mouth.style.backgroundImage = `url('${c.img}')`;
    this.setViseme(0);
  }
  setViseme(visemeId) {
    if (!this.character) return;
    const sprite = this.character.visemeMap[visemeId];
    if (sprite === undefined || sprite === null) {
      this.mouth.style.display = 'none';
      return;
    }
    this.mouth.style.display = 'block';
    this.mouth.style.backgroundPosition = `${-sprite * this.character.width}px 0`;
  }
}

// ---------------------------------------------------------------------------
// Bubble renderer (word-boundary events)
// ---------------------------------------------------------------------------
class Bubble {
  constructor(el) {
    this.el = el;
    this.words = [];
  }
  reset(text) {
    this.el.innerHTML = '';
    this.words = [];
    // Split on whitespace, preserve punctuation attached to words.
    const tokens = text.match(/\S+/g) || [];
    for (const t of tokens) {
      const span = document.createElement('span');
      span.className = 'word';
      span.textContent = t;
      this.el.appendChild(span);
      this.words.push({ pos: text.indexOf(t), len: t.length, el: span });
    }
    this.el.classList.add('visible');
  }
  speak(charPos) {
    let target = -1;
    for (let i = 0; i < this.words.length; ++i) {
      if (this.words[i].pos <= charPos && charPos < this.words[i].pos + this.words[i].len) {
        target = i;
        break;
      }
    }
    if (target < 0) return;
    for (let i = 0; i < this.words.length; ++i) {
      const cls = i < target ? 'past' : i === target ? 'spoken' : '';
      this.words[i].el.className = 'word' + (cls ? ' ' + cls : '');
    }
  }
  end() {
    for (const w of this.words) w.el.className = 'word past';
  }
}

// ---------------------------------------------------------------------------
// Audio + event scheduler
// ---------------------------------------------------------------------------
let audioCtx = null;
function getAudio() {
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  return audioCtx;
}

async function speak(opts) {
  const { text, voice, format, eventsMask, textType,
          sampleRate, channels, bits,
          rate, volume, apiKey, status, puppet, bubble } = opts;
  status.textContent = 'requesting…';

  // Format selection has two axes: the codec (ogg+vorbis, ogg+opus,
  // mp3) and whether speech events are interleaved. Events are
  // interleaved iff eventsMask != 0 -- the server enables multiplex
  // automatically in that case (no need to send `multiplex` ourselves).
  // ogg+* uses a parallel ogg stream for events; mp3 wraps the audio
  // and event packets in a leb128 frame, which the WASM demuxer pulls
  // apart on the client. mp3 with no events bypasses WASM entirely
  // and lets the browser native-decode the raw mp3.
  const hasEvents = eventsMask > 0;
  let codec;
  let multiplex      = hasEvents;
  let mp3Passthrough = false;
  if      (format === 'ogg+vorbis')                 { codec = 'vorbis'; }
  else if (format === 'ogg+opus')                   { codec = 'opus';   }
  else if (format === 'mp3' &&  hasEvents)          { codec = 'mp3'; mp3Passthrough = true; }
  else if (format === 'mp3' && !hasEvents)          { codec = null;                          }
  else { status.textContent = `unsupported format: ${format}`; return; }

  const params = new URLSearchParams({
    text, format, rate, volume,
    type: textType,
    events: String(eventsMask),
    sample_rate: String(sampleRate), channels: String(channels), bits: String(bits),
  });
  if (voice) params.set('voice', voice);

  const headers = {};
  if (apiKey) headers['Authorization'] = 'Bearer ' + apiKey;

  let resp;
  try {
    resp = await fetch('/synthesize?' + params.toString(), { headers });
  } catch (e) { status.textContent = 'fetch failed: ' + e; return; }
  if (!resp.ok) {
    let msg = `HTTP ${resp.status}`;
    try { msg += ' — ' + (await resp.text()); } catch {}
    status.textContent = msg;
    return;
  }

  status.textContent = 'downloading…';
  const arrayBuf = await resp.arrayBuffer();

  // Plain mp3 path: skip WASM, hand the bytes to the browser as-is.
  if (!multiplex) {
    const ctx = getAudio();
    if (ctx.state === 'suspended') await ctx.resume();
    let audioBuf;
    try { audioBuf = await ctx.decodeAudioData(arrayBuf.slice(0)); }
    catch (e) { status.textContent = 'decodeAudioData failed: ' + e.message; return; }
    bubble.reset(text);
    status.textContent = `playing plain ${format} (${audioBuf.duration.toFixed(2)}s, no events)`;
    const src = ctx.createBufferSource();
    src.buffer = audioBuf;
    src.connect(ctx.destination);
    src.start();
    src.onended = () => { puppet.setViseme(0); bubble.end(); status.textContent = 'done'; };
    return;
  }

  status.textContent = 'decoding (muxaudio wasm)…';
  let decoded;
  try {
    decoded = await decodeWithMuxaudio(codec, new Uint8Array(arrayBuf));
  } catch (e) {
    status.textContent = 'mux decode failed: ' + e.message;
    return;
  }
  const { audio, events: eventBlobs } = decoded;

  const ctx = getAudio();
  if (ctx.state === 'suspended') await ctx.resume();

  if (audio.length === 0) {
    status.textContent = `decoded 0 audio bytes (${eventBlobs.length} events)`;
    return;
  }

  let audioBuf;
  if (mp3Passthrough) {
    // muxaudio's WASM build doesn't carry mpg123, so the "audio" output
    // here is raw mp3 frames. Hand the concatenated stream to the browser's
    // native mp3 decoder.
    try {
      audioBuf = await ctx.decodeAudioData(audio.buffer.slice(audio.byteOffset, audio.byteOffset + audio.byteLength));
    } catch (e) {
      status.textContent = 'mp3 decodeAudioData failed: ' + e.message;
      return;
    }
  } else {
    // PCM int16 little-endian. Convert to float32 in an AudioBuffer.
    // For stereo we get L/R interleaved samples and need to deinterleave.
    const pcm    = new Int16Array(audio.buffer, audio.byteOffset, audio.byteLength >> 1);
    const frames = pcm.length / channels;
    audioBuf     = ctx.createBuffer(channels, frames, sampleRate);
    if (channels === 1) {
      const ch0 = audioBuf.getChannelData(0);
      for (let i = 0; i < frames; ++i) ch0[i] = pcm[i] / 32768;
    } else {
      const chs = Array.from({ length: channels }, (_, c) => audioBuf.getChannelData(c));
      for (let f = 0; f < frames; ++f) {
        for (let c = 0; c < channels; ++c) {
          chs[c][f] = pcm[f * channels + c] / 32768;
        }
      }
    }
  }

  // muxaudio's mux_decoder_read coalesces queued side-channel packets into one
  // ring-buffer read, so a single blob may contain many SPSERIALIZEDEVENTs back
  // to back. Each event has a 24-byte header optionally followed by a string
  // or binary payload (size in wParam, padded to a DWORD); see eventSize().
  const decodedEvents = [];
  for (const blob of eventBlobs) {
    let off = 0;
    while (off + 24 <= blob.length) {
      const sz = eventSize(blob, off);
      if (sz === 0 || off + sz > blob.length) break;
      const e = decodeEvent(blob.subarray(off, off + sz));
      if (e) decodedEvents.push(e);
      off += sz;
    }
  }

  // Event timing: use SPSERIALIZEDEVENT.ullAudioStreamOffset (bytes into the
  // source PCM stream that SAPI synthesized at). The server requests
  // sample_rate × 1 channel × 2 bytes/sample.
  const SOURCE_BYTES_PER_SECOND = sampleRate * 1 * 2;

  bubble.reset(text);
  status.textContent = `playing (${audioBuf.duration.toFixed(2)}s, ${decodedEvents.length} events)`;

  const src = ctx.createBufferSource();
  src.buffer = audioBuf;
  src.connect(ctx.destination);
  const startAt = ctx.currentTime + 0.05;
  src.start(startAt);

  for (const e of decodedEvents) {
    const tSec = e.audioOffsetBytes / SOURCE_BYTES_PER_SECOND;
    const fireAt = startAt + tSec;
    const delayMs = Math.max(0, (fireAt - ctx.currentTime) * 1000);
    setTimeout(() => handleEvent(e, text, puppet, bubble), delayMs);
  }

  src.onended = () => {
    puppet.setViseme(0);
    bubble.end();
    status.textContent = 'done';
  };
}

function handleEvent(e, text, puppet, bubble) {
  switch (e.eventId) {
    case SPEI_VISEME:
      puppet.setViseme(e.lParam & 0xffff);
      break;
    case SPEI_WORD_BOUNDARY:
      bubble.speak(e.lParam);
      break;
    // SPEI_SENTENCE_BOUNDARY, SPEI_PHONEME — reserved for future UI flourishes.
  }
}

// ---------------------------------------------------------------------------
// Wire-up
// ---------------------------------------------------------------------------
document.addEventListener('DOMContentLoaded', async () => {
  const $ = (id) => document.getElementById(id);
  const puppet = new Puppet($('puppet'), $('mouth'));
  const bubble = new Bubble($('bubble'));
  puppet.setCharacter('cat');

  $('character').addEventListener('change', (e) => puppet.setCharacter(e.target.value));

  // Each codec has its own native set of sample rates. Offering rates the
  // encoder doesn't actually support means it silently resamples (opus
  // pitches anything not in {8,12,16,24,48} kHz). Repopulate the dropdown
  // every time the format changes so the menu only shows rates that round-
  // trip cleanly. Defaults pick a middle-of-the-road rate per codec.
  const RATES = {
    'ogg+vorbis': { rates: [8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000], def: 22050 },
    'ogg+opus':   { rates: [8000, 12000, 16000, 24000, 48000],                       def: 24000 },
    'mp3':        { rates: [8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000], def: 22050 },
  };
  const fmtName = (hz) => hz % 1000 === 0 ? `${hz / 1000} kHz` : `${(hz / 1000).toFixed(3)} kHz`;
  function repopulateRates() {
    const sel = $('sample_rate');
    const prev = parseInt(sel.value, 10) || 0;
    const cfg = RATES[$('format').value] || RATES['ogg+vorbis'];
    sel.innerHTML = '';
    for (const hz of cfg.rates) {
      const o = document.createElement('option');
      o.value = String(hz);
      o.textContent = fmtName(hz);
      sel.appendChild(o);
    }
    // Try to keep the previous selection if it's still valid; otherwise
    // pick the format's default.
    sel.value = cfg.rates.includes(prev) ? String(prev) : String(cfg.def);
  }
  $('format').addEventListener('change', repopulateRates);
  repopulateRates();

  // Populate voice list.
  try {
    const r = await fetch('/voices');
    const voices = await r.json();
    const sel = $('voice');
    for (const v of voices) {
      const o = document.createElement('option');
      o.value = v.id;
      o.textContent = `${v.name} (${v.language})`;
      sel.appendChild(o);
    }
  } catch (e) {
    $('status').textContent = 'failed to load voices: ' + e;
  }

  // Pick up the public-trial api key the installer baked into keys.json,
  // so anyone hitting the bundled web client gets the rate-limited public
  // tier without having to know the key. Empty string = no default; the
  // request goes out unauthenticated and hits public_tier limits instead.
  try {
    const r = await fetch('/api/default-key');
    const j = await r.json();
    if (j.api_key && !$('apikey').value) {
      $('apikey').value = j.api_key;
      $('apikey').placeholder = 'public-trial key (rate limited per IP)';
    }
  } catch { /* server might not implement it; harmless */ }

  // Footer year stays current without us touching the HTML.
  const yearEl = $('year');
  if (yearEl) yearEl.textContent = String(new Date().getFullYear());

  $('form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const btn = $('speak');
    btn.disabled = true;
    try {
      // OR together the checked event bits to build the SAPI mask.
      // Each checkbox value is the bit-shifted SPEI_X constant.
      const eventsMask = Array.from(
        document.querySelectorAll('#events input[name="event"]:checked')
      ).reduce((m, cb) => m | parseInt(cb.value, 10), 0);

      await speak({
        text:        $('text').value,
        voice:       $('voice').value,
        format:      $('format').value,
        textType:    $('texttype').value,
        eventsMask,
        sampleRate:  parseInt($('sample_rate').value, 10),
        channels:    parseInt($('channels').value,    10),
        bits:        parseInt($('bits').value,        10),
        rate:        $('rate').value,
        volume:      $('volume').value,
        apiKey:      $('apikey').value,
        character:   $('character').value,
        status:      $('status'),
        puppet, bubble,
      });
    } finally {
      btn.disabled = false;
    }
  });
});
