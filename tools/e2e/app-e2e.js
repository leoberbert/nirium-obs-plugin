#!/usr/bin/env node
// End-to-end test against the real Niriun Android app (an emulator or a phone on USB), through `adb forward`
// (the plugin's "USB" device type). Same OBS setup as e2e.js.
//
//   adb install -r app-debug.apk, grant camera/microphone/notifications, open Niriun and press Start
//   node tools/e2e/app-e2e.js --obs <portable OBS folder> [--serial emulator-5554] [--size 1280x720] [--hevc] [--out <dir>]
//
// Checks that the app's MediaCodec output (config packet + frames), Opus audio and tally work with the plugin. The
// tally is read from the app's own labels (uiautomator); nothing from the phone screen is captured.

const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');
const { Obs, launchObs, makeChecker, parseArgs, sleep, waitFor } = require('./lib');

const args = parseArgs(process.argv.slice(2));
if (typeof args.obs !== 'string') {
  console.error('usage: node app-e2e.js --obs <portable OBS folder> [--serial <adb serial>] [--out <dir>]');
  process.exit(2);
}
const OBS_DIR = path.resolve(args.obs);
const OUT = path.resolve(typeof args.out === 'string' ? args.out : 'e2e-out');
const SERIAL = typeof args.serial === 'string' ? args.serial : 'emulator-5554';
const SIZE = typeof args.size === 'string' ? args.size : '1280x720';
const { check, summary } = makeChecker();

/** Visible text of the app, from uiautomator (the app's own labels only). */
function phoneTexts() {
  const file = '/sdcard/niriun-ui.xml';
  execFileSync('adb', ['-s', SERIAL, 'shell', 'uiautomator', 'dump', file], { stdio: 'ignore' });
  const xml = execFileSync('adb', ['-s', SERIAL, 'shell', 'cat', file], { maxBuffer: 16 << 20 }).toString('utf8');
  execFileSync('adb', ['-s', SERIAL, 'shell', 'rm', file], { stdio: 'ignore' });
  return [...xml.matchAll(/text="([^"]*)"/g)].map((m) => m[1]).filter(Boolean);
}

/** Taps the center of the first (or last) visible node whose text matches [re]. */
function tapText(re, last) {
  const file = '/sdcard/niriun-ui.xml';
  execFileSync('adb', ['-s', SERIAL, 'shell', 'uiautomator', 'dump', file], { stdio: 'ignore' });
  const xml = execFileSync('adb', ['-s', SERIAL, 'shell', 'cat', file], { maxBuffer: 16 << 20 }).toString('utf8');
  execFileSync('adb', ['-s', SERIAL, 'shell', 'rm', file], { stdio: 'ignore' });
  const nodes = [...xml.matchAll(/text="([^"]*)"[^>]*bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"/g)].filter((m) => re.test(m[1]));
  const n = last ? nodes[nodes.length - 1] : nodes[0];
  if (!n) throw new Error(`no node matching ${re}; visible: ${JSON.stringify([...xml.matchAll(/text="([^"]+)"/g)].map((m) => m[1]))}`);
  const [x1, y1, x2, y2] = n.slice(2, 6).map(Number);
  execFileSync('adb', ['-s', SERIAL, 'shell', 'input', 'tap', String(Math.round((x1 + x2) / 2)), String(Math.round((y1 + y2) / 2))]);
}

/** Minimal PNG reader (8-bit RGB/RGBA, not interlaced), enough to sample pixels of an OBS screenshot. */
function readPng(file) {
  const zlib = require('zlib');
  const buf = fs.readFileSync(file);
  let off = 8, w = 0, h = 0, bpp = 0;
  const idat = [];
  while (off < buf.length) {
    const len = buf.readUInt32BE(off);
    const type = buf.toString('latin1', off + 4, off + 8);
    const data = buf.subarray(off + 8, off + 8 + len);
    if (type === 'IHDR') {
      w = data.readUInt32BE(0);
      h = data.readUInt32BE(4);
      bpp = data[9] === 6 ? 4 : 3;
    } else if (type === 'IDAT') idat.push(data);
    off += 12 + len;
  }
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = w * bpp;
  const px = Buffer.alloc(stride * h);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (stride + 1)];
    for (let i = 0; i < stride; i++) {
      const x = raw[y * (stride + 1) + 1 + i];
      const a = i >= bpp ? px[y * stride + i - bpp] : 0;
      const b = y > 0 ? px[(y - 1) * stride + i] : 0;
      const c = i >= bpp && y > 0 ? px[(y - 1) * stride + i - bpp] : 0;
      const p = a + b - c, pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
      const paeth = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      px[y * stride + i] = (x + (f === 1 ? a : f === 2 ? b : f === 3 ? (a + b) >> 1 : f === 4 ? paeth : 0)) & 255;
    }
  }
  return { w, h, pixel: (x, y) => [px[y * stride + x * bpp], px[y * stride + x * bpp + 1], px[y * stride + x * bpp + 2]] };
}

const tallyLabel = () => phoneTexts().find((t) => /^(ON AIR|AO VIVO|PREVIEW)\b/.test(t)) || '';

async function main() {
  fs.mkdirSync(OUT, { recursive: true });
  const obsProcess = launchObs(OBS_DIR, OUT);
  const obs = new Obs(OBS_DIR);
  let exitCode = 1;
  try {
    check('OBS started and obs-websocket answers', await obs.connect(90000));
    if (!obs.ws) return;

    await obs.reset(['Cam', 'Other']);
    await obs.request('CreateScene', { sceneName: 'Cam' });
    await obs.request('CreateScene', { sceneName: 'Other' });
    await obs.request('SetCurrentProgramScene', { sceneName: 'Cam' }); // an async source only gets frames while it is shown

    // The USB entry for the phone is listed once adb sees it.
    await obs.request('CreateInput', { sceneName: 'Other', inputName: 'Niriun0', inputKind: 'niriun_source', sceneItemEnabled: true });
    await obs.request('PressInputPropertiesButton', { inputName: 'Niriun0', propertyName: 'refresh' });
    const usb = (await obs.request('GetInputPropertiesListPropertyItems', { inputName: 'Niriun0', propertyName: 'device_list' })).propertyItems;
    check('adb device is listed as [USB]', usb.some((i) => i.itemName.includes('[USB]') && i.itemName.includes(SERIAL)), JSON.stringify(usb.map((i) => i.itemName)));
    await obs.request('RemoveInput', { inputName: 'Niriun0' });

    await obs.request('CreateInput', {
      sceneName: 'Cam',
      inputName: 'Niriun1',
      inputKind: 'niriun_source',
      sceneItemEnabled: true,
      inputSettings: { device_list: `adb:${SERIAL}`, cur_dev_type: 3, cur_dev_id: SERIAL, cur_dev_name: SERIAL, app_port: 4699, resolution_str: SIZE, fps: 30, activated: true },
    });

    await sleep(8000); // a slow encoder (emulator) needs seconds for its first picture
    const size = await obs.screenshot('Niriun1', path.join(OUT, 'app-video.png'));
    check('video from the app is decoded', size > 3000, `png base64 ${size} bytes`);
    const cams = (await obs.request('GetInputPropertiesListPropertyItems', { inputName: 'Niriun1', propertyName: 'camera' })).propertyItems;
    check('camera list comes from the app', cams.length > 0, JSON.stringify(cams.map((i) => i.itemName)));
    console.log(`info  audio meter peak ${(obs.meters['Niriun1'] || 0).toFixed(4)} (an emulator microphone is silent)`);

    // tally: the app shows "ON AIR" / "PREVIEW" (read from its accessibility tree, no screenshots)
    await sleep(1000);
    let label = tallyLabel();
    check('app shows ON AIR for the source on program', /^(ON AIR|AO VIVO)$/.test(label), JSON.stringify(label));

    await obs.request('SetCurrentProgramScene', { sceneName: 'Other' });
    await sleep(1500);
    label = tallyLabel();
    check('app shows no tally when the source leaves program', label === '', JSON.stringify(label));

    await obs.request('SetStudioModeEnabled', { studioModeEnabled: true });
    await sleep(500);
    await obs.request('SetCurrentPreviewScene', { sceneName: 'Cam' });
    await sleep(1500);
    label = tallyLabel();
    check('app shows PREVIEW in Studio Mode', /^PREVIEW$/.test(label), JSON.stringify(label));
    await obs.request('SetStudioModeEnabled', { studioModeEnabled: false });

    // HEVC from the phone's own encoder (--hevc: the emulator's software HEVC encoder refuses the app's profile)
    if (args.hevc) {
      await obs.request('SetCurrentProgramScene', { sceneName: 'Cam' });
      await obs.request('SetInputSettings', { inputName: 'Niriun1', inputSettings: { video_format: 1 }, overlay: true });
      await sleep(9000);
      const sizeHevc = await obs.screenshot('Niriun1', path.join(OUT, 'app-video-hevc.png'));
      check('HEVC from the app is decoded', sizeHevc > 3000, `png base64 ${sizeHevc} bytes`);
    }

    // Stop on the phone: the app sends 0x31 first, and the source keeps a black frame or the offline card
    // (--stop black|offline says which one the app is set to; the app's default is the offline card).
    const expectCard = args.stop !== 'black';
    await obs.request('SetCurrentProgramScene', { sceneName: 'Other' }); // not on air: Stop needs no second confirmation
    await sleep(1500);
    // The controls hide 5 s after the last tap and an emulator needs seconds to dump the UI, so tap by position
    // (Pixel 9, 1080x2424): the picture first, then the Stop button at the bottom right.
    const tap = (x, y) => execFileSync('adb', ['-s', SERIAL, 'shell', 'input', 'tap', String(x), String(y)]);
    tap(540, 1200);
    await sleep(700);
    tap(914, 2251);
    check('Stop asks for confirmation', !!(await waitFor(async () => phoneTexts().some((t) => /^(Stop broadcasting\?|Parar a transmissão\?)$/.test(t)), 20000, 500)));
    check('Stop confirmed in the dialog', !!(await waitFor(async () => { try { tapText(/^(Stop|Parar)$/, true); return true; } catch { return false; } }, 20000, 1000))); // the dialog's button
    await obs.request('SetCurrentProgramScene', { sceneName: 'Cam' }); // an async source only shows frames while it is on screen
    await sleep(3000);
    const stopShot = path.join(OUT, 'app-stopped.png');
    await obs.screenshot('Niriun1', stopShot, 320, 180);
    const png = readPng(stopShot);
    const corner = png.pixel(5, 5);
    // the card is a ring around the center, so the brightest pixel of the middle row is on it (the screenshot is stretched to 16:9)
    let ring = [0, 0, 0];
    for (let x = 0; x < png.w; x++) { const p = png.pixel(x, Math.floor(png.h / 2)); if (p[0] + p[1] + p[2] > ring[0] + ring[1] + ring[2]) ring = p; }
    const dark = (p) => p[0] + p[1] + p[2] < 30; // black is 0; the card's purple is about 50
    if (expectCard) {
      check('offline card: purple background and light ring', corner[2] > corner[0] && !dark(corner) && ring[0] + ring[1] + ring[2] > 300, `corner ${corner} ring ${ring}`);
    } else {
      check('black screen after Stop', dark(corner) && dark(ring), `corner ${corner} ring ${ring}`);
    }
  } catch (e) {
    check('test run completed without exception', false, e.message);
  } finally {
    obsProcess.kill();
    exitCode = summary();
    process.exit(exitCode);
  }
}

main();
