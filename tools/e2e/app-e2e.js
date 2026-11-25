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
const { Obs, launchObs, makeChecker, parseArgs, sleep } = require('./lib');

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

    // tally: the app shows "ON AIR · <source>" / "PREVIEW · <source>" (read from its accessibility tree, no screenshots)
    await sleep(1000);
    let label = tallyLabel();
    check('app shows ON AIR for the source on program', /^(ON AIR|AO VIVO) · Niriun1$/.test(label), JSON.stringify(label));

    await obs.request('SetCurrentProgramScene', { sceneName: 'Other' });
    await sleep(1500);
    label = tallyLabel();
    check('app shows no tally when the source leaves program', label === '', JSON.stringify(label));

    await obs.request('SetStudioModeEnabled', { studioModeEnabled: true });
    await sleep(500);
    await obs.request('SetCurrentPreviewScene', { sceneName: 'Cam' });
    await sleep(1500);
    label = tallyLabel();
    check('app shows PREVIEW in Studio Mode', /^PREVIEW · Niriun1$/.test(label), JSON.stringify(label));
    await obs.request('SetStudioModeEnabled', { studioModeEnabled: false });

    // HEVC from the phone's own encoder (--hevc: the emulator's software HEVC encoder refuses the app's profile)
    if (args.hevc) {
      await obs.request('SetCurrentProgramScene', { sceneName: 'Cam' });
      await obs.request('SetInputSettings', { inputName: 'Niriun1', inputSettings: { video_format: 1 }, overlay: true });
      await sleep(9000);
      const sizeHevc = await obs.screenshot('Niriun1', path.join(OUT, 'app-video-hevc.png'));
      check('HEVC from the app is decoded', sizeHevc > 3000, `png base64 ${sizeHevc} bytes`);
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
