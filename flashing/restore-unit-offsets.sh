#!/usr/bin/env bash
# Restore per-unit calibration offsets from unit-offsets.json.
#
# An erase of a unit's EEPROM loses nothing that cannot be rebuilt EXCEPT the
# hall-sensor calibration offset: the address falls back to the DIP switches
# and the counters legitimately restart at zero, but an offset only exists
# because somebody calibrated that drum by hand.
#
# The capture lives in unit-offsets.json next to this script. Re-capture with
# --capture before an erase; replay with --apply after it.
#
# Everything goes through the master's /api/v2, also for units on a row board
# (wall-api.sh). Each write is a `set-offset` job that is waited for, and
# every unit is read back afterwards: this script does not assume a write
# landed. A row board's units are read from the facts it sends the master, so
# their read-back is waited for (WALL_READBACK_S, 90 s).
#
# Usage:
#   restore-unit-offsets.sh                  dry run: compare live vs captured
#   restore-unit-offsets.sh --apply          write the captured offsets back
#   restore-unit-offsets.sh --capture        overwrite the JSON from the wall
#   restore-unit-offsets.sh --board <id>     limit to one board
#   restore-unit-offsets.sh --master <addr>  not the master named in the JSON
#
# Exit is non-zero if any unit ends up not matching its captured value.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSON="${UNIT_OFFSETS_JSON:-$HERE/unit-offsets.json}"
# shellcheck source=wall-api.sh
source "$HERE/wall-api.sh"

MODE=verify
ONLY_BOARD=""
MASTER=""
READBACK_S="${WALL_READBACK_S:-90}"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --apply)   MODE=apply; shift ;;
    --capture) MODE=capture; shift ;;
    --board)   ONLY_BOARD="${2:?--board needs a board id}"; shift 2 ;;
    --master)  MASTER="${2:?--master needs an address}"; shift 2 ;;
    -h|--help) sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -f "$JSON" ]] || { echo "missing $JSON" >&2; exit 1; }
[[ -z "$ONLY_BOARD" ]] || wall_is_board "$ONLY_BOARD" || {
  echo "--board is a board id: letters, digits, . _ -" >&2; exit 2; }
if [[ -z "$MASTER" ]]; then
  MASTER="$(jqf "d['master']" < "$JSON")"
fi
[[ -n "$MASTER" ]] || { echo "no master: give --master, or \"master\" in $JSON" >&2; exit 1; }
wall_init

# rows() emits "board<TAB>addr<TAB>offset" for every captured unit, addresses
# in numeric order.
rows() {
  python3 - "$JSON" "$ONLY_BOARD" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
only = sys.argv[2]
for row in data["rows"]:
    if only and row["board"] != only:
        continue
    for addr in sorted(row["offsets"], key=int):
        print(f"{row['board']}\t{addr}\t{row['offsets'][addr]}")
PY
}

# Materialise the row list BEFORE any loop. Fed in as `done < <(rows)` the
# generator sits in a process substitution, where a failure — malformed JSON,
# a schema change, no python3 — never trips errexit: the loop runs zero times
# and the script exits 0 having printed nothing. A typo'd --board does the
# same. Both are indistinguishable from "all units verified", the worst lie
# from the script that restores calibration after an erase.
ROWS="$(rows)"
if [[ -z "$ROWS" ]]; then
  echo "no units matched${ONLY_BOARD:+ --board $ONLY_BOARD} in $JSON" >&2
  exit 1
fi
BOARDS="$(cut -f1 <<< "$ROWS" | uniq)"
while IFS=$'\t' read -r board addr want; do
  if ! wall_is_board "$board" || ! wall_is_number "$addr" || ! [[ "$want" =~ ^-?[0-9]+$ ]]; then
    echo "not a board id, a unit address and an offset in $JSON: '$board' '$addr' '$want'" >&2
    exit 1
  fi
done <<< "$ROWS"

# One read per board: LIVE holds "board<TAB>addr<TAB>offset" lines.
read_live() {
  local board out
  LIVE=""
  for board in $BOARDS; do
    out="$(wall_offsets "$board")" || out=""
    if [[ -n "$out" ]]; then LIVE+="$(sed "s/^/$board\t/" <<< "$out")"$'\n'; fi
  done
}

live_offset() {  # board addr -> the offset, or nothing when it was not read
  awk -F'\t' -v b="$1" -v a="$2" '$1==b && $2==a {print $3}' <<< "$LIVE"
}

if [[ "$MODE" == capture ]]; then
  echo "Re-capturing from the wall into $JSON"
  read_live
  LIVE="$LIVE" python3 - "$JSON" "$ONLY_BOARD" "$MASTER" <<'PY'
import json, os, sys, datetime
path, only, master = sys.argv[1:4]
live = {}
for line in os.environ["LIVE"].splitlines():
    board, addr, offset = line.split("\t")
    live[(board, addr)] = int(offset)
data = json.load(open(path))
for row in data["rows"]:
    if only and row["board"] != only:
        continue
    fresh = {}
    for addr in sorted(row["offsets"], key=int):
        if (row["board"], addr) in live:
            fresh[addr] = live[(row["board"], addr)]
        else:
            print(f"  {row['board']} a{addr}: NOT READ - keeping the captured value")
            fresh[addr] = row["offsets"][addr]
    row["offsets"] = fresh
    print(f"  {row['board']}: {fresh}")
data["master"] = master
data["capturedUtc"] = datetime.datetime.now(datetime.timezone.utc) \
    .replace(microsecond=0).isoformat().replace("+00:00", "Z")
json.dump(data, open(path, "w"), indent=2)
open(path, "a").write("\n")
PY
  echo "Captured. Review the diff before committing."
  exit 0
fi

read_live
fail=0
changed=0
matched=0
WRITTEN=""
while IFS=$'\t' read -r board addr want; do
  matched=$((matched + 1))
  have="$(live_offset "$board" "$addr")"

  if [[ "$MODE" == verify ]]; then
    if [[ -z "$have" ]]; then
      printf '%s  a%-3s  captured %4s  live UNREADABLE\n' "$board" "$addr" "$want"
      fail=1
    elif [[ "$have" == "$want" ]]; then
      printf '%s  a%-3s  %4s  ok\n' "$board" "$addr" "$want"
    else
      printf '%s  a%-3s  captured %4s  live %4s  DIFFERS\n' \
        "$board" "$addr" "$want" "$have"
      changed=1
      fail=1  # not verified IS a failure — see the exit contract in the header
    fi
    continue
  fi

  # apply
  if [[ "$have" == "$want" ]]; then
    printf '%s  a%-3s  %4s  already set\n' "$board" "$addr" "$want"
    continue
  fi
  if ! why="$(wall_job "$(wall_job_json set-offset "$board" "$addr" offset "$want")" 60)"; then
    printf '%s  a%-3s  WRITE FAILED: %s\n' "$board" "$addr" "$why"
    fail=1
    continue
  fi
  WRITTEN+="$board"$'\t'"$addr"$'\t'"$want"$'\n'
done <<< "$ROWS"

# Read back what was written, waiting for the units a row board has yet to
# report again.
if [[ -n "$WRITTEN" ]]; then
  deadline=$(( SECONDS + READBACK_S ))
  while :; do
    read_live
    pending=0
    while IFS=$'\t' read -r board addr want; do
      [[ -n "$board" ]] || continue
      [[ "$(live_offset "$board" "$addr")" == "$want" ]] || pending=1
    done <<< "$WRITTEN"
    if (( pending == 0 || SECONDS >= deadline )); then break; fi
    sleep "$WALL_POLL_S"
  done
  while IFS=$'\t' read -r board addr want; do
    [[ -n "$board" ]] || continue
    back="$(live_offset "$board" "$addr")"
    if [[ "$back" == "$want" ]]; then
      printf '%s  a%-3s  %4s  restored\n' "$board" "$addr" "$want"
    else
      printf '%s  a%-3s  wanted %4s  read back %4s  MISMATCH\n' \
        "$board" "$addr" "$want" "${back:-<unreadable>}"
      fail=1
    fi
  done <<< "$WRITTEN"
fi

# Belt and braces: the non-empty guard above should make this unreachable.
if [[ "$matched" == 0 ]]; then
  echo "read zero rows — refusing to report success" >&2
  exit 1
fi

if [[ "$MODE" == verify && "$changed" == 1 ]]; then
  echo
  echo "Live values differ from the capture. If the wall is the truth, re-run"
  echo "with --capture; if the capture is the truth, re-run with --apply."
fi

exit "$fail"
