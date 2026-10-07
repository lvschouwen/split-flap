#!/usr/bin/env bash
# Commission the units of one board, one at a time, stopping at the first
# thing that looks wrong: update -> verify identity -> restore offset -> zero
# the odometer -> home -> exercise -> baseline self-test -> exercise ->
# compare self-test -> verdict.
#
# Everything goes through the master's /api/v2 (wall-api.sh), also for the
# units of a row board. Each step is a job that is waited for.
#
# Requires `updateUnitsAtStart` false in the wall's settings, so no board
# updates its units by itself before this script gets a vote.
#
# WHY THE UPDATE COMES FIRST, and is also the address check. A unit with an
# erased EEPROM answers at the address its DIP switches give. A targeted
# update proves the two agree WITHOUT risking anything: the board sends the
# unit at that address into its bootloader, rescans, and only writes what it
# finds in the bootloader AT that address. If the switches disagree, nothing
# is in the bootloader there and not a byte is written.
#
# WHY THE BASELINE SELF-TEST IS NOT FIRST. On an erased unit the first PASSING
# self-test becomes the reference that unit compares itself against for the
# rest of its life (it never overwrites the first). Taking it on a cold drum
# bakes in a bad reference and makes later degradation read as improvement.
# Exercise first, then baseline.
#
# Usage:
#   commission-units.sh --board <id>            all captured units of that board
#   commission-units.sh --board <id> --only 3   one unit
#   commission-units.sh --board <id> --from 7   resume mid-campaign
#   commission-units.sh --board <id> --dry-run  print the plan, touch nothing
#   commission-units.sh --board <id> --skip 15  exclude a known-bad unit
#   commission-units.sh --master <addr> ...     not the master named in the JSON
#
# A board's id is the master's own name or the id a row board paired under
# (GET /api/v2/wall). A unit that cannot find home fails homing and both
# self-tests: repair it first or --skip it, so its failures are not what
# proves the script works.
#
# Exit is non-zero the moment a unit fails. Nothing after it is touched.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSON="${UNIT_OFFSETS_JSON:-$HERE/unit-offsets.json}"
# shellcheck source=wall-api.sh
source "$HERE/wall-api.sh"

MASTER=""
BOARD=""
ONLY=""
FROM=""
SKIP=""
DRY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --master)  MASTER="${2:?--master needs an address}"; shift 2 ;;
    --board)   BOARD="${2:?--board needs a board id}"; shift 2 ;;
    --only)    ONLY="${2:?--only needs an address}"; shift 2 ;;
    --from)    FROM="${2:?--from needs an address}"; shift 2 ;;
    --skip)    SKIP="${2:?--skip needs a comma-separated list}"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) sed -n '2,39p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$BOARD" ]]     || { echo "--board is required" >&2; exit 2; }
[[ -f "$JSON" ]]      || { echo "missing $JSON" >&2; exit 1; }
if [[ -z "$MASTER" ]]; then
  MASTER="$(jqf "d['master']" < "$JSON")"
fi
[[ -n "$MASTER" ]] || { echo "no master: give --master, or \"master\" in $JSON" >&2; exit 1; }

# --- exercise geometry -------------------------------------------------------
# The drum is 2038 steps; a jog is capped at ±127 (one signed wire byte). 17
# jogs of 120 steps is 2040 — one revolution to within two steps, and the
# following home re-syncs the belief anyway. Two rounds is 34.
JOG_STEPS=120
JOGS_PER_ROUND=17
ROUNDS="${COMMISSION_ROUNDS:-2}"

# Motor cooldown between motion phases. The self-test alone is ~2 revolutions
# of continuous stepping; back to back with two exercise phases that is ~6 on a
# 28BYJ-48 whose overheat gate is only 2 s and was never sized for this.
COOL_S="${COMMISSION_COOL_S:-20}"

# How long a changed fact may take to show in the unit's document. The master
# reads one of its own units about every 3 s, a full 16-unit round is ~48 s,
# and a row board's facts reach the master after its own round: a deadline
# inside one round makes the check a coin flip.
FACT_S="${COMMISSION_FACT_S:-150}"

# --- helpers -----------------------------------------------------------------

unit_field() {  # addr, python expression over the unit's document -> value, or empty
  wall_unit "$BOARD" "$1" | jqf "$2"
}

job() {  # seconds, name, addr [argkey argvalue] -> how it ended; non-zero unless done
  local wait_s="$1"; shift
  wall_job "$(wall_job_json "$1" "$BOARD" "${2:-}" "${3:-}" "${4:-}")" "$wait_s"
}

await_field() {  # addr, expression, wanted -> the last value read; non-zero when it never matched
  local deadline=$(( SECONDS + FACT_S )) got=""
  while :; do
    got="$(unit_field "$1" "$2")"
    if [[ "$got" == "$3" ]]; then printf '%s' "$got"; return 0; fi
    if (( SECONDS >= deadline )); then printf '%s' "$got"; return 1; fi
    sleep "$WALL_POLL_S"
  done
}

# Prints only. Deliberately returns 0: used as `cmd || { fail "..."; return 1; }`,
# a non-zero fail() would make the group's status depend on bash's errexit
# exemption rules for || lists, which is exactly the kind of subtlety this
# script is supposed to be free of.
fail() { printf '  FAIL: %s\n' "$*"; }

# --- plan --------------------------------------------------------------------

ADDRS="$(python3 - "$JSON" "$BOARD" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
for row in data["rows"]:
    if row["board"] != sys.argv[2]:
        continue
    for addr in sorted(row["offsets"], key=int):
        print(addr)
PY
)"
# Materialised BEFORE the loop on purpose: `done < <(...)` puts the generator in
# a process substitution whose failure never trips errexit, so a typo'd --board
# runs zero units and exits 0 — indistinguishable from "everything passed".
[[ -n "$ADDRS" ]] || { echo "no units for board $BOARD in $JSON" >&2; exit 1; }

PLAN=()
for a in $ADDRS; do
  [[ -n "$ONLY" && "$a" != "$ONLY" ]] && continue
  [[ -n "$FROM" && "$a" -lt "$FROM" ]] && continue
  [[ ",$SKIP," == *",$a,"* ]] && { echo "skipping a$a (--skip)"; continue; }
  PLAN+=("$a")
done
[[ ${#PLAN[@]} -gt 0 ]] || { echo "plan is empty after filters" >&2; exit 1; }

echo "master      $MASTER"
echo "board       $BOARD"
echo "units       ${PLAN[*]}"
echo "exercise    $ROUNDS round(s) = $((ROUNDS * JOGS_PER_ROUND)) jogs of $JOG_STEPS steps"
echo

if (( DRY )); then
  echo "dry run — nothing was touched."
  exit 0
fi

wall_init
wall_boards | grep -qxF "$BOARD" || { echo "$BOARD is not a board of the wall at $MASTER" >&2; exit 1; }

WANT_REV="$(wall_get /api/v2/firmware | jqf "d['units']['shouldBe']")"
[[ -n "$WANT_REV" ]] || { echo "the master does not say which unit firmware it holds" >&2; exit 1; }
echo "unit image  $WANT_REV"
echo

# The brake must be on, or a board updates its units behind our back at its
# next start and this script's whole premise is gone. "Unreadable" and "on"
# must not look the same, hence the explicit compare with False.
BRAKE="$(wall_get /api/v2/settings/wall | jqf "d['updateUnitsAtStart']")"
if [[ "$BRAKE" != "False" ]]; then
  echo "REFUSING: updateUnitsAtStart is '${BRAKE:-unreadable}', expected false." >&2
  echo "  curl -X PUT -H 'Content-Type: application/json' -d '{\"updateUnitsAtStart\":false}' \\" >&2
  echo "       'http://$MASTER/api/v2/settings/wall'" >&2
  exit 1
fi

# --- per unit ----------------------------------------------------------------

commission() {  # addr -> 0 pass, 1 fail
  local a="$1" why before_fw before_hf want_off
  printf '=== a%s ===\n' "$a"

  before_fw="$(unit_field "$a" "d['firmware']['status']")"
  before_hf="$(unit_field "$a" "d['drum']['homeFailures']")"
  printf '  before      state=%s rev=%s (%s) homeFailures=%s\n' \
    "$(unit_field "$a" "d['state']")" "$(unit_field "$a" "d['firmware']['rev']")" \
    "${before_fw:-?}" "${before_hf:-0}"

  # 1. update — also the DIP/EEPROM address proof (see the header)
  if [[ "$before_fw" == "current" ]]; then
    printf '  update      already current, skipping\n'
  else
    why="$(job 240 update-units "$a" force 1)" || { fail "unit update did not complete: $why"; return 1; }
    printf '  update      %s\n' "$why"
    # The master's own row counts what the update planned; a row board does
    # not say, and the identity step below is then the only proof.
    local planned
    planned="$(wall_get "/api/v2/board/$BOARD" | jqf "d['unitUpdate']['total']")"
    if [[ "$planned" == "0" ]]; then
      fail "nothing was planned for a$a — its bootloader did not answer at this
        address, which means the DIP switches disagree with the burned
        address. STOP: do not erase anything else until that is resolved."
      return 1
    fi
  fi

  # 2. identity. "current" is the master's judgement of the unit's image
  # against the one it holds, which a rev compare here would get wrong for an
  # image that is the same under another rev.
  why="$(await_field "$a" "d['firmware']['status']" current)" || {
    fail "firmware reads '${why:-<unreadable>}', wanted current"; return 1; }
  local st sup
  st="$(unit_field "$a" "d['state']")"
  sup="$(unit_field "$a" "d['firmware'].get('protocolSupported', True)")"
  [[ "$st" == "running" ]] || { fail "state is '$st', wanted running"; return 1; }
  [[ "$sup" == "True" ]]   || { fail "the unit speaks a protocol the master does not"; return 1; }
  printf '  identity    rev=%s current\n' "$(unit_field "$a" "d['firmware']['rev']")"

  # 3. offset — the one thing an erase destroys that is not reconstructible
  want_off="$(python3 - "$JSON" "$BOARD" "$a" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
for row in data["rows"]:
    if row["board"] == sys.argv[2] and sys.argv[3] in row["offsets"]:
        print(row["offsets"][sys.argv[3]])
PY
)"
  [[ -n "$want_off" ]] || { fail "no captured offset for a$a"; return 1; }
  why="$(job 60 set-offset "$a" offset "$want_off")" || { fail "offset write did not confirm: $why"; return 1; }
  why="$(await_field "$a" "d['drum']['offset']" "$want_off")" || {
    fail "offset read back '${why:-<unreadable>}', wanted '$want_off'"; return 1; }
  printf '  offset      %s restored\n' "$want_off"

  # 3b. odometer — zeroed BEFORE the exercise so the readings below measure
  # something. An erase rewrites only the head of the EEPROM, and the ring's
  # scan can then read bytes an older, smaller ring never wrote: one unit
  # started claiming 0x3C3C3C3C turns (#417). The job is acknowledged before
  # the ring's writes land, so the value is waited for.
  why="$(job 60 reset-odometer "$a")" || { fail "odometer reset did not confirm: $why"; return 1; }
  why="$(await_field "$a" "d['drum']['turns']" 0)" || {
    fail "odometer reads '${why:-<unreadable>}' after reset, wanted 0"; return 1; }
  printf '  odometer    zeroed\n'

  # 4. home
  why="$(job 150 home "$a")" || { fail "home did not complete: $why"; return 1; }
  why="$(await_field "$a" "d['drum']['home']" homed)" || {
    fail "not homed after home (home=${why:-<unreadable>})"; return 1; }
  local hf
  hf="$(unit_field "$a" "d['drum']['homeFailures']")"
  if [[ "${hf:-0}" != "${before_hf:-0}" ]]; then
    fail "lifetime failed-homing count rose ${before_hf:-0} -> ${hf:-0}"
    return 1
  fi
  printf '  home        ok (homeFailures=%s)\n' "${hf:-0}"

  # 5/6/7/8. exercise, baseline, exercise, compare
  local base_win base_spr win spr out
  exercise "$a" || return 1
  sleep "$COOL_S"
  out="$(self_test "$a")" || { printf '%s\n' "$out"; return 1; }
  read -r base_win base_spr <<< "$out"
  printf '  self-test   baseline hall_window=%s steps_per_rev=%s\n' "$base_win" "$base_spr"
  sleep "$COOL_S"
  exercise "$a" || return 1
  sleep "$COOL_S"
  out="$(self_test "$a")" || { printf '%s\n' "$out"; return 1; }
  read -r win spr <<< "$out"
  printf '  self-test   after    hall_window=%s steps_per_rev=%s\n' "$win" "$spr"

  # A drum that measures differently after a dozen revolutions is telling you
  # something before it stops finding home. 10% on the window, 1% on steps/rev.
  python3 - "$base_win" "$win" "$base_spr" "$spr" <<'PY' || return 1
import sys
bw, w, bs, s = (int(x) for x in sys.argv[1:5])
bad = []
if bw and abs(w - bw) > max(2, bw * 0.10):
    bad.append(f"hall_window {bw} -> {w}")
if bs and abs(s - bs) > max(4, bs * 0.01):
    bad.append(f"steps_per_rev {bs} -> {s}")
if bad:
    print("  FAIL: self-test drifted across the run: " + "; ".join(bad))
    sys.exit(1)
PY

  printf '  PASS        a%s commissioned\n\n' "$a"
}

exercise() {  # addr -> spins the drum ROUNDS revolutions via jog
  local a="$1" n=$(( ROUNDS * JOGS_PER_ROUND )) i why
  local odo_before odo_after
  odo_before="$(unit_field "$a" "d['drum']['turns']")"
  printf '  exercise    %s jogs' "$n"
  for (( i = 0; i < n; i++ )); do
    why="$(job 30 jog "$a" steps "$JOG_STEPS")" || { echo; fail "jog $i did not complete: $why"; return 1; }
    printf '.'
  done
  odo_after="$(unit_field "$a" "d['drum']['turns']")"
  printf ' turns %s -> %s\n' "${odo_before:-?}" "${odo_after:-?}"
  # The odometer counts COMMANDED steps, so this proves the jogs landed — not
  # that the drum physically turned. Physical truth is the self-test compare.
  if [[ -n "$odo_before" && -n "$odo_after" && "$odo_after" -lt "$odo_before" ]]; then
    fail "odometer went backwards"
    return 1
  fi
}

# Prints "hall_window steps_per_rev"; on failure the FAIL line instead.
self_test() {  # addr
  local a="$1" why body idfile win spr
  idfile="$(mktemp)"
  if ! why="$(WALL_JOB_ID_FILE="$idfile" job 180 self-test "$a")"; then
    body="$(wall_op "$(cat "$idfile")" 2>/dev/null || true)"
    rm -f "$idfile"
    fail "self-test failed: $why $(printf '%s' "$body" | jqf "'(the unit: '+d['data']['unit_reason']+')'")"
    return 1
  fi
  body="$(wall_op "$(cat "$idfile")")"
  rm -f "$idfile"
  win="$(printf '%s' "$body" | jqf "d['data']['hall_window']")"
  spr="$(printf '%s' "$body" | jqf "d['data']['steps_per_rev']")"
  if ! [[ "$win" =~ ^[0-9]+$ && "$spr" =~ ^[0-9]+$ ]]; then
    fail "self-test gave no measurements"
    return 1
  fi
  printf '%s %s\n' "$win" "$spr"
}

for a in "${PLAN[@]}"; do
  if ! commission "$a"; then
    echo
    echo "STOPPED at a$a. ${#PLAN[@]} unit(s) were planned; everything after"
    echo "a$a is untouched. Fix the cause before continuing:"
    echo "  commission-units.sh --board $BOARD --from $a"
    exit 1
  fi
done

echo "All ${#PLAN[@]} unit(s) commissioned on $WANT_REV."
# A commissioned unit stands at home, and a row renders only when its text
# changes.
echo "They stand at blank until the row's text next changes; to bring it back now:"
echo "  curl -X POST -H 'Content-Type: application/json' -d '{\"name\":\"home-all\",\"target\":{\"row\":\"$(wall_target_row "$BOARD")\"}}' \\"
echo "       'http://$MASTER/api/v2/action'"
