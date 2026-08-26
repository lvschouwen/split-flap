# Web UI — Modern Hybrid (2026-08-24)

Operator-driven redesign of the Master web UI. Replaces the dense departure-board
overhaul skin with a modern responsive hybrid: strong Home, clear tabs, cluster
as a first-class Wall surface.

## Goals

- **Modern + responsive** — fluid layout, readable on phone and desktop, sticky board header.
- **Hybrid density (C)** — Home is the daily driver (composer + display controls);
  Wall / Maintain / Settings / System / Logs hold the rest without feeling like a
  control-room dump.
- **Cluster from day one** — 2+ row walls (leader + followers) get a dedicated Wall tab,
  multi-row mirror, and per-row message inputs while leading.
- **PROGMEM assets** — still baked by `build_assets.py`. No artificial ~25 KB gzip
  ceiling; app slots are 4 MB. LittleFS `storage` remains an option for later bulk assets.
- **OTA safe** — master binary must stay well under the 4 MB A/B slot. USB only for
  partition/rescue edge cases.
- **API evolution allowed** — keep existing routes working where practical; change or
  version endpoints when it clearly improves operator UX or code quality. Document
  breaks in the PR / release notes.

## Information architecture

| Tab | Role |
|---|---|
| **Home** | Composer (single or per-row when leading), mode / alignment / speed, stop |
| **Wall** | Cluster config, member table, scan/add, follower firmware store, leave/promote |
| **Maintain** | Calibration, unit health, provisioning, width override, device role, master OTA |
| **Settings** | Device name + timezone, MQTT, WiFi forget |
| **System** | Vitals + sparklines, links counters, firmware/rescue identity, cluster vitals |
| **Logs** | Live RAM log, flash log |

Persistent chrome: device name, live/cluster status, flap mirror (single or wall),
health strip(s), banners.

First-run wizard keeps the three-step flow; new visual language only.

## Visual direction

- Charcoal ground, elevated surfaces, amber accent (departure-board DNA without the
  cramped mono-everything look).
- Flap tiles keep the two-leaf fold + riffle animation; health segments stay under tiles.
- System UI uses clearer hierarchy: larger titles, more breathing room, cards with
  consistent padding, sticky tab bar on small screens where useful.
- `prefers-reduced-motion` still disables folds/pulses.

## Data flow (phase 1)

Unchanged wire contracts:

- `GET /settings` (5 s poll), `GET /events` (SSE display/wall), `GET /units/health`
- `POST /` provided-field settings, transient text/dwell
- Cluster: `/cluster/config|status|discover|leave|promote|digest|follower-firmware|…`
- Maintenance / firmware / logs as today

Later phases may add a compact `GET /status` consumer on the Home path, or a
versioned `/api/v2/…` surface if the JSON shapes need a clean break — only if the
gain is real.

## Constraints that remain

- Vanilla JS, no CDN, no webfonts.
- Wire-derived strings → text nodes only (never `innerHTML` for host/name/rev).
- `CALIBRATION_LETTERS` stays byte-synced with `SFP_ALPHABET` (build gate).
- CSRF on mutating routes; upload MD5 gates unchanged.

## Phases

1. **Foundation (this branch start)** — shell, tokens, Wall tab, sticky board, CSS polish;
   existing `script.js` still drives behavior.
2. **Home + Wall UX** — clearer composer, wall status at a glance, less banner noise.
3. **Maintain / Settings / System / Logs** — progressive disclosure, denser tables that
   still scan on mobile.
4. **JS modularization** — split by concern without changing behavior; optional API
   cleanups behind flags or dual endpoints.
5. **Bench** — OTA to leader first, verify 2-row wall, calibration, master OTA, follower push.

## Non-goals (for now)

- Moving web assets onto LittleFS.
- Auth / HTTPS on the LAN UI.
- Rewriting the TUI/CLI in the same PR (they stay on the HTTP surface).
