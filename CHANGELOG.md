# ZERODROID changelog

## v0.9.1 - FSR 1 upscaler for low-bandwidth streaming

- AMD FidelityFX Super Resolution 1 (FSR 1) video upscaler:
  - EASU edge-adaptive upscaling (luma plane, 12 taps clamped to the visible
    NVDEC area) plus the RCAS sharpening pass, running entirely on the GPU with
    zero-copy NVDEC input.
  - Settings -> Advanced -> Upscaler (FSR 1 / OFF, persisted as `upscaler`).
    FSR also runs at 1:1 in handheld mode, where RCAS is what keeps a soft
    low-bitrate stream readable.
  - New FSR source resolutions: 540p (10 Mbps) and 480p (7 Mbps, handheld).
    They request fewer pixels at a much lower bitrate over the Switch's WiFi and
    let FSR rebuild the panel resolution. Docked, 480p promotes to 540p so the
    upscale stays inside FSR's 4x area limit.
  - Performance HUD tags the active path (`[FSR]` / `[FSR-S]`).
  - With FSR 1 enabled, the encoder ceiling requested from the gateway (and
    shown in the HUD) is 10% lower, leaving more headroom for the WiFi.
- In-stream menu: new Mouse row (ON/OFF, persisted as `mouse`). When OFF the
  touchpad is ignored outside the menu, so accidental touches never move or
  click the virtual mouse. The panel grew to fit the extra row.

## v0.9.0 - WebRTC vs Native UDP and adaptive jitter recovery

- Adaptive NACK retention window in the jitter buffer: scales with measured RTT
  (1.5x RTT + 15 ms, clamped to 40-200 ms) instead of a fixed 200 ms timeout,
  accelerating packet-loss recovery on low-latency connections.
- Differentiation between WebRTC and Native UDP (proto: 2):
  - Added network protocol selector in Settings -> Advanced (Auto / Native UDP / WebRTC).
  - In-stream performance overlay displays active transport badges (`[NAT-UDP]` / `[WebRTC]`)
    and matching loss metrics (`(FEC)` / `(NACK)`).
  - Native UDP now tracks real packet loss and Reed-Solomon shard recovery.
  - Proactive NVDEC keyframe gating and WebSocket keyframe request if Reed-Solomon FEC recovery fails.

## v0.8.9 - H.265 streaming and advanced settings

- Switched streaming to H.265/HEVC by default (RFC 7798 over WebRTC, NVDEC/nvtegra
  zero-copy), with H.264 as automatic fallback.
- Added an RFC 7798 depacketizer (single NAL, AP and FU) and HEVC IRAP
  keyframe gating (NAL types 16-21) to the video jitter buffer.
- Settings -> Advanced (collapsible): video codec (Auto / H.265 / H.264) and
  FPS limit (Auto / 30 / 60). Auto prefers H.265 and retries the session once
  with H.264 when no frame arrives; Auto FPS keeps the previous 60 fps default.
- The in-stream performance overlay now shows the negotiated codec and the
  measured/target bitrate.

## v0.8.8 - Quality modes and session reuse

- Added 720HQ (20 Mbps) and 1080HQ (30 Mbps) streaming modes.
- Added left-stick navigation for the library and Settings.
- Localized all UI and status messages for the selected language.
- Removed the client-side queue timeout; waiting continues until cancellation.
- Added 15-minute reuse of the previous streaming session for the same game.
- Removed the Xbox Guide/Game Bar overlay and the both-sticks shortcut.

## v0.8.7 - Video recovery

- Reworked video recovery after RTP loss and decode errors.
- Predictive frames are dropped until a clean IDR, preventing persistent
  corruption.
- Avoided flushing the NVTEGRA decoder, removing green/pink artifacts and stale
  menu frames after recovery.
- Added a bounded wait for an IDR so video cannot freeze indefinitely.
- Raised the 720p bitrate ceiling to 15 Mbps.

## v0.8.6 - Streaming settings and release automation

- Added Leave Session to the in-game touch settings.
- Added streaming resolution selection: Automatic, 720p or 1080p, with matching
  bitrate requests to Boosteroid.
- Redesigned home Settings with larger cards, automatic scrolling and no
  redundant captions.
- Added GitHub Actions to build and publish a release on every push to main.

## v0.8.5 - Virtual mouse

- Added a purple touchpad-style virtual mouse during streaming.
- A quick tap sends a left click; a long press sends a right click.
- The cursor starts at the centre and hides after 10 seconds without touch.
- Renamed the build output to ZERODROID.nro.

## v0.8.4 - Xbox Game Bar shortcut

- Fixed the touch Xbox Guide/Home button and the stick-click combo to open the
  Xbox Game Bar using Boosteroid's Ctrl+L shortcut.

## v0.8.3 - Optional ping display

- Added an in-stream PING ON/OFF option to the touch settings panel.
- The ping badge is hidden by default at the start of every streaming session.
- Disabling ping also removes the ping line from the performance HUD.

## v0.8.2 - Library organization

- Added native game search through the Nintendo Switch software keyboard.
- Added All, Favorites and Recently Played tabs.
- Added persistent favorites and a 30-game recent history.
- Added a persistent Spanish/English interface selector.
- Moved sign out into Settings so X can toggle favorites.

## v0.8.1 - Local test beta

- New ZERODROID name, visual identity, data directory and application icon.
- Added WebSocket round-trip ping measurement and an always-visible ping badge.
- Activated the touch Xbox Guide/Home button.
- Activated the touch two-dot stream settings panel.
- Added Xbox Guide through simultaneous left/right stick clicks.
- Added live Nintendo/Xbox A/B/X/Y switching from Settings and the touch panel.
- Preserved preferred server and distant-region configuration.
