# AGENTS.md

## What this repo is

- Unofficial Boosteroid cloud-gaming client for Nintendo Switch homebrew.
  Buildable code lives under `switch/`; the repo root is docs and release assets.
- C++17, devkitA64/libnx, SDL2, FFmpeg/Opus, libpeer WebRTC, and deko3d.
- There is no test suite, CI, package manager, or host build target. Verification
  is a devkitA64 build plus an on-console run. `src/core` has some non-Switch
  fallbacks, but they are not wired into a host/test build.
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
  wrapper (`http.*`), and the QR encoder. `BoosteroidAPI` loads the saved
  session in its constructor.
- `switch/src/switch/main.cpp` — the entire SDL2 login/library/settings UI,
  input loop, and async request handling. UI strings are Spanish/English pairs
  through `ui("es", "en")`; Spanish is the default.
- `switch/src/switch/stream/` — streaming engine, FFmpeg/Opus pipeline, libpeer
  transport, and deko3d renderer. `Engine` owns worker/decode/native-UDP threads;
  only the main thread may create or destroy SDL textures.
- During playback, SDL output is suspended and deko3d takes over. Leaving the
  stream calls `end_deko_output()` and `Gfx::resume()`, which rebuilds the SDL
  window/renderer and clears the text cache. Shaders load from `romfs:/shaders/`.
- `switch/deps/shim/include` plus `src/switch/stream/switch_compat.c` supply the
  missing newlib/libnx pieces (ifaddrs, mbedtls entropy, libpeer log sink).
  Dependency sources are patched from `switch/deps/patches/`; do not edit the
  generated `switch/deps/src/` tree.

## Runtime and debugging

- App data is under `sdmc:/switch/ZERODROID/` (uppercase; the lowercase path in
  a `boosteroid_api.hpp` comment is stale): `session.json`, `settings.json`,
  `zerodroid.log`, `stream.log`, rotated `stream-prev.log`, and `covers/`.
- `zerodroid.log` is recreated each launch; `stream.log` rotates on each stream
  start. Ask for these logs when debugging playback or login issues.
- Keep libpeer at `LOG_LEVEL=2` in `deps/build-switch.sh`; DEBUG-level
  per-packet logging floods the SD card and stalls video.
- Boosteroid endpoints are reverse-engineered and can change without notice.
  `/api/v1/streaming/session/details?session=...` returns a `gw`/`queryString`
  pair signed for that specific VM; never mix it with a generic gateway or
  signature. TLS uses `romfs:/cacert.pem` because the Switch has no CA store.
