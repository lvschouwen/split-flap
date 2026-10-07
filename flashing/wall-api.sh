# wall-api.sh — the master's /api/v2 for the campaign scripts. Sourced, not run.
# shellcheck shell=bash
#
# Every script here talks to the master only, also for units on a row board,
# and needs nothing but bash, curl and python3: recovery must not depend on
# anything that has to be installed first.
#
# The caller sets MASTER (address, optionally :port) and calls wall_init once.
# A board is named by its id: the master by its own name, a row board by the
# id it paired under (GET /api/v2/wall lists them).

WALL_POLL_S="${WALL_POLL_S:-2}"
WALL_MASTER_ID=""

# What comes from the command line or the capture file goes into URLs, into
# bash arithmetic and into JSON: nothing but these shapes gets that far.
wall_is_board() { [[ "$1" =~ ^[A-Za-z0-9._-]+$ ]]; }
wall_is_number() { [[ "$1" =~ ^[0-9]+$ ]]; }

jqf() {  # json on stdin, python expression over `d` -> value, or nothing
  python3 -c "import sys,json
try: d=json.load(sys.stdin)
except Exception: sys.exit(1)
try: v=($1)
except Exception: sys.exit(1)
print('' if v is None else v)" 2>/dev/null || true
}

# Never fails: a caller under `set -e -o pipefail` would die without a word on
# a master that is away. No answer, or an answer that is not 2xx, is an empty
# body, and every caller judges what it got.
wall_get() {  # path -> body on stdout, nothing when the master did not answer 2xx
  curl -sf --max-time 20 "http://$MASTER$1" || true
}

wall_init() {  # reads the master's name; non-zero when it does not answer
  WALL_MASTER_ID="$(wall_get /api/v2/wall | jqf "d['master']['id']")"
  [[ -n "$WALL_MASTER_ID" ]] || { echo "no /api/v2 answer from the master at $MASTER" >&2; return 1; }
}

wall_boards() {  # every board id of the wall, the master first
  wall_get /api/v2/wall | jqf "'\n'.join([d['master']['id']]+[r['id'] for r in d['rows'] if not r['own']])"
}

# An action's target names the master's own row as "".
wall_target_row() {  # board id -> the row to put in target.row
  if [[ "$1" == "$WALL_MASTER_ID" ]]; then printf ''; else printf '%s' "$1"; fi
}

wall_job_json() {  # name board [unit [argkey argvalue]] -> the action's body
  python3 - "$1" "$(wall_target_row "$2")" "${3:-}" "${4:-}" "${5:-}" <<'PY'
import json, sys
name, row, unit, key, value = sys.argv[1:6]
body = {"name": name, "target": {"row": row}}
if unit:
    body["target"]["unit"] = int(unit)
if key:
    body["args"] = {key: int(value)}
print(json.dumps(body))
PY
}

wall_start() {  # action body -> the job's id; the refusal on stderr, non-zero
  local out code body
  out="$(curl -sS --max-time 20 -X POST -H 'Content-Type: application/json' \
          -d "$1" -w '\n%{http_code}' "http://$MASTER/api/v2/action")" || {
    echo "no answer from the master" >&2; return 1; }
  code="${out##*$'\n'}"
  body="${out%$'\n'*}"
  if [[ "$code" == 202 ]]; then
    printf '%s\n' "$(printf '%s' "$body" | jqf "d['op']")"
    return 0
  fi
  echo "refused ($code): $(printf '%s' "$body" | jqf "d['error']")" >&2
  return 1
}

wall_op() {  # job id -> its document (a running job answers 202)
  curl -s --max-time 20 "http://$MASTER/api/v2/op/$1" || true
}

wall_await() {  # job id, seconds -> how it ended on stdout, non-zero unless done
  local op="$1" deadline=$(( SECONDS + $2 )) body state
  while (( SECONDS < deadline )); do
    body="$(wall_op "$op")"
    state="$(printf '%s' "$body" | jqf "d['state']")"
    case "$state" in
      done)   printf '%s' "$body" | jqf "d.get('result','ok')"; return 0 ;;
      failed) printf '%s' "$body" | jqf "d.get('reason','?')"; return 1 ;;
    esac
    sleep "$WALL_POLL_S"
  done
  echo timeout
  return 1
}

# Start a job and wait for it. Prints how it ended; with WALL_JOB_ID_FILE set,
# the job's id is left there for a caller that wants its data.
wall_job() {  # action body, seconds
  local op
  op="$(wall_start "$1" 2>&1)" || { printf '%s\n' "$op"; return 1; }
  [[ "$op" =~ ^[0-9]+$ ]] || { echo "the master gave no job id"; return 1; }
  if [[ -n "${WALL_JOB_ID_FILE:-}" ]]; then printf '%s' "$op" > "$WALL_JOB_ID_FILE"; fi
  wall_await "$op" "$2"
}

wall_unit() {  # board, address -> the unit's document
  wall_get "/api/v2/unit/$1/$2"
}

# The offsets of a board's units as the master last read them, one
# "address<TAB>offset" a line; a unit whose offset was not read is left out.
wall_offsets() {  # board
  wall_get "/api/v2/board/$1" | jqf "'\n'.join(
    '%d\t%d' % (r[d['units']['fields'].index('address')], r[d['units']['fields'].index('offset')])
    for r in d['units']['rows'] if r[d['units']['fields'].index('offset')] is not None)"
}
