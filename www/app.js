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
// Ogg parsing
// ---------------------------------------------------------------------------
// Walk Ogg pages, reassemble packets per logical bitstream, identify which
// stream is our event channel by the BOS magic, and yield events with their
// audio-sample offsets (granulepos on event packets = current audio sample
// count when the event was emitted).
const EVENT_MAGIC = '\x80sapievents\x00';

function parseOggEvents(buffer) {
  const view = new DataView(buffer);
  const bytes = new Uint8Array(buffer);
  let pos = 0;

  const streams = new Map(); // serial → { isEvents, partial: Uint8Array, packets: [{data, granule}] }
  const events = [];
  let eventSerial = null;

  while (pos + 27 <= bytes.length) {
    if (bytes[pos] !== 0x4f || bytes[pos+1] !== 0x67 || bytes[pos+2] !== 0x67 || bytes[pos+3] !== 0x53) {
      // Not aligned on OggS — abort gracefully (audio decoder will get the bytes raw).
      break;
    }
    const headerType = bytes[pos+5];
    // 64-bit granulepos LE; we use the low 32 bits which is plenty for any single
    // synthesis call (>2 hours @ 44.1kHz).
    const granuleLo = view.getUint32(pos+6, true);
    const serial = view.getUint32(pos+14, true);
    const segCount = bytes[pos+26];
    if (pos + 27 + segCount > bytes.length) break;
    const segs = bytes.slice(pos+27, pos+27+segCount);
    let payloadStart = pos + 27 + segCount;

    let stream = streams.get(serial);
    if (!stream) {
      stream = { isEvents: false, partial: null, packetGranule: granuleLo };
      streams.set(serial, stream);
    }

    let segOff = payloadStart;
    let curBuf = stream.partial;
    let curStart = segOff;
    for (let i = 0; i < segCount; ++i) {
      const len = segs[i];
      segOff += len;
      if (len < 255) {
        // Packet ends here.
        const packetBytes = bytes.slice(curStart, segOff);
        const data = curBuf
          ? concat(curBuf, packetBytes)
          : packetBytes;
        curBuf = null;
        curStart = segOff;
        // Identify event stream by first packet content.
        if (eventSerial === null && startsWithMagic(data, EVENT_MAGIC)) {
          eventSerial = serial;
          stream.isEvents = true;
          continue; // BOS magic packet itself isn't an event payload
        }
        if (stream.isEvents) {
          // granulepos is recorded against the LAST packet that ends on this page
          // — which is the convention we use server-side.
          events.push({ data, granule: granuleLo });
        }
      }
      // (255-len segments continue into the next segment; accumulate via curBuf)
    }
    if (curStart < segOff) {
      // Trailing segment(s) carried over to the next page.
      const tail = bytes.slice(curStart, segOff);
      stream.partial = curBuf ? concat(curBuf, tail) : tail;
    } else {
      stream.partial = null;
    }

    pos = payloadStart;
    for (let i = 0; i < segCount; ++i) pos += segs[i];
  }
  return events;
}

function concat(a, b) {
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

function startsWithMagic(buf, magic) {
  if (buf.length < magic.length) return false;
  for (let i = 0; i < magic.length; ++i) {
    if (buf[i] !== magic.charCodeAt(i)) return false;
  }
  return true;
}

// SPSERIALIZEDEVENT: 24 bytes
//   eEventId (i16) elParamType (i16) ulStreamNum (u32)
//   ullAudioStreamOffset (u64, low half used) wParam (u32) lParam (i32)
function decodeEvent(packet) {
  if (packet.length < 24) return null;
  const v = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
  return {
    eventId: v.getInt16(0, true),
    paramType: v.getInt16(2, true),
    streamNum: v.getUint32(4, true),
    audioOffsetBytes: v.getUint32(8, true),
    wParam: v.getUint32(16, true),
    lParam: v.getInt32(20, true),
  };
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
    const sprite = this.character.visemeMap[visemeId] ?? 0;
    if (!sprite) {
      this.mouth.style.display = 'none';
      return;
    }
    this.mouth.style.display = '';
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
  const { text, voice, format, rate, volume, apiKey, character, status, puppet, bubble } = opts;
  status.textContent = 'requesting…';

  const params = new URLSearchParams({
    text, format, rate, volume,
    events: 'all', multiplex: 'true',
    sample_rate: '22050', channels: '1', bits: '16',
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

  status.textContent = 'decoding…';
  const ctx = getAudio();
  if (ctx.state === 'suspended') await ctx.resume();

  // Native audio decode of the multiplexed Ogg. Most browsers' vorbis/opus
  // decoders happily ignore unknown logical streams in the same container.
  let audioBuf;
  try {
    audioBuf = await ctx.decodeAudioData(arrayBuf.slice(0));
  } catch (e) {
    status.textContent = 'decode failed (browser may not support this format): ' + e;
    return;
  }

  // Extract event packets from the multiplexed Ogg.
  const eventPackets = (format === 'ogg' || format === 'ogg+opus')
    ? parseOggEvents(arrayBuf)
    : [];
  const decodedEvents = eventPackets
    .map(p => Object.assign(decodeEvent(p.data) ?? {}, { granule: p.granule }))
    .filter(e => e.eventId !== undefined);

  // Convert granulepos → wall time. For ogg+vorbis at sample_rate, granule is samples.
  // For ogg+opus, granule is at 48kHz internally (server uses 48000 for opus).
  const sampleRate = (format === 'ogg+opus') ? 48000 : 22050;

  bubble.reset(text);
  status.textContent = `playing (${audioBuf.duration.toFixed(2)}s, ${decodedEvents.length} events)`;

  const src = ctx.createBufferSource();
  src.buffer = audioBuf;
  src.connect(ctx.destination);
  const startAt = ctx.currentTime + 0.05;
  src.start(startAt);

  // Schedule events relative to startAt.
  for (const e of decodedEvents) {
    const tSec = e.granule / sampleRate;
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
