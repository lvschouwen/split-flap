# splitflap

Operator TUI for the split-flap wall (#441). A Textual dashboard + command
bar talking to the v2 masters' existing HTTP/JSON surface over your LAN/VPN
— no new firmware endpoints, no MQTT dependency.

## Install

```bash
pipx install ./cli
```

(or, for development: `cd cli && python -m venv .venv && .venv/bin/pip install -e ".[dev]"`)

This installs the `splitflap` command (`splitflap_tui/__main__.py`).

## Configure

Create `~/.config/splitflap/config.toml`:

```toml
default = "leader"

[[boards]]
name = "leader"
url = "http://192.168.15.88"

[[boards]]
name = "row0"
url = "http://192.168.15.121"
```

`default` names the board the dashboard polls on startup (normally the
cluster leader — it's the only board that serves `/status`, `/cluster/status`,
`/log/flash`, and the SSE `/events` wall mirror). Every entry in `boards`
becomes reachable from the `b` board-detail cycle regardless of platform.
Optional: `poll_s` (default `5.0`) and `log_poll_s` (default `10.0`) tune the
dashboard's own poll cadence — board-detail polling has its own 5 s floor,
independent of these, because it may be aimed at an ESP-01 follower's
superloop.

## Run

```bash
splitflap
```

The dashboard wears an amber board-strip identity (`splitflap_tui/splitflap.tcss`,
#450) and mirrors the wall itself: the `wall` panel renders each display
character as a discrete flap cell (amber-on-dark, one cell per unit) when
there's room, degrading to plain text on a narrow terminal rather than
wrapping cells.

The `units` panel reports **lifetime** wear (#455): `sxl` is cumulative
steps-to-home and `hf` the hall-failure count, both read straight from the
unit's own EEPROM. The `flags` column adds two derived markers on top of
`STALE`/`FAULT`:

| Marker | Meaning |
|---|---|
| `HALL` | the unit has recorded at least one hall failure (`hf`) — the firmware's own escalation bar is 3 |
| `DRAG` | `sxl` is at least 8× its row's median and at least 100 — friction or a slipping coupling, not a sensor |

`DRAG` is relative to the row median rather than an absolute number, so a
wall that ages evenly does not light up end to end, and a re-calibrated row
re-baselines itself. The flip side is that it needs a majority-healthy row
to have something to be an outlier from. A unit can carry both markers.

Note that a board's own `faulty` count answers "is this unit responding?",
not "is it wearing out" — a wall can report `faulty: 0` while carrying units
that need physical attention, which is exactly what these markers surface.

## Keybindings

| Key | Action |
|---|---|
| `:` | open the command bar |
| `?` | open the command reference/help overlay |
| `b` | push the board-detail screen for the next board in `config.boards` (repeat to cycle) |
| `l` | push the leader's flash-log screen |
| `s` | push the board-health screen (OTA, rescue slot, boot cause, task stacks, vitals) |
| `u` | focus the units table — ↑/↓ pick a unit, `enter` opens its detail screen, `escape` releases focus |
| `r` | *(discover screen only)* re-run the scan |
| `ctrl+s` | STOP — blank + halt the wall immediately, no confirm (always active, even mid-command-entry) |
| `escape` | abandon a half-typed command and close the command bar |
| `q` | quit |

Inside the board-detail screen: `escape` pops back to the dashboard and stops
that board's poll thread — a follower is only polled while its screen is
open. Inside the log screen: `p` toggles between the current and previous
boot's flash log (`?prev=1`); `escape` pops back.

## Command bar

Press `:` to open it, type a command, Enter to submit, `escape` to abandon
it — escape clears the bar and hands focus back, so the dashboard hotkeys
keep working. The command name
autocompletes inline as you type, suggesting from the same vocabulary as
the table below; **Tab accepts the suggestion** (so does `→`). Enter always
submits exactly what is typed — a unique prefix is never auto-expanded,
because `stop` is a no-confirm kill command and `st` + Enter must not be
able to blank the wall. Type `help` (or `:help`, or press `?` — see Keybindings)
to open a command reference overlay generated straight from the command
table, so it can't drift out of sync. Submitted lines persist across runs:
the last 100 are saved to `~/.config/splitflap/history` and reloaded at
startup (best-effort — an unwritable config dir never blocks the app);
recall them with up/down while the bar is focused.

Commands are tiered by risk:

- **kill** — runs immediately, no confirm.
- **routine** — runs immediately, no confirm.
- **confirm** — a `y`/`n` modal first.
- **typed** — a modal that requires typing the echoed token (unit address or
  command name) exactly, then Enter.

| Command | Tier | Notes |
|---|---|---|
| `stop` | kill | blank + halt the wall |
| `text <text…>` | routine | rest of the line verbatim; a literal `\n` forces a grid-row break (#290) |
| `mode text\|clock` | routine | |
| `notify <dwell-s> <text…>` | routine | transient overlay |
| `set <field> <value>` | confirm | raw settings-form write |
| `op home\|jog\|identify\|self-test\|reset-odometer\|offset <unit> [value]` | confirm | `jog`/`offset` require the value arg |
| `gates <unit> <mask>` | confirm | `SET_GATES` mask, hex or decimal (`0x02` or `2`) |
| `reboot [board]` | typed | targets `board` (config name) or the default board when omitted; token = the board name when given, else `reboot` |
| `reset-units` | typed | token = `reset-units` |
| `addr burn <unit> <target>` | typed | token = the unit address |
| `addr clear <unit>` | typed | token = the unit address |
| `promote` | typed | token = `promote` |
| `cluster leave` | typed | token = `cluster-leave` |
| `cluster config <host\|row\|col\|width;…>` | typed | token = `config` |
| `discover` | routine | scan the LAN for boards — opens its own result screen |
| `baseline [clear]` | routine | snapshot unit wear locally; adds the `Δsxl` column |

`text` is normally routine (no confirm), but while the wall's last known
mode is `clock` it routes through the confirm modal instead — a warning,
not a block, since the clock reclaims the display at the next minute tick
anyway (`:mode text` first avoids the fight).

### `discover`

`discover` is the one command whose answer is a table rather than a status
line, so it opens its own screen: `scanning…`, then one row per board found
(name / host / rev / width / plat), `r` to rescan, `escape` to go back. The
`host` column is exactly what a `cluster config` member table wants — a
dotted quad, or `<name>.local` when the mDNS answer carried no address.

There is **no client-side mDNS** — multicast doesn't cross the operator VPN.
The scan runs on the *leader*: `POST /cluster/discover` arms a browse that
runs in its netTask, and the GET is polled until it reports done (10 s
deadline, 500 ms cadence — the same bound the Web UI's Cluster card uses).
Because mDNS is link-local, an empty result is the normal answer for
anything not on the leader's own subnet, so the screen says "no boards
found" rather than reporting a failure. The leader filters its own
advertisement out and returns at most 8 boards. A board that advertises no
`plat` is an S3 (only foreign platforms tag themselves), so that column
never renders blank.

`discover` is not served on an ESP-01 at all — asking a follower to scan is
rejected client-side by the capability table.

### Board health (`s`)

Everything the leader reports about its own condition, on one screen and
costing no extra request — it renders the same `/status` the dashboard is
already polling, and keeps re-rendering while open:

- **image** — running/next OTA slot, last flash result, last invalid image,
  whether the last OTA reverted, factory-slot validity;
- **rescue** — rescue-slot state and its warning flag;
- **boot** — last reset cause and uptime;
- **tasks** — per-task stack low-water marks, **smallest flagged**. These are
  *bytes still free at the worst point since boot*, so the smallest is the
  closest to overflow — the failure mode that took out clusterTask (#437);
- **system** — heap, min heap, largest allocatable block (fragmentation),
  PSRAM, per-core load, die temperature, RSSI, NTP age, I2C counters;
- **config** — role, mode, reflash-on-boot, cluster leader.

Anything the board didn't report reads `-`, never `0`: these keys are
validity-gated firmware-side, so absent genuinely means "no reading". Before
the first successful poll the screen says so outright rather than showing a
page of dashes that would look like data.

### Unit detail (`u`, then `enter`)

The dashboard table has room for eight columns; a unit answers about
thirty-seven. The detail screen shows all of them grouped (identity, wear,
self-test, homing, power, bus) plus the per-unit I2C error attribution
(`err`/`errAge`) that the wall only emits once an error has been charged.

Two rows are composed because the numbers only mean something in pairs:

| Row | Reads like | Why |
|---|---|---|
| `worst step-excess` | `1465 lifetime · 0 since boot` | `sxl` never forgets; `sx` resets at reboot. A bad lifetime figure with a clean since-boot figure means the unit misbehaved **historically** and has been fine this boot |
| `hall window` / `steps/rev` | `48 → 47`, `2050 → 2051` | the unit keeps its own FIRST self-test measurements alongside its latest, so the pair is a drift check with no host-side bookkeeping |

Actions run against the selected unit: `h` home, `i` identify, `t`
self-test, `z` reset-odometer go straight through the normal confirm tiers.
`o` offset, `j` jog and `g` gates need a value, so they pre-fill the command
bar with the address already typed instead of firing on one keystroke.
`identify` is usually the one you want first — it's how you find a unit on a
physical wall.

The labels are our wording for the firmware's keys, and a test gates them
against the board's own `/api` legend, so a firmware rename fails the suite
instead of leaving a column quietly showing `-` forever.

### Wear baselines (`:baseline`)

`sxl` is a lifetime high-water mark in the unit's own EEPROM. That makes it
trustworthy across reboots and useless as a before/after: after servicing a
unit, its lifetime figure still reads whatever its worst day was. `:baseline`
snapshots the current wear to `~/.config/splitflap/wear-baseline.json`, and
the units table grows a **`Δsxl`** column — a serviced unit's delta goes
flat, a failing one's keeps climbing. `:baseline clear` drops it.

A unit the baseline never saw (added or re-addressed since) reads `—`, not a
delta of 0. So does a unit whose lifetime counter went *backwards*, which
would mean the address now points at different hardware or an erased EEPROM
— never a negative number that would look like an improvement.

This is not a replacement for a master-side history (#465): it only records
while the TUI is running, whereas the master runs around the clock. It is
cheaper — no flash, no firmware change — and blinder.

Every route is gated client-side against the connected board's platform
capability table (`splitflap_client.capability`) before it's even offered a
confirm — a command not served on the current platform is rejected with
`⛔ <name>: not served on <plat>` and never reaches the wire. Every HTTP
error the board returns is surfaced verbatim (`⛔ <status>: <body>`), never
paraphrased.

## What this tool does NOT do

This TUI never moves firmware images — no `/firmware/*` upload, no
`/reflash-units`, no `/cluster/follower-firmware`. Flashing stays on
`flashing/ota-flash.sh` and the unit-campaign scripts, which already own
that surface (staged bins, MD5 verdicts, multi-device fan-out). The TUI is
read/operate only: display text, settings, unit ops, cluster membership
commands, and diagnostics.
