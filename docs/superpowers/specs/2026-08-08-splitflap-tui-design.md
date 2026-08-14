# `splitflap` — interactive operator TUI (design)

**Status:** IMPLEMENTED — the v1 cut line below is what `cli/` ships.
**Date:** 2026-08-08
**Scope:** one host-side Python program. No firmware changes.

## What and why

A single interactive terminal program (`splitflap`) that replaces the Web UI for
operating and health-checking the wall: launch it, watch the wall live, drive
display/settings/maintenance from a command bar. It wraps the *existing* firmware
HTTP surface — the shelved v2 operator API rewrite stays shelved; the client
library in this tool is the adapter layer that rewrite would have been.

A second, separate project (external-content feeder: RSS / transit departures →
wall text) will import the same client library. Nothing feeder-specific is built
now; the library boundary is the only concession to it.

## Shape

New `cli/` directory in this repo. Python 3.11+, installed with
`pipx install ./cli` (or `uv tool install`). Two packages:

- **`splitflap_client`** — pure library. HTTP transport, typed models
  (dataclasses), platform capability table, SSE consumer, op contract. No
  printing, no Textual imports.
- **`splitflap_tui`** — the Textual application. All rendering, keybindings,
  command parsing. No direct HTTP.

Runtime deps: `textual`, `httpx` (plain requests + SSE streaming), stdlib
`tomllib`. No pydantic; models are dataclasses with explicit `from_json`
parsers that tolerate missing/extra fields (firmware evolves additively).

## Client library

**Transport.** Short timeouts (2 s connect / 5 s read; SSE unlimited read),
clear typed errors (`Unreachable`, `HttpError` carrying the verbatim firmware
body, `ParseError`). No automatic retry of mutating requests; firmware 409/503
bodies are surfaced verbatim, never retried.

**Platform capability model.** Two platforms exist: `esp32` (S3 master) and
`esp01` (follower row), identified by the `plat` settings key (same detection
ota-flash.sh uses). The client carries a **static capability table** keyed by
`plat` — which routes exist, which fields are absent (esp01: no `pv`/`pmm`, no
SSE, no digest/promote/config/discover, plain `/log` ring instead of
`/log/flash`). There is **no runtime `/api` negotiation**: surfaces are fixed at
build time, so discovery machinery would model variability that doesn't exist.
Instead, `GET /api` (both platforms serve it, esp01 includes `notServed`) pins
the table at **test time** — a pytest compares the capability table against
committed `/api` fixtures, the same drift-gate pattern as
`tests/test_api_index.py`.

**Models.** Parsed views of: `/status` (the S3 one-shot aggregate:
settings + stats.now + units + cluster + ota), `/settings`, `/units/health`,
`/cluster/status`, `/cluster/digest`, `/system/stats`, `/log/flash` (S3) and
`/log?after=` (esp01). Cluster state is first-class in the model: per-member
`joined/suspect/degraded`, `rescue`, rollout state incl. `updateBlocked` and
`src`, follower-image store state, HMAC on/off, rev convergence.

**Op contract.** One function: submit a `{"seq":N}` op, poll
`/unit/op-result` until verdict or timeout, return a state machine result —
`pending / ok / failed(reason) / expired`. Used identically against S3 and
esp01 (the esp01 serves the op subset; its per-boot `maintSeqCounter` reset is
tolerated by keying polls on the submitted seq only within one connection).

**SSE.** `/events` consumer (S3 only): yields display text + `selfRow`/`rows`
wall updates, reconnects with backoff, reports connection state to the caller.

## The TUI

### Main screen (dashboard)

| Panel | Content | Source |
|---|---|---|
| Wall | all rows as flap text, live | SSE `/events` (`clusterMirrorRows` data) |
| Cluster strip | one line per member: joined/suspect/degraded, rescue, rev + convergence, rollout/`updateBlocked`, relay store state, HMAC, esp01 heap/RSSI | `/cluster/status` |
| Units | per-unit fault flags, wear, `sx`/`sxl`, vccMin, gates, odometer for the selected row | `/status` units (own row); `/cluster/status` health strips (other rows, #294 ping piggyback) |
| Log tail | newest flash-log lines | `/log/flash` |
| Stats bar | leader heap, RSSI, uptime, stack low-water | `/status` stats.now |

**Refresh rules — leader-only, and honest about staleness:**

- The wall view talks to the **leader only**. Followers are never
  background-polled (an esp01 superloop blocks on handler work; #326 documented
  what per-second traffic does to a busy follower). Follower endpoints are hit
  only from that board's detail screen, at ≥5 s cadence.
- SSE is the **only true wall-row source** (#277: the poll fallback can only
  collapse the wall via `clusterLeading`; row content relies on SSE resend).
  While SSE is down, the wall panel is marked **STALE** — it is never
  reconstructed from polls.
- Poll cadence: `/status` every 5 s (one aggregate request, not an endpoint
  scatter), `/cluster/status` every 5 s, log tail every 10 s.

### Other screens

- **Board detail** — drill into one member: its `/settings`, `/units/health`,
  foreign-contact counters, esp01 `/log` ring. The screen renders only what the
  capability table says the platform serves; everything else is absent, not
  erroring.
- **Log** — full-height tail; on S3 a toggle for `?prev=1` (previous boot).

### Command bar

k9s-style `:` prompt with history. Explicit command inventory:

| Tier | Commands | Confirm |
|---|---|---|
| Kill switch | `stop` (also bound to a dedicated key, always active — it is the one producer-gate exception in the firmware) | none |
| Routine | `text …` (`\n` = grid-row break, #290 semantics), `mode clock\|text`, `notify …` | none |
| Mutating | `set KEY VAL`, `gates …`, `op home\|calibrate\|exercise\|selftest\|odo-reset --unit N` | one-keystroke |
| Dangerous | `reboot [board]`, `promote`, `reset-units`, `addr burn\|clear --unit N`, `cluster config …`, `cluster leave` | typed confirmation (board/unit name echoed back) |

Ops show live progress from the op-result state machine. Any firmware 409/503
displays its reason verbatim. Commands a platform doesn't serve are rejected
client-side by the capability table with a message naming the platform.

**Deliberately NOT in v1** (the cut line: v1 reads everything, mutates
display/settings/ops, and never moves firmware images):

- master OTA upload, unit reflash campaigns — `flashing/ota-flash.sh` and
  `commission-units.sh` stay authoritative (they encode leader-first ordering
  and the esp01 store-relay path),
- `POST /cluster/member/update` and follower-image store uploads (relay
  *status* is displayed; triggering image movement is v2, together with the
  #419 store-wedge behavior),
- rescue/coredump/factory-slot management,
- one-shot/scriptable subcommand mode.

### Failure behavior

Leader unreachable → persistent disconnected banner, exponential-backoff
retry, app never exits on its own (OTA reboots and brownout boots are normal
life). Partial data (e.g. cluster up but SSE down) degrades per-panel, each
panel showing its own age.

## Config

`~/.config/splitflap/config.toml`: named boards (`leader`, `spare`, `row0`, …)
with IPs, default board, timeouts/poll intervals. Nothing else in v1. No
client-side mDNS (multicast doesn't cross the operator VPN); the `discover`
command runs the leader's staged `POST/GET /cluster/discover` (leader-side
mDNS scan) and prints the result.

## Testing

pytest, run in CI as a new `cli/` job alongside the existing suites.

- **Cluster-wire truth:** the client's cluster calls run against the existing
  pytest-pinned wire twins (`fake_follower.py` variants incl. esp01/rescue) —
  the same twins that pin the firmware, so the client can't drift on that wire.
- **Operator-surface fixtures:** the twins do NOT cover `/status`,
  `/units/health`, op results, etc. Those get committed JSON fixtures captured
  from live boards by a small capture script; the `/api` fixtures double as the
  capability-table drift gate (see above), keeping the fixture set anchored
  rather than rotting.
- **Contract cases:** malformed/missing fields, 409/503 bodies, op
  pending → ok / failed / expired, SSE reconnect, esp01 `notServed` gating,
  timeouts.
- **TUI layer:** Textual `Pilot` against a fake transport loaded with the
  fixtures — panels, command parsing, tiered confirm flows, disconnected
  banner, resize.

Coverage target: the usual 80% on pure logic (client library + command
parsing); visual layout is exercised, not coverage-chased.

## Follow-ups (separate issues, not v1 work)

- **Feeder daemon** (RSS / transit / airport → `splitflap_client` → leader
  grid path): own brainstorm + spec once the CLI is in use.
- **Optional firmware nicety:** additive `bootId` in `/settings` (both
  platforms) to make cross-boot op tracking airtight (the follower
  `maintSeqCounter` resets every boot). File only if the CLI proves the need.
- **v2 candidates:** OTA + reflash-campaign absorption, follower-image relay
  commands, one-shot `--json` subcommand mode for cron.
