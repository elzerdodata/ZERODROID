# ZERODROID changelog

## v0.8.9 - H.265 and advanced streaming settings

- Switched streaming to H.265/HEVC: the gateway handshake requests `h265`, the
  WebRTC offer advertises `H265/90000` (payload type 104, RFC 7798) and the
  NVDEC/nvtegra path decodes HEVC zero-copy into the existing deko3d renderer.
- Added an RFC 7798 depacketizer (single NAL, AP and FU) and HEVC IRAP
  keyframe gating (NAL types 16-21) to the video jitter buffer.
- Settings -> Advanced (collapsible): video codec (Auto / H.265 / H.264) and
  FPS limit (Auto / 30 / 60). Auto prefers H.265 and retries the session once
  with H.264 when no frame arrives; Auto FPS keeps the previous 60 fps default.
- The in-stream performance overlay now shows the negotiated codec and the
  measured/target bitrate.
- Adaptive NACK retention window in the jitter buffer: scales with measured RTT
  (1.5x RTT + 15 ms, clamped to 40-200 ms) instead of a fixed 200 ms timeout,
  accelerating packet-loss recovery on low-latency connections.

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
