# Niriun OBS plugin

An OBS Studio source that receives the camera and microphone of a phone running [Niriun](https://github.com/leoberbert/Niriun),
and tells the phone when the source is **on air** or in **preview** (tally).

It started from [droidcam-obs-plugin](https://github.com/dev47apps/droidcam-obs-plugin) (GPL-2.0-or-later) by Dev47Apps, version 2.5.1
(commit `ddfa888`). This repository is independent, not a GitHub fork, and does not follow the upstream; its history still contains the original
commits. The DroidCam protocol was replaced by Niriun's. The plugin is the PC side of the Niriun protocol, so **nothing else has to be installed
on the computer**: no Iriun Webcam client, no virtual camera driver, no OBS WebSocket.

```
phone (Niriun app)  ──── Wi-Fi or USB ────►  OBS + niriun-obs plugin
   camera + mic         H.264/HEVC + Opus      decodes, shows the source
   tally border   ◄──── tally (ON AIR) ─────  knows if the source is on program
```

Portuguese (main documentation): [README.md](README.md).

## Use

1. Install the plugin (below) and restart OBS Studio.
2. On the phone: open Niriun and press **Start**.
3. In OBS: **Sources → + → Niriun**. Pick the phone in **Device** (it is found automatically on the same Wi-Fi; for a cable
   choose the `[USB]` entry, or use *WiFi IP* and type the address shown in the app's Details) and press **Activate**.
4. The phone's border turns **magenta** while the source is on the program output and **amber** when it is in the preview
   (Studio Mode), and the app asks for a double confirmation before stopping while you are live.

Properties: resolution, frame rate, H.264/HEVC, audio on/off, color matrix, hardware decoding, low-latency (unbuffered)
output, "disconnect when not showing", and remote camera controls (camera, zoom, torch, mirror, lock exposure/focus).
Every change that affects the stream reconnects automatically; if the phone goes away the plugin keeps trying.

### How the phone is found

| Mechanism | What it needs |
|---|---|
| UDP multicast beacon `239.255.188.58:4698` (the phone sends it every 2 s while no computer is connected) | OBS may listen on UDP; Windows asks for firewall permission the first time |
| **Refresh** probes the local /24 on TCP 4697 (the phone answers with its hello) | outbound TCP only: works where multicast is blocked |
| USB: `adb forward` | `adb` in PATH or in the Android SDK (`ANDROID_HOME`, `%LOCALAPPDATA%\Android\Sdk`); optional |
| *WiFi IP* | the address of the phone, port 4699 |

## Install (Windows, OBS Studio 32.x)

Download `niriun-obs-windows-x64` from the CI artifacts (or build it, below) and either

* unzip it into the OBS Studio folder (`C:\Program Files\obs-studio`, administrator), or
* run `scripts\install-windows.ps1` for the current user only (`%APPDATA%\obs-studio\plugins\niriun-obs`).

The plugin links against the FFmpeg that OBS ships; it refuses to load if the major version of `avcodec` differs from the
one it was built with (build against the OBS release you run).

## Build

Windows (Visual Studio 2022, CMake 3.20+, git):

```powershell
scripts\fetch-windows-deps.ps1                       # headers + import libraries matching the installed OBS (-ObsDir to change)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DNIRIUN_DEPS_DIR="$PWD\deps"
cmake --build build --config RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo             # protocol unit tests
scripts\install-windows.ps1
```

Linux / macOS: `cmake -S . -B build` with `libobs`, `obs-frontend-api` and FFmpeg found through CMake config files or
pkg-config, then `cmake --build build`. On Linux the sources pass a `g++ -fsyntax-only` check and the unit tests run; the plugin itself
has only been run on Windows, and macOS has not been compiled at all.

## Test

* `ctest`: protocol framing, parsing and builders (no OBS needed).
* `tools/phone-sim/phone-sim.js`: a stand-in for the phone (real H.264/HEVC and Opus from ffmpeg, beacon, probe port,
  controls, rotation). `node tools/phone-sim/phone-sim.js` and add a source with *WiFi IP* `127.0.0.1`.
* `tools/e2e/e2e.js`: starts a portable OBS with the plugin plus the simulator and checks discovery, video, audio, ACKs,
  tally (program / off / preview / program), camera controls, rotation, settings changes, HEVC and reconnection through
  obs-websocket. `tools/e2e/app-e2e.js` does the video/tally part against the real Android app over `adb`.

## Protocol

The wire protocol is documented in the Niriun repository (`docs/PROTOCOL.md`). What this plugin does:

| Direction | Message | When |
|---|---|---|
| phone → PC | `0x01` hello, `0x0A` sizes, `0x18` cameras | right after connecting |
| PC → phone | `0x0B` START (audio, codec 1/2, width, height, fps) | after the sizes arrive |
| phone → PC | `0x08` video (f32 seconds + Annex-B), `0x07` audio (raw Opus 48 kHz mono) | continuously |
| PC → phone | `0x09` ACK of every video packet (the same f32, decode time in ms) | after each decode |
| PC → phone | `0x10` camera controls | from the properties dialog |
| PC → phone | **`0x30` tally** (u32 state 0 off / 1 preview / 2 program + 256-byte source name) | after START and on every change |
| PC → phone | `0x01` no-op | every 3 s when idle (the phone drops silent clients after 10 s) |

`0x30` is a Niriun extension; an Iriun phone app ignores it. Tally is derived from OBS's own source state: *program* = the
source is active (`activate`/`deactivate`), *preview* = it is visible but not active (`show`/`hide`, i.e. the preview in Studio
Mode). Opening a Multiview or a projector of a scene also counts as visible.

## What changed from droidcam-obs-plugin

* New: the Niriun client (`niriun_protocol.h`, `source.cc`), Opus decoding, in-band tally, camera controls, beacon and subnet
  discovery (`discovery.cc`), a CMake build (the original only shipped Makefiles), the simulator and the end-to-end tests.
* Reworked: the FFmpeg wrapper (decoder flags are now set *before* `avcodec_open2`, so `LOW_DELAY` takes effect; picture
  format and color are re-evaluated when the stream changes), the networking helpers (cancellable connects, no leaks),
  `adb` handling (finds the Android SDK, quotes paths with spaces, uses `forward tcp:0`).
* Removed: the DroidCam HTTP protocol, MJPEG, iOS/usbmuxd, the mDNS browser and the Qt "add device" dialog.

## License

GPL-2.0-or-later, as the original. See [LICENSE](LICENSE). Parts are © DEV47APPS (droidcam-obs-plugin) and © Hugh Bailey
(OBS Studio's FFmpeg decode code); new code is © Leonardo Berbert. The Niriun app is a separate program (Apache-2.0) that
talks to this plugin over the network.
