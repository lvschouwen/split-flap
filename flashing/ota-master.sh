#!/usr/bin/env bash
# Upload a master firmware .bin to a running Split-Flap master via the
# /firmware/master endpoint. Computes MD5 locally so the master rejects a
# truncated/corrupted upload (Update.setMD5 on the device side).
#
# Verifies the flash actually took by comparing the device's running
# sketchMd5 before and after, plus checking lastFlashResult (set by the
# boot-time RTC cookie check — see #53). This replaces the old
# "did the version string change?" check, which had a false-negative
# every time both sides tagged the same GIT_REV (e.g. both -dirty).
#
# Usage: ota-master.sh <firmware.bin> <http://host:port>
#
# Requires: curl, md5sum, python3
#
# The device needs to be reachable from wherever this runs — same LAN,
# port-forward, or VPN. No auth/TLS today; don't run this against a
# master exposed on the open internet without protection in front of it.

set -euo pipefail

FW="${1:?usage: $0 <firmware.bin> <http://host:port>}"
TARGET="${2:?usage: $0 <firmware.bin> <http://host:port>}"

[[ -f "$FW" ]] || { echo "not a file: $FW" >&2; exit 2; }
[[ "$TARGET" =~ ^https?://[^/]+$ ]] || {
  echo "target must look like http://host[:port] (no trailing path)" >&2
  exit 2
}

SIZE=$(stat -c%s "$FW")
MD5=$(md5sum "$FW" | awk '{print $1}')

# Fetch a single JSON field from /settings. Soft-fails (returns "?") if the
# device is unreachable or the field is missing — the caller decides what
# "?" means in context. Using python3 (stdlib) so there's no jq dependency.
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

echo "Target   : $TARGET"
echo "Firmware : $FW"
echo "Size     : $SIZE bytes"
echo "MD5      : $MD5"

PRE_VERSION="$(fetch_setting version)"
PRE_SKETCH_MD5="$(fetch_setting sketchMd5)"
echo "Running  : version=$PRE_VERSION  sketchMd5=$PRE_SKETCH_MD5"
echo

read -rp "Proceed? [y/N] " ans
[[ "$ans" == "y" || "$ans" == "Y" ]] || { echo "aborted"; exit 1; }

BODY=$(mktemp)
trap 'rm -f "$BODY"' EXIT

# --progress-bar streams a '######' bar to stderr while the upload flows;
# stdout stays clean so --write-out still captures just the HTTP code.
HTTP_CODE=$(curl --progress-bar --show-error --max-time 180 \
  --output "$BODY" --write-out '%{http_code}' \
  -F "firmware=@$FW" \
  "$TARGET/firmware/master?md5=$MD5")

echo "HTTP $HTTP_CODE"
cat "$BODY"; echo

[[ "$HTTP_CODE" == "200" ]] || { echo "flash failed (HTTP $HTTP_CODE)" >&2; exit 1; }

# Device takes ~15 s to reboot + reconnect to WiFi. Poll up to 90 s for
# /settings to come back, then decode the verdict. Waiting past 5-min
# cache boundaries is pointless; if it hasn't come back by 90 s something
# below the sketch layer is wrong.
echo "Master is rebooting — polling /settings for verdict..."
deadline=$(( $(date +%s) + 90 ))
POST_VERSION="?"
POST_SKETCH_MD5="?"
LAST_FLASH_RESULT="?"
while (( $(date +%s) < deadline )); do
  sleep 3
  POST_SKETCH_MD5="$(fetch_setting sketchMd5)"
  if [[ "$POST_SKETCH_MD5" != "?" && -n "$POST_SKETCH_MD5" ]]; then
    POST_VERSION="$(fetch_setting version)"
    LAST_FLASH_RESULT="$(fetch_setting lastFlashResult)"
    break
  fi
done

echo
echo "=== Post-reboot verdict ==="
echo "  version         : $PRE_VERSION -> $POST_VERSION"
echo "  sketchMd5       : $PRE_SKETCH_MD5 -> $POST_SKETCH_MD5"
echo "  lastFlashResult : $LAST_FLASH_RESULT"
echo

# Decision matrix — see #53 piece 3.
#   lastFlashResult="ok"       AND sketchMd5 changed   -> success
#   lastFlashResult="reverted" AND sketchMd5 unchanged  -> eboot silent revert
#   lastFlashResult=""         AND sketchMd5 unchanged  -> upload never reached handler
#                                                         (or device didn't reboot)
#   anything else                                       -> inconsistent; report raw
if [[ "$LAST_FLASH_RESULT" == "ok" && "$POST_SKETCH_MD5" != "$PRE_SKETCH_MD5" && "$POST_SKETCH_MD5" != "?" ]]; then
  echo "[ok] SUCCESS — new firmware is running."
  exit 0
fi

if [[ "$LAST_FLASH_RESULT" == "reverted" ]]; then
  cat >&2 <<MSG
[warn] EBOOT SILENT REVERT — the staged image was rejected at copy time.
       Most likely causes: ESP-01 power sag during flash erase/write, flash
       wear at the staging sector, or a brownout during restart. This failure
       mode needs physical access (USB-serial at 74880 baud for eboot's own
       log) or a hardware fix (power supply / decoupling).
MSG
  exit 3
fi

if [[ -z "$LAST_FLASH_RESULT" && "$POST_SKETCH_MD5" == "$PRE_SKETCH_MD5" && "$POST_SKETCH_MD5" != "?" ]]; then
  cat >&2 <<MSG
[warn] UPLOAD DID NOT REACH HANDLER — sketchMd5 unchanged and no RTC cookie
       was recorded. The POST returned 200 but the /firmware/master success
       path never ran. Check reverse-proxy / port-forward rules, and compare
       the request path to what the device logs.
MSG
  exit 3
fi

cat >&2 <<MSG
[warn] INCONSISTENT POST-FLASH STATE — couldn't decide success vs failure.
       Raw values are above; inspect /log on the device to figure out what
       happened.
MSG
exit 3
