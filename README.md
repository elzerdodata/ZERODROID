# ZERODROID

**Zero Data · Nintendo Switch cloud gaming client**

ZERODROID is an open-source, unofficial Boosteroid client for Nintendo Switch
homebrew. Version 0.9.1 provides QR login, a controller-first game library,
native H.265/HEVC video with automatic H.264 fallback, FSR 1 upscaling, Opus
audio and complete controller input.

> [!IMPORTANT]
> ZERODROID is a beta and is not affiliated with or endorsed by Boosteroid or
> Nintendo. It requires a legitimate Boosteroid account and does not provide,
> download or bypass access to games, subscriptions, DRM or Nintendo software.

## New in v0.9.1

- AMD FidelityFX Super Resolution 1 (FSR 1) as the video upscaler: EASU edge
  adaptive upscaling when the stream is below the display resolution and the
  RCAS sharpening pass in every case, so low-bitrate sessions stay sharp and
  readable. It also runs at 1:1 in handheld mode.
- Settings -> Advanced -> Upscaler (FSR 1 / OFF).
- New FSR source resolutions: 540p (10 Mbps) and 480p (7 Mbps, handheld). Fewer
  pixels at a much lower bitrate over the Switch's WiFi, with FSR rebuilding the
  panel resolution. Docked, 480p promotes to 540p to stay inside FSR's 4x area
  limit.
- With FSR 1 enabled the encoder ceiling is 10% lower, giving the WiFi extra
  headroom; the HUD shows the negotiated cap.
- The performance overlay tags the active path (`[FSR]` / `[FSR-S]`).

## Included from v0.9.0

- Differentiation between WebRTC and Native UDP (proto: 2): custom Reed-Solomon
  FEC loss tracking, prompt keyframe gating on packet recovery failure, and
  active transport badges (`[NAT-UDP]` / `[WebRTC]`) with matching loss
  metrics (`(FEC)` / `(NACK)`) in the HUD.
- Network protocol selector in Settings -> Advanced (Auto / Native UDP / WebRTC).
- Adaptive NACK retention window in the jitter buffer (scales with measured
  RTT instead of a fixed 200 ms timeout), speeding up packet-loss recovery.

## Included from v0.8.9

- Streaming in H.265/HEVC by default (RFC 7798 over WebRTC, NVDEC/nvtegra
  zero-copy), with H.264 as automatic fallback.
- New Settings -> Advanced section (collapsible): video codec (Auto / H.265 /
  H.264) and FPS limit (Auto / 30 / 60) with disclosure arrow.
- In-game performance overlay displaying the active video codec and measured/target
  bitrate.

## Included from v0.8.8

- Added 720HQ (20 Mbps) and 1080HQ (30 Mbps) quality modes.
- Added left-stick navigation for the library and Settings.
- Localized all UI and status messages for the selected language.
- The queue now waits until the user cancels instead of timing out.
- Added 15-minute last-session reuse for the same game.
- Removed the unused Xbox Guide/Game Bar overlay and stick-click shortcut.

## Included from v0.8.7

- More resilient video recovery after packet loss and decode errors.
- Broken predictive frames are discarded until a fresh IDR instead of showing
  persistent corruption.
- Reduced green/pink artifacts and stale frames after NVDEC errors.
- 720p streaming bitrate raised to 15 Mbps.

## Included from v0.8.6

- Added Leave Session to the in-game touch settings, returning to the library.
- Added streaming resolution selection: Automatic, 720p or 1080p.
- Redesigned home Settings with larger cards, scrolling and no redundant
  captions.
- Pushes to `main` now build and publish GitHub releases automatically.

## Included from v0.8.5

- Purple touchpad-style virtual mouse during streaming.
- Quick tap sends a left click; long press sends a right click.
- The cursor starts at the centre of the screen and hides after 10 seconds
  without touch input.
- The build output is now named `ZERODROID.nro`.

## Included from v0.8.3

- Optional in-stream ping display, hidden by default for a cleaner picture.
- The ping badge and ping line in the performance HUD can now be toggled from
  the touch settings panel.

## Included from v0.8.2

- Search the library with the native Nintendo Switch keyboard.
- All, Favorites and Recently Played library tabs.
- Persistent favorites and a 30-game recent history stored locally.
- Spanish and English interface selector in Settings.

## Included from v0.8.1

- Completely new ZERODROID identity and lilac/graphite visual system.
- Original ZD application icon.
- Live gateway ping meter during streaming.
- Touch two-dot menu during streaming.
- In-stream performance HUD, Nintendo/Xbox face-button layout and image
  controls.
- Library Settings menu for distant regions, preferred server and controller
  layout.

## Download and install

Download the latest NRO from [GitHub Releases](../../releases/latest), then
copy it to:

```text
sdmc:/switch/ZERODROID/ZERODROID.nro
```

Launch Homebrew Menu through title takeover by holding **R** while opening an
installed game. Album/applet mode does not provide enough memory for streaming.

The console must already be capable of running legal homebrew. ZERODROID does
not include or install custom firmware.

## Controls

- **Minus in the library:** ZERODROID Settings.
- **Streaming resolution in Settings:** Automatic, 720p, 720HQ, 1080p, 1080HQ,
  540p or 480p. The two low modes are FSR sources: they ask the server for
  fewer pixels at a much lower bitrate (10 Mbps / 7 Mbps) and FSR 1 upscales
  them back to the panel. Applies on the next game launch.
- **Upscaler in Settings -> Advanced:** FSR 1 or OFF. FSR upscales 720p/540p/
  480p sources to the display and always applies its RCAS sharpening pass, also
  at 1:1 in handheld mode (HUD shows `[FSR]` or `[FSR-S]`). While FSR is on, the
  encoder ceiling requested from the server is 10% lower to give the WiFi
  headroom.
- **Left stick or D-pad:** navigate the library and Settings menus.
- **ZL / ZR:** switch between All, Favorites and Recent.
- **Y in the library:** search using the system keyboard.
- **X in the library:** add or remove the selected game from Favorites.
- **R3 in the library:** refresh the Boosteroid library.
- **Two dots during streaming:** open the touch settings panel, which also has
  the Mouse switch for the virtual touchpad cursor.
- **Leave Session in the stream panel:** stop streaming and return to the
  library.
- **Touchpad during streaming:** move the purple virtual mouse. A quick tap
  sends a left click and a long press sends a right click. The in-stream panel
  has a **Mouse** row: when it is OFF, touching the screen outside the panel
  does nothing.
- **Ping in the stream panel:** show or hide the latency badge; it starts off
  in every new session.
- **L + R + Minus:** stop streaming and return to the library.

Application data is stored only under `sdmc:/switch/ZERODROID/`.

## Technical overview

- C++17 application built with devkitA64 and libnx.
- SDL2 library and login UI; deko3d video presentation while streaming.
- FFmpeg/libavcodec H.265/HEVC decoding (NVDEC via the nvtegra hwaccel) and
  Opus audio playback.
- libpeer, WebRTC data channels, SRTP and SCTP for the streaming transport.
- Device-code QR authentication; the resulting session remains on the SD card.
- No analytics, advertising, referral links or telemetry.

## Build from source

The easiest way is to build with Docker. The root `Dockerfile` installs the
devkitPro Switch toolchain and portlibs, builds the WebRTC dependency bundle,
and produces the NRO:

```bash
DOCKER_BUILDKIT=1 docker build --output type=local,dest=dist .
```

The resulting file is `dist/ZERODROID.nro`.

For a native build, install the standard devkitPro Switch toolchain and Switch
portlibs. The WebRTC dependency bundle can be built reproducibly with Docker:

```bash
cd switch
bash deps/build-switch.sh
make -j4
```

The dependency script clones upstream sources, applies the Switch-specific
patches in `switch/deps/patches/`, and places generated libraries under the
ignored `switch/deps/switch/` directory. It also supports `--container`, which
builds in-place without invoking Docker; the root `Dockerfile` uses that mode.

## Beta status and feedback

Service-side API or streaming changes can temporarily break an unofficial
client. Please include the ZERODROID version, server region and relevant lines
from `sdmc:/switch/ZERODROID/zerodroid.log` in reproducible bug reports. Remove
any personal information before posting logs publicly.

Licensed under Apache-2.0. Third-party components remain under their respective
licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
