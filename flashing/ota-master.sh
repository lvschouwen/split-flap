#!/usr/bin/env bash
# Upload a master firmware .bin to a running Split-Flap master via the
# /firmware/master endpoint. Computes MD5 locally so the master rejects a
# truncated/corrupted upload (Update.setMD5 on the device side), and passes
# ?v=<gitrev> so the device's intendedVersion EEPROM slot is populated
# (enables the post-boot otaReverted flag — #52/#53).
#
# Verifies the flash by comparing running sketchMd5 before and after and
# checking lastFlashResult (set by the boot-time RTC cookie check — see #53).
#
# Silent reverts on ESP-01 are genuinely transient (power sag during flash
# erase/write — see CLAUDE.md's OTA section). This script retries the full
# upload + reboot cycle up to MAX_ATTEMPTS times when the verdict is
# "reverted". Other failure modes (no-handler, inconsistent, upload-failed)
# are NOT retried — those are config/network/logic bugs that won't resolve
# by retrying.
#
# Usage: ota-master.sh [-n <attempts>] [-v <gitrev>] <firmware.bin> <http://host:port>
#
#   -n <attempts>   max total attempts (default 4)
#   -v <gitrev>     override the version string sent as ?v=. If omitted,
#                   derived from a firmware-<rev>.bin stamped filename
#                   (produced by build_assets.py alongside firmware.bin).
#                   If neither is available, ?v= is omitted entirely.
#
# Requires: curl, md5sum, python3
#
# The device needs to be reachable from wherever this runs — same LAN,
# port-forward, or VPN. No auth/TLS today; don't run this against a
# master exposed on the open internet without protection in front of it.

set -euo pipefail

MAX_ATTEMPTS=4
OVERRIDE_VERSION=""

usage() {
  echo "usage: $0 [-n <attempts>] [-v <gitrev>] <firmware.bin> <http://host:port>" >&2
  exit 2
}

while getopts "n:v:h" opt; do
  case "$opt" in
    n) MAX_ATTEMPTS="$OPTARG" ;;
    v) OVERRIDE_VERSION="$OPTARG" ;;
    h|*) usage ;;
  esac
done
shift $((OPTIND - 1))

FW="${1:-}"
TARGET="${2:-}"
[[ -n "$FW" && -n "$TARGET" ]] || usage

[[ -f "$FW" ]] || { echo "not a file: $FW" >&2; exit 2; }
[[ "$TARGET" =~ ^https?://[^/]+$ ]] || {
  echo "target must look like http://host[:port] (no trailing path)" >&2
  exit 2
}
[[ "$MAX_ATTEMPTS" =~ ^[1-9][0-9]*$ ]] || {
  echo "-n must be a positive integer, got: $MAX_ATTEMPTS" >&2
  exit 2
}

SIZE=$(stat -c%s "$FW")
MD5=$(md5sum "$FW" | awk '{print $1}')

# Derive GIT_REV from the stamped filename (build_assets.py writes
# firmware-<rev>.bin alongside firmware.bin) unless -v overrides.
GIT_REV=""
if [[ -n "$OVERRIDE_VERSION" ]]; then
  GIT_REV="$OVERRIDE_VERSION"
else
  base=$(basename "$FW")
  if [[ "$base" =~ ^firmware-([a-f0-9]+(-dirty)?)\.bin$ ]]; then
    GIT_REV="${BASH_REMATCH[1]}"
  fi
fi

# Fetch a single JSON field from /settings. Soft-fails (returns "?") if the
# device is unreachable or the field is missing — caller decides what "?"
# means in context. Using python3 (stdlib) so there's no jq dependency.
fetch_setting() {
  local field="$1"
  local json
  if ! json=$(curl -fsS --max-time 5 "$TARGET/settings" 2>/dev/null); then
    echo "?"
    return
  fi
  printf '%s' "$json" | python3 -c "
import json, sys
try:
  d = json.loads(sys.stdin.read())
  v = d.get('$field')
  print('' if v is None else v)
except Exception:
  print('?')
" 2>/dev/null || echo "?"
}

BODY=$(mktemp)
trap 'rm -f "$BODY"' EXIT

# Run one attempt. Returns a verdict string via the global LAST_VERDICT:
#   success       new bits are running
#   reverted      eboot silent revert (retryable)
#   no-handler    HTTP 200 but sketch unchanged and no RTC cookie recorded
#                 (not retryable — config/network/proxy problem)
#   upload-failed curl/HTTP-level failure (not retryable)
#   unreachable   device didn't respond to /settings (not retryable)
#   inconsistent  unknown post-flash state (not retryable)
run_attempt() {
  local attempt_num="$1"

  local pre_version pre_md5
  pre_version="$(fetch_setting version)"
  pre_md5="$(fetch_setting sketchMd5)"
  if [[ "$pre_md5" == "?" ]]; then
    echo "[attempt $attempt_num] device unreachable at $TARGET" >&2
    LAST_VERDICT="unreachable"
    return
  fi
  echo "[attempt $attempt_num] running: version=$pre_version sketchMd5=$pre_md5"

  local url="$TARGET/firmware/master?md5=$MD5"
  if [[ -n "$GIT_REV" ]]; then
    url="$url&v=$GIT_REV"
  fi

  local http_code
  http_code=$(curl --progress-bar --show-error --max-time 180 \
    --output "$BODY" --write-out '%{http_code}' \
    -F "firmware=@$FW" "$url" || echo "000")

  echo "HTTP $http_code"
  cat "$BODY"; echo

  if [[ "$http_code" != "200" ]]; then
    LAST_VERDICT="upload-failed"
    return
  fi

  # Device takes ~15 s to reboot + reconnect. Poll up to 90 s for /settings
  # to come back; waiting past that is pointless.
  echo "[attempt $attempt_num] master rebooting — polling /settings..."
  local deadline=$(( $(date +%s) + 90 ))
  local post_md5="?" post_version="?" last_result="?" intended=""
  while (( $(date +%s) < deadline )); do
    sleep 3
    post_md5="$(fetch_setting sketchMd5)"
    if [[ "$post_md5" != "?" && -n "$post_md5" ]]; then
      post_version="$(fetch_setting version)"
      last_result="$(fetch_setting lastFlashResult)"
      intended="$(fetch_setting intendedVersion)"
      break
    fi
  done

  echo "  version         : $pre_version -> $post_version"
  echo "  sketchMd5       : $pre_md5 -> $post_md5"
  echo "  lastFlashResult : $last_result"
  echo "  intendedVersion : $intended"

  # Decision matrix — see #53 piece 3 + #60.
  if [[ "$last_result" == "ok" && "$post_md5" != "$pre_md5" && "$post_md5" != "?" ]]; then
    LAST_VERDICT="success"
  elif [[ "$last_result" == "reverted" ]]; then
    LAST_VERDICT="reverted"
  elif [[ -z "$last_result" && "$post_md5" == "$pre_md5" && "$post_md5" != "?" ]]; then
    LAST_VERDICT="no-handler"
  else
    LAST_VERDICT="inconsistent"
  fi
}

echo "Target        : $TARGET"
echo "Firmware      : $FW"
echo "Size          : $SIZE bytes"
echo "MD5           : $MD5"
echo "Git rev       : ${GIT_REV:-<none>}"
echo "Max attempts  : $MAX_ATTEMPTS"
echo

INITIAL_VERSION="$(fetch_setting version)"
INITIAL_MD5="$(fetch_setting sketchMd5)"
echo "Running       : version=$INITIAL_VERSION  sketchMd5=$INITIAL_MD5"
echo

read -rp "Proceed? [y/N] " ans
[[ "$ans" == "y" || "$ans" == "Y" ]] || { echo "aborted"; exit 1; }

LAST_VERDICT=""
for (( attempt=1; attempt<=MAX_ATTEMPTS; attempt++ )); do
  echo
  echo "=== OTA attempt $attempt of $MAX_ATTEMPTS ==="
  run_attempt "$attempt"

  case "$LAST_VERDICT" in
    success)
      echo
      echo "[ok] SUCCESS — new firmware is running (attempt $attempt of $MAX_ATTEMPTS)."
      exit 0
      ;;
    reverted)
      if (( attempt < MAX_ATTEMPTS )); then
        echo
        echo "[warn] EBOOT SILENT REVERT on attempt $attempt — retrying in 5 s..."
        sleep 5
      fi
      ;;
    no-handler)
      cat >&2 <<MSG

[warn] UPLOAD DID NOT REACH HANDLER — sketchMd5 unchanged and no RTC cookie
       was recorded. POST returned 200 but /firmware/master's success path
       never ran. NOT retrying — this is a reverse-proxy / port-forward /
       routing issue, not a transient hardware sag. Compare the device's
       /log to what the request actually hit.
MSG
      exit 3
      ;;
    upload-failed)
      echo "[warn] upload-failed on attempt $attempt — not retrying" >&2
      exit 3
      ;;
    unreachable)
      echo "[warn] device unreachable on attempt $attempt — not retrying" >&2
      exit 3
      ;;
    inconsistent)
      cat >&2 <<MSG

[warn] INCONSISTENT POST-FLASH STATE — could not classify success vs failure.
       Raw values are above. Inspect /log on the device. NOT retrying —
       this is not a signal the hardware will resolve by trying again.
MSG
      exit 3
      ;;
    *)
      echo "[warn] unknown verdict: $LAST_VERDICT" >&2
      exit 3
      ;;
  esac
done

# Retry budget exhausted; last verdict was "reverted".
cat >&2 <<MSG

[fail] EBOOT SILENT REVERT — $MAX_ATTEMPTS attempts, all reverted.
       This looks non-transient. Likely causes:
         - ESP-01 power supply margin too tight (add 1000 uF electrolytic
           + 100 nF ceramic on VCC near the module)
         - Flash wear at the OTA staging sector
         - Upstream 3.3 V rail shared with something noisy (stepper drivers,
           relays) — move the ESP to its own AMS1117-3.3 (>=1 A)
       Needs physical access (USB-serial @ 74880 baud for eboot's own log)
       or a hardware fix; more firmware changes won't help from here.
MSG
exit 3
