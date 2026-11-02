# ZERODROID changelog

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
