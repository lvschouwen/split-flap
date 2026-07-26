#!/usr/bin/env bash
# Restore per-unit calibration offsets from unit-offsets.json.
#
# The #406 day-0 EEPROM re-layout erases and re-initialises every unit's
# EEPROM. Everything in there is reconstructible EXCEPT the hall-sensor
# calibration offset: the I2C address falls back to the DIP switches (which
# is what twiboot uses anyway) and the counters legitimately restart at zero,
# but an offset only exists because somebody calibrated that drum by hand.
#
# Capture lives in unit-offsets.json next to this script. Re-capture with
# --capture before a reflash; replay with --apply after it.
#
# Each write goes through the master's async maintenance contract
# (POST /unit/offset -> {"seq":N} -> GET /unit/op-result), and every unit is
# read back afterwards — a silent SET_OFFSET is exactly the unverified path
# #405 exists to close, so this script does not assume the write landed.
#
# Usage:
#   restore-unit-offsets.sh                  dry run: compare live vs captured
#   restore-unit-offsets.sh --apply          write the captured offsets back
#   restore-unit-offsets.sh --capture        overwrite the JSON from the wall
#   restore-unit-offsets.sh --host <ip>      limit to one row
#
# Exit is non-zero if any unit ends up not matching its captured value.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSON="$HERE/unit-offsets.json"

MODE=verify
ONLY_HOST=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --apply)   MODE=apply; shift ;;
    --capture) MODE=capture; shift ;;
    --host)    ONLY_HOST="${2:?--host needs an address}"; shift 2 ;;
    -h|--help) sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -f "$JSON" ]] || { echo "missing $JSON" >&2; exit 1; }

# rows() emits "host<TAB>row<TAB>addr<TAB>offset" for every captured unit,
# addresses in numeric order.
rows() {
  python3 - "$JSON" "$ONLY_HOST" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
only = sys.argv[2]
for row in data["rows"]:
    if only and row["host"] != only:
        continue
    for addr in sorted(row["offsets"], key=int):
        print(f"{row['host']}\t{row['row']}\t{addr}\t{row['offsets'][addr]}")
PY
}

get_offset() {  # host addr -> prints the integer, or nothing on failure
  curl -sf --max-time 8 "http://$1/unit/offset?address=$2" 2>/dev/null |
    python3 -c 'import sys,json; print(json.load(sys.stdin)["offset"])' 2>/dev/null || true
}

if [[ "$MODE" == capture ]]; then
  echo "Re-capturing from the wall into $JSON"
  python3 - "$JSON" "$ONLY_HOST" <<'PY'
import json, subprocess, sys, datetime
path, only = sys.argv[1], sys.argv[2]
data = json.load(open(path))
for row in data["rows"]:
    if only and row["host"] != only:
        continue
    host = row["host"]
    subprocess.run(["curl", "-sf", "--max-time", "20",
                    f"http://{host}/units/health?refresh=1"],
                   stdout=subprocess.DEVNULL, check=False)
    fresh = {}
    for addr in sorted(row["offsets"], key=int):
        out = subprocess.run(
            ["curl", "-sf", "--max-time", "8",
             f"http://{host}/unit/offset?address={addr}"],
            capture_output=True, text=True, check=False)
        try:
            fresh[addr] = json.loads(out.stdout)["offset"]
        except Exception:
            print(f"  {host} a{addr}: READ FAILED - keeping captured value")
            fresh[addr] = row["offsets"][addr]
    row["offsets"] = fresh
    print(f"  {host}: {fresh}")
data["capturedUtc"] = datetime.datetime.now(datetime.timezone.utc) \
    .replace(microsecond=0).isoformat().replace("+00:00", "Z")
json.dump(data, open(path, "w"), indent=2)
open(path, "a").write("\n")
PY
  echo "Captured. Review the diff before committing."
  exit 0
fi

# Materialise the row list BEFORE the loop. Feeding it in as `done < <(rows)`
# put rows() inside a process substitution, where a failure — malformed JSON, a
# schema change, no python3 — never trips errexit: the loop just ran zero times,
# `fail` stayed 0, and the script exited 0 having printed nothing. A typo'd
# --host did exactly the same. Both are indistinguishable from "all units
# verified", which is the worst possible lie from the one script that restores
# calibration after a destructive erase.
ROWS="$(rows)"
if [[ -z "$ROWS" ]]; then
  echo "no units matched${ONLY_HOST:+ --host $ONLY_HOST} in $JSON" >&2
  exit 1
fi

fail=0
changed=0
matched=0
while IFS=$'\t' read -r host row addr want; do
  matched=$((matched + 1))
  have="$(get_offset "$host" "$addr")"

  if [[ "$MODE" == verify ]]; then
    if [[ -z "$have" ]]; then
      printf 'row %s  a%-3s  captured %4s  live UNREADABLE\n' "$row" "$addr" "$want"
      fail=1
    elif [[ "$have" == "$want" ]]; then
      printf 'row %s  a%-3s  %4s  ok\n' "$row" "$addr" "$want"
    else
      printf 'row %s  a%-3s  captured %4s  live %4s  DIFFERS\n' \
        "$row" "$addr" "$want" "$have"
      changed=1
      fail=1  # not verified IS a failure — see the exit contract in the header
    fi
    continue
  fi

  # apply
  if [[ "$have" == "$want" ]]; then
    printf 'row %s  a%-3s  %4s  already set\n' "$row" "$addr" "$want"
    continue
  fi
  if ! curl -sf --max-time 10 -X POST \
        "http://$host/unit/offset?address=$addr&value=$want" >/dev/null; then
    printf 'row %s  a%-3s  POST FAILED\n' "$row" "$addr"
    fail=1
    continue
  fi
  # The unit persists the offset in loop context, not in the Wire ISR — give
  # the write a beat before reading it back through the next probe.
  sleep 2
  back="$(get_offset "$host" "$addr")"
  if [[ "$back" == "$want" ]]; then
    printf 'row %s  a%-3s  %4s  restored\n' "$row" "$addr" "$want"
  else
    printf 'row %s  a%-3s  wanted %4s  read back %4s  MISMATCH\n' \
      "$row" "$addr" "$want" "${back:-<unreadable>}"
    fail=1
  fi
done <<< "$ROWS"

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
