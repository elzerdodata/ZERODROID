# AGENTS.md

## What this repo is

- Unofficial Boosteroid cloud-gaming client for Nintendo Switch homebrew.
  Buildable code lives under `switch/`; the repo root is docs and release assets.
- C++17, devkitA64/libnx, SDL2, FFmpeg/Opus, libpeer WebRTC, and deko3d.
- There is no test suite, package manager, or host build target. CI only builds
  and publishes releases on `main`; verification is a devkitA64 build plus an
  on-console run. `src/core` has some non-Switch fallbacks, but no host/test
  target wires them up.
- The build uses `-fno-rtti`; avoid `dynamic_cast` and `typeid`.

## Build

The supported host-independent build is the root `Dockerfile`. It installs
devkitPro/portlibs, builds the patched WebRTC dependencies (libpeer, libsrtp2,
usrsctp, mbedtls), and compiles the NRO:

```bash
DOCKER_BUILDKIT=1 docker build --output type=local,dest=dist .
# dist/ZERODROID.nro
```

Native builds still work: run `cd switch && bash deps/build-switch.sh` to build
the dependencies (that script uses the `devkitpro/devkita64` image), then run
`make` where `DEVKITPRO=/opt/devkitpro`. The Dockerfile invokes the dependency
script with `--container`, which builds in-place without nested Docker.

- `deps/build-switch.sh` pins libpeer to commit `9319aa4...`; the Switch patch
  only applies to that revision. Regenerate the patches before bumping the pin.
- Output is `switch/ZERODROID.nro`; `APP_VERSION` and `TARGET` are set in
  `switch/Makefile`. Keep the README/CHANGELOG in sync on releases.
- `.github/workflows/release.yml` builds and publishes a GitHub release on every
  push to `main`. Bump `APP_VERSION` for a new tag; with the same version it
  refreshes the existing release asset.
- `make all` also compiles `switch/shaders/*.glsl` with `uam` into ignored
  `switch/romfs/shaders/*.dksh`. Use `make shaders` for shader-only changes.
- The Makefile only adds `src/switch/stream` when `deps/switch/lib/libpeer.a`
  exists, but `main.cpp` unconditionally uses `stream::Engine`; a build without
  the deps fails at link time. Treat the deps as required.
- `SOURCES` lists `src/core` and `src/switch`; new source directories need a
  Makefile change. Do not commit `switch/deps/switch`, `switch/build-switch/`,
  generated `.dksh`, NRO/ELF artifacts, or `session.json`.

## Architecture

- `switch/src/core/` — Boosteroid API (`boosteroid_api.*`), blocking libcurl
  wrapper (`http.*`), QR encoder, and runtime language switch (`i18n.hpp`).
  `BoosteroidAPI` loads the saved auth session and last streaming session in its
  constructor.
- `switch/src/switch/main.cpp` — the entire SDL2 login/library/settings UI,
  input loop, and async request handling. Use `ui("es", "en")` for main UI text;
  shared engine/API code must use `gnx::i18n::tr("es", "en")`. Never hardcode one
  language. The selected language is applied at startup and updated immediately
  from Settings.
- Home/settings navigation accepts both D-pad and left stick through `navDown`
  and `AnalogNavigation` (deadzone + repeat). Streaming still sends the raw pad
  state to `Engine::send_gamepad`; do not route gameplay through `navDown`.
- `switch/src/switch/stream/` — streaming engine, FFmpeg/Opus pipeline, libpeer
  transport, and deko3d renderer. `Engine` owns worker/decode/native-UDP threads;
  only the main thread may create or destroy SDL textures.
- Video is H.265/HEVC end to end: the datachannel `getstatus` reply requests
  `codec: h265`, the libpeer offer advertises `H265/90000` (PT 104, RFC 7798),
  `VideoJitterBuffer` depacketizes single NAL/AP(48)/FU(49), and
  `VideoDecoder` opens `AV_CODEC_ID_HEVC` on nvtegra. Keyframe gating accepts
  HEVC IRAP NAL types 16-21 (never bare VPS/SPS/PPS). Changing the codec means
  updating all four places at once.
- Settings -> Advanced selects the codec (`CodecPreference`: Auto / H.265 /
  H.264, persisted as `codec` in `settings.json`), the FPS limit (`fps`,
  sent as `framerate_max` in the `getstatus` reply), the network protocol
  (`TransportPreference`: Auto / Native UDP / WebRTC, sent as `proto` 2 or 1
  in the `getstatus` reply) and the upscaler (`UpscalerPreference`: Off / FSR 1,
  persisted as `upscaler`).
- Auto codec falls back to H.264 when `video_.init()` fails, and the worker
  retries the session once with H.264 when an H.265 session produces no frame.
  `Engine::worker()` runs `run_session()` in a loop for that; the decoder reopen
  is handed to the decode thread through `pendingCodec_` because only that
  thread owns `VideoDecoder` while streaming.
- The upscaler runs AMD FidelityFX FSR 1 in `DkVideoRenderer::render()`:
  `video_fsr_fsh.glsl` (EASU on the luma plane, 12 texel fetches clamped to the
  visible NVDEC area) renders into an RGBA8 intermediate target, then
  `fsr_rcas_fsh.glsl` sharpens into the framebuffer. EASU only runs when the
  frame is smaller than the output (1x-4x area); at 1:1 and when downscaling,
  pass 1 uses `video_fsh.glsl` and only RCAS runs. Keep the built-in unsharp
  mask disabled while FSR is on (`sharp_data.x = 0`). The intermediate target is
  rebuilt with the swapchain, and the HUD tags the active path `[FSR]` or
  `[FSR-S]`.
- Streaming resolution is requested in the gateway `x`/`y` parameters and
  `bitrate_max`: 720p = 15 Mbps, 720HQ = 20 Mbps, 1080p = 25 Mbps,
  1080HQ = 30 Mbps, 540p = 10 Mbps and 480p = 7 Mbps; Auto follows dock mode.
  540p/480p are FSR sources for low-bandwidth WiFi: fewer pixels at a much
  lower bitrate, upscaled back to the panel. Docked, 480p promotes to 540p so
  the upscale stays inside FSR's 4x area limit. With FSR 1 enabled every
  ceiling is trimmed by 10% (`stream_bitrate`), trading a little detail for
  more WiFi headroom; it is applied in the `getstatus` reply and in the HUD cap.
- The in-stream touch panel (two-dot handle) is rasterized on the CPU into a
  672x870 RGBA texture at the geometry in `quick_menu.hpp`; keep
  `kQuickTextureRect`, `kQuickPanelRect`, `kQuickTexH` and the `kQuickQuad`
  clip-space corners in sync when adding rows. Its `Mouse` row gates the
  virtual mouse: `main.cpp` only calls `virtual_mouse_move/click` while
  `quickMenuState.mouse` is set, and the flag persists as `mouse` in
  `settings.json`.
- The performance overlay (`QuickPerformance`) shows resolution, codec,
  active transport tag (`[NAT-UDP]` / `[WebRTC]`), measured/target Mbps and
  matching loss metrics (`(FEC)` / `(NACK)`); `Engine::update_net_stats()`
  feeds it once per second. Native UDP tracks packet loss and Reed-Solomon
  recovery shards, immediately gating the decoder on IRAP if FEC recovery fails.
- Streaming resolution is requested in the gateway `x`/`y` parameters and
  `bitrate_max`: 720p = 15 Mbps, 720HQ = 20 Mbps, 1080p = 25 Mbps,
  1080HQ = 30 Mbps; Auto follows dock mode.
- During playback, SDL output is suspended and deko3d takes over. Leaving the
  stream calls `end_deko_output()` and `Gfx::resume()`, which rebuilds the SDL
  window/renderer and clears the text cache. Shaders load from `romfs:/shaders/`.
- `switch/deps/shim/include` plus `src/switch/stream/switch_compat.c` supply the
  missing newlib/libnx pieces (ifaddrs, mbedtls entropy, libpeer log sink).
  Dependency sources are patched from `switch/deps/patches/`; do not edit the
  generated `switch/deps/src/` tree.

## Runtime and debugging

- App data is under `sdmc:/switch/ZERODROID/` (uppercase; the lowercase path in
  a `boosteroid_api.hpp` comment is stale): `session.json`, `last_session.json`,
  `settings.json`, `zerodroid.log`, `stream.log`, rotated `stream-prev.log`, and
  `covers/`. Never commit `session.json` or `last_session.json`.
- `zerodroid.log` is recreated each launch; `stream.log` rotates on each stream
  start. Ask for these logs when debugging playback or login issues.
- The queue has no client-side timeout: it waits until the user cancels
  (`quit_`/cancel flag). Do not reintroduce a fixed queue deadline.
- `last_session.json` stores `sessionId`, `appId`, and `usedAt`. Starting the same
  game within 15 minutes first calls
  `/api/v1/streaming/session/details?session=...` to resume; any error falls back
  to a fresh enqueue. `stopStreamingSession` refreshes `usedAt`, and logout
  removes the file.
- Video recovery: consume decoder errors even when a frame was rendered, drop
  predictive frames until a fresh IDR, and request keyframes with a 500 ms
  debounce. The jitter also probes for an IDR every 4 s. Never call
  `avcodec_flush_buffers()` on the NVTEGRA backend; deko3d may still own those
  surfaces (green/pink frames). Native UDP bypasses `VideoJitterBuffer` and has
  no WebRTC PLI path, so do not assume PLI-based recovery works there.
- Keep libpeer at `LOG_LEVEL=2` in `deps/build-switch.sh`; DEBUG-level
  per-packet logging floods the SD card and stalls video.
- Boosteroid endpoints are reverse-engineered and can change without notice.
  `/api/v1/streaming/session/details?session=...` returns a `gw`/`queryString`
  pair signed for that specific VM; never mix it with a generic gateway or
  signature. TLS uses `romfs:/cacert.pem` because the Switch has no CA store.
