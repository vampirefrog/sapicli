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
// { audio: Int16Array (interleaved if stereo), events: Uint8Array[] }.
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
        if (st === MUX_STREAM_AUDIO) {
          // PCM is int16. Copy out (slice() detaches from HEAP).
          const samples = written >> 1;
          audioChunks.push(new Int16Array(m.HEAP16.buffer, outPtr, samples).slice());
        } else {
          eventBlobs.push(m.HEAPU8.slice(outPtr, outPtr + written));
        }
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

  // Concatenate audio chunks.
  const totalSamples = audioChunks.reduce((s, c) => s + c.length, 0);
  const audio = new Int16Array(totalSamples);
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
  const { text, voice, format, rate, volume, apiKey, status, puppet, bubble } = opts;
  status.textContent = 'requesting…';

  // Format → muxaudio codec name + per-codec sample rate. Opus only accepts
  // 8/12/16/24/48 kHz, so the server snaps requests to 24000 for opus; the
  // decoder outputs PCM at whatever rate the codec used.
  let codec, sampleRate;
  if (format === 'ogg' || format === 'ogg+vorbis') { codec = 'vorbis'; sampleRate = 22050; }
  else if (format === 'ogg+opus')                  { codec = 'opus';   sampleRate = 24000; }
  else if (format === 'mp3')                       { codec = 'mp3';    sampleRate = 22050; }
  else { status.textContent = `unsupported format: ${format}`; return; }

  const params = new URLSearchParams({
    text, format, rate, volume,
    events: 'all', multiplex: 'true',
    sample_rate: String(sampleRate), channels: '1', bits: '16',
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
  const inputBytes = new Uint8Array(arrayBuf);

  status.textContent = 'decoding (muxaudio wasm)…';
  let decoded;
  try {
    decoded = await decodeWithMuxaudio(codec, inputBytes);
  } catch (e) {
    status.textContent = 'mux decode failed: ' + e.message;
    return;
  }
  const { audio, events: eventBlobs } = decoded;

  const ctx = getAudio();
  if (ctx.state === 'suspended') await ctx.resume();

  // Build an AudioBuffer from the decoded PCM (mono, int16 → float32).
  if (audio.length === 0) {
    status.textContent = `decoded 0 audio samples (${eventBlobs.length} events)`;
    return;
  }
  const audioBuf = ctx.createBuffer(1, audio.length, sampleRate);
  const ch = audioBuf.getChannelData(0);
  for (let i = 0; i < audio.length; ++i) ch[i] = audio[i] / 32768;

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

  $('form').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const btn = $('speak');
    btn.disabled = true;
    try {
      await speak({
        text: $('text').value,
        voice: $('voice').value,
        format: $('format').value,
        rate: $('rate').value,
        volume: $('volume').value,
        apiKey: $('apikey').value,
        character: $('character').value,
        status: $('status'),
        puppet, bubble,
      });
    } finally {
      btn.disabled = false;
    }
  });
});
