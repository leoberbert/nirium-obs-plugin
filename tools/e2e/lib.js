// Helpers shared by e2e.js (phone simulator) and app-e2e.js (real Niriun app): drive a portable OBS Studio
// through obs-websocket. Needs Node 22+ (global WebSocket).

const { spawn, execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const WS_PORT = 4466;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function parseArgs(argv) {
  return Object.fromEntries(
    argv.reduce((acc, a, i, all) => {
      if (a.startsWith('--')) acc.push([a.slice(2), all[i + 1] && !all[i + 1].startsWith('--') ? all[i + 1] : true]);
      return acc;
    }, []),
  );
}

async function waitFor(fn, ms, step = 100) {
  const end = Date.now() + ms;
  for (;;) {
    const v = await fn();
    if (v) return v;
    if (Date.now() > end) return null;
    await sleep(step);
  }
}

/** Collects PASS/FAIL lines. */
function makeChecker() {
  const results = [];
  return {
    results,
    check(name, ok, detail = '') {
      results.push({ name, ok });
      console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}${detail ? '  (' + detail + ')' : ''}`);
      return ok;
    },
    summary() {
      const failed = results.filter((r) => !r.ok).length;
      console.log(`\n${results.length - failed}/${results.length} checks passed`);
      return failed === 0 ? 0 : 1;
    },
  };
}

/**
 * Starts the portable OBS in `obsDir` with an empty scene collection. Its next start would stop at the
 * "unclean shutdown / safe mode" question because we kill it at the end, so the sentinel is removed first.
 */
function launchObs(obsDir, outDir) {
  fs.mkdirSync(outDir, { recursive: true });
  const config = path.join(obsDir, 'config', 'obs-studio');
  const scenesDir = path.join(config, 'basic', 'scenes');
  if (fs.existsSync(scenesDir)) {
    for (const f of fs.readdirSync(scenesDir)) if (/\.json(\.bak)?$/.test(f)) fs.rmSync(path.join(scenesDir, f));
  }
  fs.rmSync(path.join(config, '.sentinel'), { recursive: true, force: true });

  const log = fs.createWriteStream(path.join(outDir, 'obs-stdout.txt'));
  const proc = spawn(path.join(obsDir, 'bin', '64bit', 'obs64.exe'), ['--portable', '--disable-updater', '--disable-shutdown-check'], {
    cwd: path.join(obsDir, 'bin', '64bit'),
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  proc.stdout.pipe(log);
  proc.stderr.pipe(log);
  return {
    proc,
    kill() {
      try {
        execFileSync('taskkill', ['/PID', String(proc.pid), '/T', '/F'], { stdio: 'ignore' });
      } catch {}
    },
  };
}

class Obs {
  constructor(obsDir) {
    this.obsDir = obsDir;
    this.pending = new Map();
    this.n = 0;
    this.meters = {};
  }

  /**
   * obs-websocket answers while OBS is still loading and then hangs or refuses requests. The plugin logs
   * "using bind IP" on OBS_FRONTEND_EVENT_FINISHED_LOADING, which is the reliable "ready" signal.
   */
  async waitFinishedLoading(ms) {
    const logs = path.join(this.obsDir, 'config', 'obs-studio', 'logs');
    return waitFor(() => {
      const files = fs.existsSync(logs) ? fs.readdirSync(logs).filter((f) => f.endsWith('.txt')).sort() : [];
      if (files.length === 0) return false;
      return fs.readFileSync(path.join(logs, files[files.length - 1]), 'utf8').includes('[niriun-obs] using bind IP');
    }, ms, 250);
  }

  async connect(ms) {
    const end = Date.now() + ms;
    while (Date.now() < end) {
      try {
        await new Promise((resolve, reject) => {
          const ws = new WebSocket(`ws://127.0.0.1:${WS_PORT}`);
          ws.onerror = () => reject(new Error('ws'));
          ws.onmessage = (m) => {
            const msg = JSON.parse(m.data);
            if (msg.op === 0) {
              // 0x7FF = every event category except the high-volume ones, plus InputVolumeMeters (1 << 16).
              ws.send(JSON.stringify({ op: 1, d: { rpcVersion: 1, eventSubscriptions: 0x7ff | (1 << 16) } }));
            } else if (msg.op === 2) {
              this.ws = ws;
              ws.onmessage = (mm) => this.onMessage(JSON.parse(mm.data));
              resolve();
            }
          };
        });
        if (!(await this.waitFinishedLoading(60000))) return false;
        await waitFor(() => this.request('GetSceneList').then(() => true, () => false), 30000, 500);
        return true;
      } catch {
        await sleep(500);
      }
    }
    return false;
  }

  onMessage(msg) {
    if (msg.op === 7) {
      const p = this.pending.get(msg.d.requestId);
      if (!p) return;
      this.pending.delete(msg.d.requestId);
      if (msg.d.requestStatus.result) p.resolve(msg.d.responseData || {});
      else p.reject(new Error(`${msg.d.requestType}: ${msg.d.requestStatus.comment}`));
    } else if (msg.op === 5 && msg.d.eventType === 'InputVolumeMeters') {
      for (const i of msg.d.eventData.inputs) {
        const peak = Math.max(0, ...i.inputLevelsMul.map((c) => c[1]));
        this.meters[i.inputName] = Math.max(this.meters[i.inputName] || 0, peak);
      }
    }
  }

  request(type, data = {}) {
    const id = String(++this.n);
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.ws.send(JSON.stringify({ op: 6, d: { requestType: type, requestId: id, requestData: data } }));
      setTimeout(() => {
        if (this.pending.delete(id)) reject(new Error(`${type}: timeout`));
      }, 20000);
    });
  }

  /** Removes every input and the given scenes (the scene collection survives between runs). */
  async reset(sceneNames) {
    for (const { inputName } of (await this.request('GetInputList')).inputs) await this.request('RemoveInput', { inputName });
    for (const { sceneName } of (await this.request('GetSceneList')).scenes) {
      if (sceneNames.includes(sceneName)) await this.request('RemoveScene', { sceneName });
    }
  }

  /** Saves a PNG of the source and returns the size of its base64 text (a blank frame is tiny). */
  async screenshot(name, file, w = 640, h = 360) {
    const r = await this.request('GetSourceScreenshot', { sourceName: name, imageFormat: 'png', imageWidth: w, imageHeight: h });
    const b64 = r.imageData.split(',')[1];
    fs.writeFileSync(file, Buffer.from(b64, 'base64'));
    return b64.length;
  }
}

module.exports = { Obs, WS_PORT, launchObs, makeChecker, parseArgs, sleep, waitFor };
