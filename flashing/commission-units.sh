#!/usr/bin/env bash
# Commission units onto the #407 day-0 firmware, one at a time, stopping at
# the first thing that looks wrong.
#
# The #407 image erases and re-initialises every unit's EEPROM under a brand
# new wire contract, and has never run on hardware. Handing that to a 21-unit
# sweep means finding out on unit 21. This drives the campaign per unit and
# refuses to continue past a failure.
#
# Requires #412 on the master:
#   reflashOnBoot=false     so the boot auto-install does not converge the
#                           fleet before this script gets a vote
#   /reflash-units?address= so one unit can be flashed at a time
#
# Per unit: flash -> verify identity -> restore offset -> home -> exercise ->
# baseline self-test -> exercise -> compare self-test -> verdict.
#
# WHY THE FLASH COMES FIRST, and is also the address check. Every unit on the
# wall runs an EEPROM-provisioned address (ae:1); the day-0 init clears that,
# so each one falls back to its DIP switches. A targeted reflash proves the
# two agree WITHOUT risking anything: the job sends ENTER_BOOTLOADER to the
# address, rescans, and only flashes what it finds in twiboot AT that address.
# If the DIP disagrees, nothing is in twiboot there, the job reports 0 planned,
# and not a byte is written. (Watching for twiboot directly is not possible
# from outside: /unit/reboot arms the v1 #88 probe inhibit, and twiboot times
# out in ~1 s anyway.)
#
# WHY THE BASELINE SELF-TEST IS NOT FIRST. After the erase
# selfTestFirstHallWindow is 0, so the first PASSING self-test becomes the
# reference that unit compares itself against for the rest of its life
# (unitEeRecordSelfTest never overwrites the first). Taking it on a cold drum
# bakes in a bad reference and makes later degradation read as improvement.
# Exercise first, then baseline.
#
# Usage:
#   commission-units.sh --host <ip>            all captured units on that row
#   commission-units.sh --host <ip> --only 3   one unit
#   commission-units.sh --host <ip> --from 7   resume mid-campaign
#   commission-units.sh --host <ip> --dry-run  print the plan, touch nothing
#   commission-units.sh --host <ip> --skip 15  exclude a known-bad unit
#
# a15 is hall-dead: it will fail homing and both self-tests. Either repair it
# first or --skip it, so its failures are not what proves the script works.
#
# Exit is non-zero the moment a unit fails. Nothing after it is touched.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JSON="$HERE/unit-offsets.json"
REV_FILE="$HERE/../firmware/v2/Master/data/unit-firmware.rev"

HOST=""
ONLY=""
FROM=""
SKIP=""
DRY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --host)    HOST="${2:?--host needs an address}"; shift 2 ;;
    --only)    ONLY="${2:?--only needs an address}"; shift 2 ;;
    --from)    FROM="${2:?--from needs an address}"; shift 2 ;;
    --skip)    SKIP="${2:?--skip needs a comma-separated list}"; shift 2 ;;
    --dry-run) DRY=1; shift ;;
    -h|--help) sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$HOST" ]]      || { echo "--host is required" >&2; exit 2; }
[[ -f "$JSON" ]]      || { echo "missing $JSON" >&2; exit 1; }
[[ -f "$REV_FILE" ]]  || { echo "missing $REV_FILE — build + stage first" >&2; exit 1; }

WANT_REV="$(tr -d ' \t\r\n' < "$REV_FILE")"
[[ -n "$WANT_REV" ]] || { echo "empty rev in $REV_FILE" >&2; exit 1; }

# --- exercise geometry -------------------------------------------------------
# The drum is 2038 steps; a jog is capped at ±127 (one signed wire byte). 17
# jogs of 120 steps is 2040 — one revolution to within two steps, and the
# following home re-syncs the belief anyway. Two rounds is 34.
JOG_STEPS=120
JOGS_PER_ROUND=17
ROUNDS=2

# Motor cooldown between motion phases. The self-test alone is ~2 revolutions
# of continuous stepping; back to back with two exercise phases that is ~6 on a
# 28BYJ-48 whose overheat gate is only 2 s and was never sized for this.
COOL_S=20

# --- helpers -----------------------------------------------------------------

api() {  # method path -> body on stdout, non-zero on transport failure
  local method="$1" path="$2"
  curl -sf --max-time 20 -X "$method" "http://$HOST$path"
}

jqf() {  # json-on-stdin, python expression over `d` -> value or empty
  python3 -c "import sys,json
try: d=json.load(sys.stdin)
except Exception: sys.exit(1)
try: print($1)
except Exception: sys.exit(1)" 2>/dev/null || true
}

unit_field() {  # addr field -> value, or empty
  api GET "/units/health" | jqf "next(u['$2'] for u in d['units'] if u['a']==$1)"
}

# Wait out a {"seq":N} maintenance op. Prints ok / the failure reason.
await_op() {  # seq timeout_s -> echoes final state, non-zero unless ok
  local seq="$1" deadline=$(( SECONDS + $2 )) state
  while (( SECONDS < deadline )); do
    state="$(api GET "/unit/op-result?seq=$seq" | jqf "d['state']")"
    case "$state" in
      ok)      echo ok; return 0 ;;
      failed)  api GET "/unit/op-result?seq=$seq" | jqf "d.get('reason','?')+'/'+d.get('detail','')"; return 1 ;;
      expired) echo expired; return 1 ;;
    esac
    sleep 2
  done
  echo timeout
  return 1
}

post_seq() {  # path -> seq, or empty
  api POST "$1" | jqf "d['seq']"
}

# Prints only. Deliberately returns 0: used as `cmd || { fail "..."; return 1; }`,
# a non-zero fail() would make the group's status depend on bash's errexit
# exemption rules for || lists, which is exactly the kind of subtlety this
# script is supposed to be free of.
fail() { printf '  FAIL: %s\n' "$*"; }

# --- plan --------------------------------------------------------------------

ADDRS="$(python3 - "$JSON" "$HOST" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
for row in data["rows"]:
    if row["host"] != sys.argv[2]:
        continue
    for addr in sorted(row["offsets"], key=int):
        print(addr)
PY
)"
# Materialised BEFORE the loop on purpose: `done < <(...)` puts the generator in
# a process substitution whose failure never trips errexit, so a typo'd --host
# runs zero units and exits 0 — indistinguishable from "everything passed".
# restore-unit-offsets.sh was caught by exactly this.
[[ -n "$ADDRS" ]] || { echo "no units for host $HOST in $JSON" >&2; exit 1; }

PLAN=()
for a in $ADDRS; do
  [[ -n "$ONLY" && "$a" != "$ONLY" ]] && continue
  [[ -n "$FROM" && "$a" -lt "$FROM" ]] && continue
  [[ ",$SKIP," == *",$a,"* ]] && { echo "skipping a$a (--skip)"; continue; }
  PLAN+=("$a")
done
[[ ${#PLAN[@]} -gt 0 ]] || { echo "plan is empty after filters" >&2; exit 1; }

echo "host        $HOST"
echo "target rev  $WANT_REV"
echo "units       ${PLAN[*]}"
echo "exercise    $ROUNDS round(s) = $((ROUNDS * JOGS_PER_ROUND)) jogs of $JOG_STEPS steps"
echo

if (( DRY )); then
  echo "dry run — nothing was touched."
  exit 0
fi

# The brake must be on, or the master will converge the fleet behind our back
# on its next boot and this script's whole premise is gone.
BRAKE="$(api GET "/settings" | jqf "d['reflashOnBoot']")"
if [[ "$BRAKE" != "False" && "$BRAKE" != "false" ]]; then
  echo "REFUSING: reflashOnBoot is '${BRAKE:-unreadable}', expected false." >&2
  # /settings is GET-only; the settings POST is the form route at /.
  echo "  curl -X POST -d reflashOnBoot=false 'http://$HOST/'" >&2
  exit 1
fi

# --- per unit ----------------------------------------------------------------

commission() {  # addr -> 0 pass, 1 fail
  local a="$1" seq why before_rev before_hf want_off
  printf '=== a%s ===\n' "$a"

  before_rev="$(unit_field "$a" rev)"
  before_hf="$(unit_field "$a" hf)"
  printf '  before      rev=%s st=%s hf=%s\n' \
    "${before_rev:-?}" "$(unit_field "$a" st)" "${before_hf:-0}"

  # 1. flash — also the DIP/EEPROM address proof (see the header)
  if [[ "$before_rev" == "$WANT_REV" ]]; then
    printf '  flash       already on %s, skipping\n' "$WANT_REV"
  else
    seq="$(post_seq "/reflash-units?address=$a")"
    [[ -n "$seq" ]] || { fail "reflash POST gave no seq"; return 1; }
    printf '  flash       seq=%s ' "$seq"
    await_op "$seq" 180 || { fail "reflash did not complete"; return 1; }
    # One read, both fields: two GETs could straddle a state change, and an
    # absent reflash object must be indistinguishable from neither.
    local health planned flashed
    health="$(api GET "/units/health" || true)"
    planned="$(printf '%s' "$health" | jqf "d['reflash']['total']")"
    flashed="$(printf '%s' "$health" | jqf "d['reflash']['done']")"
    # Demand a POSITIVE INTEGER. Comparing against the literal "0" alone let an
    # empty value through — and empty is exactly what an unreadable health
    # response or an omitted reflash object produces (WebMaintenance splices
    # that object only when it fits the JSON cap). That would silently skip the
    # one check this step exists for.
    if ! [[ "$planned" =~ ^[0-9]+$ ]]; then
      fail "could not read reflash.total from /units/health (got '${planned}')
        — refusing to assume the flash planned anything."
      return 1
    fi
    if [[ "$planned" -eq 0 ]]; then
      fail "nothing was planned for a$a — its twiboot did not answer at this
        address, which means the DIP switches disagree with the burned
        address. STOP: do not erase anything else until that is resolved."
      return 1
    fi
    printf '  flashed     %s/%s\n' "${flashed:-?}" "${planned}"
  fi

  # 2. identity
  local rev pv pmm st
  rev="$(unit_field "$a" rev)"; pv="$(unit_field "$a" pv)"
  pmm="$(unit_field "$a" pmm)"; st="$(unit_field "$a" st)"
  [[ "$rev" == "$WANT_REV" ]] || { fail "rev is '$rev', wanted '$WANT_REV'"; return 1; }
  [[ "$st" == "1" ]]         || { fail "state is '$st', wanted 1 (sketch)"; return 1; }
  [[ -z "$pmm" || "$pmm" == "0" ]] || { fail "protocol mismatch (pmm=$pmm, pv=$pv)"; return 1; }
  printf '  identity    rev=%s pv=%s ok\n' "$rev" "${pv:-<none>}"

  # 3. offset — the one thing the erase destroys that is not reconstructible
  want_off="$(python3 - "$JSON" "$HOST" "$a" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
for row in data["rows"]:
    if row["host"] == sys.argv[2] and sys.argv[3] in row["offsets"]:
        print(row["offsets"][sys.argv[3]])
PY
)"
  [[ -n "$want_off" ]] || { fail "no captured offset for a$a"; return 1; }
  seq="$(post_seq "/unit/offset?address=$a&value=$want_off")"
  [[ -n "$seq" ]] || { fail "offset POST gave no seq"; return 1; }
  why="$(await_op "$seq" 30)" || { fail "offset write did not confirm: $why"; return 1; }
  local back
  back="$(api GET "/unit/offset?address=$a" | jqf "d['offset']")"
  [[ "$back" == "$want_off" ]] || { fail "offset read back '$back', wanted '$want_off'"; return 1; }
  printf '  offset      %s restored\n' "$want_off"

  # 3b. odometer — the day-0 init deliberately leaves the ring unwritten on the
  # theory that erased slots read 0xFFFFFFFF and recover as 0. That is true of a
  # factory-fresh Nano and false of every unit on this wall: the erase rewrites
  # only bytes 0..22, and the ring grew 16 -> 128 slots, so the new scan reads
  # stale bytes the old ring never wrote. a1 booted claiming 0x3C3C3C3C
  # revolutions (#417). Zero it BEFORE the exercise so the odo readings below
  # measure something.
  seq="$(post_seq "/unit/reset-odometer?address=$a")"
  [[ -n "$seq" ]] || { fail "odometer reset POST gave no seq"; return 1; }
  why="$(await_op "$seq" 30)" || { fail "odometer reset did not confirm: $why"; return 1; }
  # Two lags stack here, so POLL instead of sleeping a guessed constant: 128
  # slots is ~2 s of EEPROM writes and the op ACKs before they land, and
  # /units/health serves the master's CACHED facts — a read issued straight
  # after ?refresh=1 outruns the re-poll and returns the pre-reset value.
  local odo_zeroed="" odo_deadline=$(( SECONDS + 40 ))
  while (( SECONDS < odo_deadline )); do
    api GET "/units/health?refresh=1" >/dev/null || true
    sleep 3
    odo_zeroed="$(unit_field "$a" odo)"
    [[ "$odo_zeroed" == "0" ]] && break
  done
  [[ "$odo_zeroed" == "0" ]] || {
    fail "odometer reads '${odo_zeroed:-<unreadable>}' after reset, wanted 0"
    return 1
  }
  printf '  odometer    zeroed\n'

  # 4. home
  seq="$(post_seq "/unit/home?address=$a")"
  [[ -n "$seq" ]] || { fail "home POST gave no seq"; return 1; }
  why="$(await_op "$seq" 60)" || { fail "home did not complete: $why"; return 1; }
  local hs2 hf
  hs2="$(unit_field "$a" hs2)"; hf="$(unit_field "$a" hf)"
  [[ "$hs2" == "2" ]] || { fail "not homed after home (hs2=$hs2)"; return 1; }
  if [[ "${hf:-0}" != "${before_hf:-0}" ]]; then
    fail "lifetime failed-homing count rose ${before_hf:-0} -> ${hf:-0}"
    return 1
  fi
  printf '  home        ok (hf=%s)\n' "${hf:-0}"

  # 5/6/7/8. exercise, baseline, exercise, compare
  local base_win base_spr win spr out
  exercise "$a" || return 1
  sleep "$COOL_S"
  out="$(self_test "$a")" || return 1
  read -r base_win base_spr <<< "$out"
  printf '  self-test   baseline hall_window=%s steps_per_rev=%s\n' "$base_win" "$base_spr"
  sleep "$COOL_S"
  exercise "$a" || return 1
  sleep "$COOL_S"
  out="$(self_test "$a")" || return 1
  read -r win spr <<< "$out"
  printf '  self-test   after    hall_window=%s steps_per_rev=%s\n' "$win" "$spr"

  # A drum that measures differently after a dozen revolutions is telling you
  # something before it becomes a15. 10% on the window, 1% on steps/rev.
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
  local a="$1" n=$(( ROUNDS * JOGS_PER_ROUND )) i seq why
  local odo_before odo_after
  odo_before="$(unit_field "$a" odo)"
  printf '  exercise    %s jogs' "$n"
  for (( i = 0; i < n; i++ )); do
    seq="$(post_seq "/unit/jog?address=$a&steps=$JOG_STEPS")"
    [[ -n "$seq" ]] || { echo; fail "jog $i POST gave no seq"; return 1; }
    why="$(await_op "$seq" 20)" || { echo; fail "jog $i did not complete: $why"; return 1; }
    printf '.'
  done
  odo_after="$(unit_field "$a" odo)"
  printf ' odo %s -> %s\n' "${odo_before:-?}" "${odo_after:-?}"
  # The odometer counts COMMANDED steps, so this proves the jogs landed — not
  # that the drum physically turned. Physical truth is the self-test compare.
  if [[ -n "$odo_before" && -n "$odo_after" && "$odo_after" -lt "$odo_before" ]]; then
    fail "odometer went backwards"
    return 1
  fi
}

self_test() {  # addr -> "hall_window steps_per_rev" on stdout
  local a="$1" seq body state
  seq="$(post_seq "/unit/self-test?address=$a")"
  [[ -n "$seq" ]] || { fail "self-test POST gave no seq" >&2; return 1; }
  local deadline=$(( SECONDS + 120 ))
  while (( SECONDS < deadline )); do
    body="$(api GET "/unit/self-test-result?seq=$seq")"
    state="$(printf '%s' "$body" | jqf "d['state']")"
    case "$state" in
      ok)
        printf '%s %s\n' \
          "$(printf '%s' "$body" | jqf "d['hall_window']")" \
          "$(printf '%s' "$body" | jqf "d['steps_per_rev']")"
        return 0 ;;
      failed)
        fail "self-test failed: $(printf '%s' "$body" | jqf "d.get('unit_reason','?')")" >&2
        return 1 ;;
      expired)
        fail "self-test result expired" >&2; return 1 ;;
    esac
    sleep 3
  done
  fail "self-test timed out" >&2
  return 1
}

for a in "${PLAN[@]}"; do
  if ! commission "$a"; then
    echo
    echo "STOPPED at a$a. ${#PLAN[@]} unit(s) were planned; everything after"
    echo "a$a is untouched. Fix the cause before continuing:"
    echo "  commission-units.sh --host $HOST --from $a"
    exit 1
  fi
done

echo "All ${#PLAN[@]} unit(s) commissioned on $WANT_REV."
