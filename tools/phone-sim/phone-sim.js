#!/usr/bin/env node
// Stand-in for the Niriun phone app, to test the OBS plugin without a phone.
//
//   node tools/phone-sim/phone-sim.js [--name "Test Phone"] [--no-audio] [--no-beacon] [--log events.jsonl]
//                                     [--rotate-after 6] [--drop-idle]
//
// It listens on TCP 4699 (control) and 4697 (probe) and multicasts the hello beacon, like the app does, then:
//   * sends hello, sizes and the camera list when a client connects;
//   * on START starts ffmpeg (testsrc2 + libx264/libx265, sine + libopus) and streams real H.264/HEVC Annex-B and
//     Opus packets with the same framing as the app (config packet first with ts 0, one slice NAL per frame);
//   * records everything the client sends (START, ACK, controls, tally, no-op) as JSON lines on stdout / --log;
//   * --rotate-after N swaps width/height after N seconds with a fresh config packet (phone rotation);
//   * --drop-idle drops the client after 10 s without any packet, like the app's idle timer.
// Needs ffmpeg in PATH.

const net = require('net');
const dgram = require('dgram');
const os = require('os');
const fs = require('fs');
const { spawn } = require('child_process');

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, all) => {
    if (a.startsWith('--')) acc.push([a.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
    return acc;
  }, []),
);
const NAME = typeof args.name === 'string' ? args.name : 'Niriun Test Phone';
const LOG = typeof args.log === 'string' ? fs.createWriteStream(args.log, { flags: 'w' }) : null;
const ROTATE_AFTER = Number(args['rotate-after'] || 0);
const DUMP = typeof args.dump === 'string' ? fs.createWriteStream(args.dump) : null; // raw video payloads, for ffmpeg
const UUID = Buffer.from('00112233445566778899AABBCCDDEEFF', 'hex');

const T = { HELLO: 1, AUDIO: 7, VIDEO: 8, ACK: 9, SIZES: 0x0a, START: 0x0b, CONTROL: 0x10, PEER: 0x11, CAMERAS: 0x18, USAGE: 0x1a, TALLY: 0x30 };

function event(obj) {
  const line = JSON.stringify({ t: Date.now(), ...obj });
  console.log(line);
  if (LOG) LOG.write(line + '\n');
}

function header(type, total) {
  const b = Buffer.alloc(4);
  b.writeUInt32LE(((type << 24) | (total & 0xffffff)) >>> 0);
  return b;
}
const packet = (type, body) => Buffer.concat([header(type, 4 + body.length), body]);

function nameField(text) {
  const b = Buffer.alloc(256);
  Buffer.from(text, 'utf8').copy(b, 0, 0, 255);
  return b;
}

function helloPacket() {
  const body = Buffer.alloc(0x118 - 4);
  UUID.copy(body, 0);
  body.writeUInt16LE(2, 16);
  body.writeUInt16LE(2, 18);
  nameField(NAME).copy(body, 20);
  return Buffer.concat([header(T.HELLO, 0x118), body]);
}

function sizesPacket() {
  const modes = [[640, 480, 30], [1280, 720, 30], [1920, 1080, 30], [1920, 1080, 60]];
  const body = Buffer.alloc(8 + modes.length * 12);
  body.writeUInt32LE(args['no-audio'] ? 0 : 1, 0);
  body.writeUInt32LE(modes.length, 4);
  modes.forEach(([w, h, f], i) => {
    body.writeInt32LE(w, 8 + i * 12);
    body.writeInt32LE(h, 12 + i * 12);
    body.writeFloatLE(f, 16 + i * 12);
  });
  return packet(T.SIZES, body);
}

const CAMERAS = ['Back camera', 'Front camera'];
let selectedCamera = 0;
function camerasPacket() {
  const body = Buffer.alloc(8 + 256 * CAMERAS.length);
  body.writeUInt32LE(CAMERAS.length, 0);
  body.writeUInt32LE(selectedCamera, 4);
  CAMERAS.forEach((n, i) => nameField(n).copy(body, 8 + i * 256));
  return packet(T.CAMERAS, body);
}

// ---- encoders ----------------------------------------------------------------------------------------

const START_CODE = Buffer.from([0, 0, 0, 1]);

// Splits an Annex-B byte stream into NAL units (without start codes), calling onNal for each complete one.
function nalSplitter(onNal) {
  let pending = Buffer.alloc(0);
  const find = (buf, from) => {
    for (let i = from; i + 2 < buf.length; i++) {
      if (buf[i] === 0 && buf[i + 1] === 0 && buf[i + 2] === 1) return i;
    }
    return -1;
  };
  return {
    push(chunk) {
      pending = Buffer.concat([pending, chunk]);
      let start = find(pending, 0);
      if (start < 0) return;
      for (;;) {
        const next = find(pending, start + 3);
        if (next < 0) break;
        let end = next;
        if (end > start + 3 && pending[end - 1] === 0) end--; // the 4-byte start code's leading zero
        onNal(pending.subarray(start + 3, end));
        start = next;
      }
      pending = pending.subarray(start);
    },
  };
}

class Streamer {
  constructor(sock, req) {
    this.sock = sock;
    this.req = req;
    this.t0 = Date.now();
    this.frames = 0;
    this.configSent = false;
    this.procs = [];
    this.stopped = false;
    this.startVideo(req.width, req.height);
    if (req.audio && !args['no-audio']) this.startAudio();
    if (ROTATE_AFTER > 0) {
      this.rotateTimer = setTimeout(() => {
        event({ ev: 'rotate', width: req.height, height: req.width });
        this.restartVideo(req.height, req.width);
      }, ROTATE_AFTER * 1000);
    }
  }

  write(buf) {
    if (!this.stopped && !this.sock.destroyed) this.sock.write(buf);
  }

  startVideo(w, h) {
    const hevc = this.req.codec === 2;
    const ff = [
      '-hide_banner', '-loglevel', 'error', '-re',
      '-f', 'lavfi', '-i', `testsrc2=size=${w}x${h}:rate=${this.req.fps}`,
      '-pix_fmt', 'yuv420p',
      ...(hevc
        ? ['-c:v', 'libx265', '-preset', 'ultrafast', '-tune', 'zerolatency', '-x265-params', 'log-level=error:keyint=60:frame-threads=1']
        : ['-c:v', 'libx264', '-preset', 'ultrafast', '-tune', 'zerolatency', '-profile:v', 'baseline', '-g', '60', '-bf', '0',
           '-threads', '1', '-x264-params', 'aud=0']),
      '-f', hevc ? 'hevc' : 'h264', 'pipe:1',
    ];
    const p = spawn('ffmpeg', ff, { stdio: ['ignore', 'pipe', 'inherit'] });
    this.videoProc = p;
    this.procs.push(p);
    this.configNals = [];
    this.au = [];
    this.configSentForProc = false;
    const split = nalSplitter((nal) => this.onNal(hevc, nal, p));
    p.stdout.on('data', (d) => split.push(d));
    p.on('error', (e) => event({ ev: 'error', msg: `ffmpeg: ${e.message}` }));
  }

  restartVideo(w, h) {
    const old = this.videoProc;
    this.videoProc = null;
    if (old) old.kill();
    this.t0Offset = (Date.now() - this.t0) / 1000;
    this.startVideo(w, h);
  }

  onNal(hevc, nal, proc) {
    if (this.stopped || proc !== this.videoProc || nal.length === 0) return;
    const type = hevc ? (nal[0] >> 1) & 0x3f : nal[0] & 0x1f;
    const isParam = hevc ? type >= 32 && type <= 34 : type === 7 || type === 8;
    const isSlice = hevc ? type <= 21 : type === 1 || type === 5;
    if (isParam) {
      if (!this.configSentForProc) this.configNals.push(Buffer.concat([START_CODE, nal]));
      return;
    }
    if (!isSlice) return; // SEI etc.
    // One packet per access unit, like a MediaCodec output buffer: a new picture starts at the slice whose
    // first_mb_in_slice (H.264) / first_slice_segment_in_pic_flag (HEVC) is set, i.e. the first payload bit.
    const first = hevc ? (nal[2] & 0x80) !== 0 : (nal[1] & 0x80) !== 0;
    if (first) this.flushAu();
    if (this.au.length === 0) this.auTs = (Date.now() - this.t0) / 1000;
    this.au.push(Buffer.concat([START_CODE, nal]));
  }

  flushAu() {
    if (this.au.length === 0) return;
    if (!this.configSentForProc) {
      this.configSentForProc = true;
      const cfg = Buffer.concat(this.configNals);
      this.write(packet(T.VIDEO, Buffer.concat([Buffer.alloc(4), cfg]))); // ts 0: like the MediaCodec config buffer
      if (DUMP) DUMP.write(cfg);
      event({ ev: 'config_sent', bytes: cfg.length });
    }
    const ts = Buffer.alloc(4);
    ts.writeFloatLE(this.auTs);
    this.write(packet(T.VIDEO, Buffer.concat([ts, ...this.au])));
    if (DUMP) DUMP.write(Buffer.concat(this.au));
    this.au = [];
    this.frames++;
  }

  startAudio() {
    const ff = [
      '-hide_banner', '-loglevel', 'error', '-re',
      '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
      '-ac', '1', '-c:a', 'libopus', '-application', 'audio', '-frame_duration', '20', '-b:a', '48k',
      '-f', 'ogg', 'pipe:1',
    ];
    const p = spawn('ffmpeg', ff, { stdio: ['ignore', 'pipe', 'inherit'] });
    this.procs.push(p);
    let buf = Buffer.alloc(0);
    let carry = Buffer.alloc(0);
    let skip = 2; // OpusHead, OpusTags
    p.stdout.on('data', (d) => {
      buf = Buffer.concat([buf, d]);
      while (buf.length >= 27) {
        if (buf.toString('latin1', 0, 4) !== 'OggS') { buf = buf.subarray(1); continue; }
        const nseg = buf[26];
        if (buf.length < 27 + nseg) return;
        let size = 0;
        for (let i = 0; i < nseg; i++) size += buf[27 + i];
        if (buf.length < 27 + nseg + size) return;
        let off = 27 + nseg;
        for (let i = 0; i < nseg; i++) {
          const len = buf[27 + i];
          carry = Buffer.concat([carry, buf.subarray(off, off + len)]);
          off += len;
          if (len < 255) { // packet complete
            if (skip > 0) skip--;
            else this.write(packet(T.AUDIO, Buffer.concat([Buffer.alloc(4), carry])));
            carry = Buffer.alloc(0);
          }
        }
        buf = buf.subarray(27 + nseg + size);
      }
    });
  }

  stop() {
    this.stopped = true;
    clearTimeout(this.rotateTimer);
    for (const p of this.procs) p.kill();
  }
}

// ---- control server ----------------------------------------------------------------------------------

let client = null;
let streamer = null;

function onPacket(type, body) {
  switch (type) {
    case T.START: {
      const req = {
        audio: body.readUInt32LE(0) !== 0,
        codec: body.readUInt32LE(4),
        width: body.readUInt32LE(8),
        height: body.readUInt32LE(12),
        fps: body.length >= 20 ? body.readFloatLE(16) : 30,
      };
      event({ ev: 'start', ...req });
      if (streamer) streamer.stop();
      streamer = new Streamer(client, req);
      break;
    }
    case T.ACK:
      event({ ev: 'ack', ts: body.readFloatLE(0), decode_ms: body.length >= 12 ? body.readUInt32LE(4) : null });
      break;
    case T.CONTROL: {
      const id = body.readUInt32LE(0);
      const raw = body.readUInt32LE(4);
      if (id === 14) {
        selectedCamera = raw % CAMERAS.length;
        client.write(camerasPacket());
      }
      event({ ev: 'control', id, raw });
      break;
    }
    case T.TALLY: {
      const state = body.readUInt32LE(0);
      const name = body.length > 4 ? body.toString('utf8', 4, 4 + 256).replace(/\0.*$/s, '') : '';
      event({ ev: 'tally', state: ['off', 'preview', 'program'][state] ?? state, name, length: body.length + 4 });
      break;
    }
    case T.HELLO:
      event({ ev: 'noop' });
      break;
    case T.USAGE:
    case T.PEER:
      event({ ev: 'other', type });
      break;
    default:
      event({ ev: 'unknown', type });
  }
}

const server = net.createServer((sock) => {
  if (client) {
    sock.destroy();
    return;
  }
  client = sock;
  sock.setNoDelay(true);
  event({ ev: 'connected', peer: sock.remoteAddress });
  sock.write(helloPacket());
  sock.write(sizesPacket());
  sock.write(camerasPacket());

  let pending = Buffer.alloc(0);
  let lastRx = Date.now();
  const idle = args['drop-idle']
    ? setInterval(() => {
        if (Date.now() - lastRx > 10000) {
          event({ ev: 'idle_drop' });
          sock.destroy();
        }
      }, 500)
    : null;

  sock.on('data', (d) => {
    lastRx = Date.now();
    pending = Buffer.concat([pending, d]);
    while (pending.length >= 4) {
      const len = pending.readUInt32LE(0) & 0xffffff;
      const type = pending[3];
      if (len < 4 || len > 4_000_000) {
        event({ ev: 'bad_length', len });
        sock.destroy();
        return;
      }
      if (pending.length < len) break;
      onPacket(type, pending.subarray(4, len));
      pending = pending.subarray(len);
    }
  });
  const done = () => {
    if (client !== sock) return;
    client = null;
    clearInterval(idle);
    if (streamer) streamer.stop();
    streamer = null;
    event({ ev: 'disconnected' });
  };
  sock.on('close', done);
  sock.on('error', () => {});
});
server.listen(4699, '::', () => event({ ev: 'listening', port: 4699 }));

// Probe port: hello, then close.
net
  .createServer((sock) => {
    sock.on('error', () => {});
    sock.end(helloPacket());
    event({ ev: 'probe', peer: sock.remoteAddress });
  })
  .listen(4697, '::', () => event({ ev: 'listening', port: 4697 }));

// Beacon: only while nobody is connected, on every IPv4 interface, every 2 s.
if (!args['no-beacon']) {
  setInterval(() => {
    if (client) return;
    const hello = helloPacket();
    for (const list of Object.values(os.networkInterfaces())) {
      for (const ifc of list || []) {
        if (ifc.family !== 'IPv4' || ifc.internal) continue;
        const s = dgram.createSocket({ type: 'udp4', reuseAddr: true });
        s.bind(0, ifc.address, () => {
          try {
            s.setMulticastInterface(ifc.address);
            s.setMulticastTTL(1);
            s.send(hello, 4698, '239.255.188.58', () => s.close());
          } catch {
            s.close();
          }
        });
        s.on('error', () => s.close());
      }
    }
  }, 2000);
}

process.on('SIGINT', () => process.exit(0));
