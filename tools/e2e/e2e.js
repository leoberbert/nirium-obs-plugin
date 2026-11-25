#!/usr/bin/env node
// End-to-end test: a real OBS Studio (portable copy) with the plugin, against tools/phone-sim.
//
//   1. Copy an OBS Studio install to a scratch folder and put the plugin in it
//      (obs-plugins/64bit/niriun-obs.dll, data/obs-plugins/niriun-obs/locale).
//   2. Enable obs-websocket there (config/obs-studio/plugin_config/obs-websocket/config.json: server on, port 4466, no auth).
//   3. node tools/e2e/e2e.js --obs <that folder> [--out <folder for screenshots>]
//
// It starts the phone simulator and OBS, creates a Niriun source through obs-websocket and checks video, audio,
// tally (program / preview / off), camera controls, discovery, rotation, reconnection and codec changes.
// Needs Node 22+ and ffmpeg in PATH.

const { spawn } = require('child_process');
const path = require('path');
const { Obs, launchObs, makeChecker, parseArgs, sleep, waitFor } = require('./lib');

const args = parseArgs(process.argv.slice(2));
if (typeof args.obs !== 'string') {
  console.error('usage: node e2e.js --obs <portable OBS folder> [--out <dir>]');
  process.exit(2);
}
const OBS_DIR = path.resolve(args.obs);
const OUT = path.resolve(typeof args.out === 'string' ? args.out : 'e2e-out');
const { check, summary } = makeChecker();

class Sim {
  constructor(extra = []) {
    this.events = [];
    this.proc = spawn(process.execPath, [path.join(__dirname, '..', 'phone-sim', 'phone-sim.js'), ...extra], {
      stdio: ['ignore', 'pipe', 'inherit'],
    });
    let buf = '';
    this.proc.stdout.on('data', (d) => {
      buf += d;
      let i;
      while ((i = buf.indexOf('\n')) >= 0) {
        try {
          this.events.push(JSON.parse(buf.slice(0, i)));
        } catch {}
        buf = buf.slice(i + 1);
      }
    });
  }
  count(ev, pred = () => true) {
    return this.events.filter((e) => e.ev === ev && pred(e)).length;
  }
  async waitEvent(ev, pred = () => true, ms = 15000, after = 0) {
    return waitFor(() => this.events.slice(after).find((e) => e.ev === ev && pred(e)), ms);
  }
  mark() {
    return this.events.length;
  }
  kill() {
    this.proc.kill();
  }
}

async function main() {
  const sim = new Sim(['--rotate-after', '14', '--drop-idle']);
  await sim.waitEvent('listening');

  const obsProcess = launchObs(OBS_DIR, OUT);
  const obs = new Obs(OBS_DIR);
  let exitCode = 1;
  try {
    check('OBS started and obs-websocket answers', await obs.connect(90000));
    if (!obs.ws) return;

    await obs.reset(['Cam', 'Other']);
    await obs.request('CreateScene', { sceneName: 'Cam' });
    await obs.request('CreateScene', { sceneName: 'Other' });
    await obs.request('SetCurrentProgramScene', { sceneName: 'Cam' });

    // ---- 1. discovery before anything is connected: multicast beacon, then TCP probe -----------------------
    // An idle (not activated) source starts the discovery; the simulator beacons every 2 s while nobody is connected.
    await obs.request('CreateInput', { sceneName: 'Other', inputName: 'Niriun0', inputKind: 'niriun_source', sceneItemEnabled: true });
    const listed = await waitFor(async () => {
      const r = await obs.request('GetInputPropertiesListPropertyItems', { inputName: 'Niriun0', propertyName: 'device_list' });
      return r.propertyItems.find((i) => i.itemName.startsWith('Niriun Test Phone [WiFi]'));
    }, 8000, 500);
    check('multicast beacon: phone appears in the device list before any connection', !!listed && sim.count('connected') === 0, listed ? listed.itemName : 'not listed');
    const probeMark = sim.mark();
    await obs.request('PressInputPropertiesButton', { inputName: 'Niriun0', propertyName: 'refresh' });
    check('Refresh probes the local subnet on TCP 4697', !!(await sim.waitEvent('probe', () => true, 5000, probeMark)));
    await obs.request('RemoveInput', { inputName: 'Niriun0' });

    const created = sim.mark();
    await obs.request('CreateInput', {
      sceneName: 'Cam',
      inputName: 'Niriun1',
      inputKind: 'niriun_source',
      sceneItemEnabled: true,
      inputSettings: { device_list: 'wifi_manual', wifi_ip: '127.0.0.1', app_port: 4699, cur_dev_type: 1, cur_dev_ip: '127.0.0.1', cur_dev_name: '127.0.0.1', activated: true },
    });

    // ---- 2. connection, START, tally ----------------------------------------------------------------------
    check('plugin connected to the phone', !!(await sim.waitEvent('connected', () => true, 15000, created)));
    const start = await sim.waitEvent('start', () => true, 15000, created);
    check('START requested 1920x1080 @30 H.264 with audio', !!start && start.width === 1920 && start.height === 1080 && start.fps === 30 && start.codec === 1 && start.audio === true, JSON.stringify(start));
    const tallyProgram = await sim.waitEvent('tally', (e) => e.state === 'program', 8000, created);
    check('tally = program sent with the source name', !!tallyProgram && tallyProgram.name === 'Niriun1' && tallyProgram.length === 264, JSON.stringify(tallyProgram));

    // ---- 3. video, ACK, audio -------------------------------------------------------------------------------
    await sleep(2500);
    const size = await obs.screenshot('Niriun1', path.join(OUT, 'video-1080p.png'));
    check('decoded video is not blank', size > 20000, `png base64 ${size} bytes`);
    const acks = sim.count('ack');
    check('plugin acknowledges video packets', acks > 30, `${acks} acks`);
    const peak = obs.meters['Niriun1'] || 0;
    check('Opus audio reaches OBS (non-silent meter)', peak > 0.01, `peak ${peak.toFixed(3)}`);

    // ---- 4. lists --------------------------------------------------------------------------------------------
    const items = (await obs.request('GetInputPropertiesListPropertyItems', { inputName: 'Niriun1', propertyName: 'device_list' })).propertyItems;
    check('device list contains the phone', items.some((i) => i.itemName.startsWith('Niriun Test Phone [WiFi]')), JSON.stringify(items.map((i) => i.itemName)));
    const cams = (await obs.request('GetInputPropertiesListPropertyItems', { inputName: 'Niriun1', propertyName: 'camera' })).propertyItems;
    check('camera list comes from the phone', cams.length === 2 && cams[0].itemName === 'Back camera', JSON.stringify(cams.map((i) => i.itemName)));

    // ---- 5. rotation: new SPS/size mid-stream ---------------------------------------------------------------
    await sim.waitEvent('rotate', () => true, 20000, created);
    await sleep(3000);
    const sceneItems = (await obs.request('GetSceneItemList', { sceneName: 'Cam' })).sceneItems;
    const tr = (await obs.request('GetSceneItemTransform', { sceneName: 'Cam', sceneItemId: sceneItems[0].sceneItemId })).sceneItemTransform;
    check('picture switched to 1080x1920 after the phone rotated', tr.sourceWidth === 1080 && tr.sourceHeight === 1920, `${tr.sourceWidth}x${tr.sourceHeight}`);
    const sizeRot = await obs.screenshot('Niriun1', path.join(OUT, 'video-rotated.png'));
    check('rotated video is not blank', sizeRot > 15000, `png base64 ${sizeRot} bytes`);

    // ---- 6. camera controls via the properties buttons ------------------------------------------------------
    for (const [button, id] of [['torch', 2], ['mirror', 3], ['lock', 1]]) {
      const m = sim.mark();
      await obs.request('PressInputPropertiesButton', { inputName: 'Niriun1', propertyName: button });
      check(`${button} button sends control ${id}`, !!(await sim.waitEvent('control', (e) => e.id === id, 3000, m)));
    }

    // ---- 7. tally transitions -------------------------------------------------------------------------------
    let m = sim.mark();
    await obs.request('SetCurrentProgramScene', { sceneName: 'Other' });
    check('tally = off when the scene leaves program', !!(await sim.waitEvent('tally', (e) => e.state === 'off', 5000, m)));

    await obs.request('SetStudioModeEnabled', { studioModeEnabled: true });
    await sleep(500);
    m = sim.mark();
    await obs.request('SetCurrentPreviewScene', { sceneName: 'Cam' });
    check('tally = preview in Studio Mode', !!(await sim.waitEvent('tally', (e) => e.state === 'preview', 5000, m)));

    m = sim.mark();
    await obs.request('TriggerStudioModeTransition');
    check('tally = program after the transition', !!(await sim.waitEvent('tally', (e) => e.state === 'program', 8000, m)));
    await obs.request('SetStudioModeEnabled', { studioModeEnabled: false });

    // ---- 8. settings change reconnects with the new START ---------------------------------------------------
    m = sim.mark();
    await obs.request('SetInputSettings', { inputName: 'Niriun1', inputSettings: { resolution_str: '1280x720', fps: 60 }, overlay: true });
    const s2 = await sim.waitEvent('start', () => true, 15000, m);
    check('changing resolution/fps reconnects and sends a new START', !!s2 && s2.width === 1280 && s2.height === 720 && s2.fps === 60, JSON.stringify(s2));

    // ---- 9. HEVC --------------------------------------------------------------------------------------------
    m = sim.mark();
    await obs.request('SetInputSettings', { inputName: 'Niriun1', inputSettings: { video_format: 1, resolution_str: '1280x720', fps: 30 }, overlay: true });
    const s3 = await sim.waitEvent('start', () => true, 15000, m);
    check('HEVC selected: START codec = 2', !!s3 && s3.codec === 2, JSON.stringify(s3));
    await sleep(3500);
    check('decoded HEVC video is not blank', (await obs.screenshot('Niriun1', path.join(OUT, 'video-hevc.png'))) > 15000);

    // ---- 10. phone disappears and comes back ----------------------------------------------------------------
    sim.kill();
    await sleep(1500);
    const sim2 = new Sim([]);
    const back = await sim2.waitEvent('start', () => true, 20000, sim2.mark());
    check('plugin reconnects by itself when the phone returns', !!back);
    sim2.kill();
  } catch (e) {
    check('test run completed without exception', false, e.message);
  } finally {
    obsProcess.kill();
    sim.kill();
    exitCode = summary();
    process.exit(exitCode);
  }
}

main();
