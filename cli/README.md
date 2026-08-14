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

## Keybindings

| Key | Action |
|---|---|
| `:` | open the command bar |
| `?` | open the command reference/help overlay |
| `b` | push the board-detail screen for the next board in `config.boards` (repeat to cycle) |
| `l` | push the leader's flash-log screen |
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
the table below. Type `help` (or `:help`, or press `?` — see Keybindings)
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

`text` is normally routine (no confirm), but while the wall's last known
mode is `clock` it routes through the confirm modal instead — a warning,
not a block, since the clock reclaims the display at the next minute tick
anyway (`:mode text` first avoids the fight).

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
