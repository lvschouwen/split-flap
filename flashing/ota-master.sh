#!/usr/bin/env bash
# Upload a master firmware .bin to a running Split-Flap master via the
# /firmware/master endpoint. Computes MD5 locally so the master rejects a
# truncated/corrupted upload (Update.setMD5 on the device side).
#
# Before flashing, probes /settings on the target to show what version is
# already running — catches the "did I point at the right box?" and "is my
# local build actually newer?" mistakes. After flashing, polls the same
# endpoint until the version changes (or a 60 s deadline lapses) so a
# silent-failure OTA is surfaced instead of assumed-success.
#
# Usage: ota-master.sh <firmware.bin> <http://host:port>
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

# Probe the device's /settings JSON for `version` and `mqttHost` so we can
# tell the user what's running. Soft-fails: if the device is unreachable
# or serves a different endpoint set (e.g. recovery SoftAP), we return a
# placeholder and let the flash proceed. This script's job is to flash,
# not gate on introspection — but it should be honest about what it saw.
read_version() {
  local url="$1"
  local json
  if ! json=$(curl -fsS --max-time 5 "$url/settings" 2>/dev/null); then
    echo "unknown (device unreachable at $url/settings)"
    return
  fi
  printf '%s' "$json" | python3 -c '
import json, sys
try:
  d = json.loads(sys.stdin.read())
  v = d.get("version", "?")
  mh = d.get("mqttHost")
  if mh is None:
    mqtt = "no mqttHost field — pre-#50 firmware"
  elif mh == "":
    mqtt = "configured-but-disabled (empty host)"
  else:
    mqtt = f"host=\"{mh}\""
  print(f"{v}  [MQTT: {mqtt}]")
except Exception as e:
  print(f"unknown (parse error: {e})")
' || echo "unknown (python missing?)"
}

RUNNING_BEFORE="$(read_version "$TARGET")"

echo "Target   : $TARGET"
echo "Firmware : $FW"
echo "Size     : $SIZE bytes"
echo "MD5      : $MD5"
echo "Running  : $RUNNING_BEFORE"
echo
read -rp "Proceed? [y/N] " ans
[[ "$ans" == "y" || "$ans" == "Y" ]] || { echo "aborted"; exit 1; }

BODY=$(mktemp)
trap 'rm -f "$BODY"' EXIT

# --progress-bar writes a simple #########  100% bar to stderr while the
# multipart upload streams. stdout remains clean so `--write-out` still
# captures just the HTTP code. curl knows the upload size up front with
# -F so the bar is accurate (not an indeterminate spinner).
HTTP_CODE=$(curl --progress-bar --show-error --max-time 180 \
  --output "$BODY" --write-out '%{http_code}' \
  -F "firmware=@$FW" \
  "$TARGET/firmware/master?md5=$MD5")

echo "HTTP $HTTP_CODE"
cat "$BODY"; echo

[[ "$HTTP_CODE" == "200" ]] || { echo "flash failed" >&2; exit 1; }

# Grab /log IMMEDIATELY — the master has sent us the "flashed, rebooting"
# response but hasn't actually called ESP.restart() yet (there's a 500 ms
# delay in the main loop to flush the TCP socket first). That window is our
# one chance to snapshot the pre-reboot log ring, which holds the Update
# success/failure messages. After ESP.restart() the ring is zeroed.
# Keep logs across runs so we can compare attempts. Filenames include a
# timestamp so successive flashes don't clobber each other. Not deleted on
# exit: they're small and the forensic trail is more valuable than a clean
# /tmp.
TS=$(date +%Y%m%d-%H%M%S)
PRE_REBOOT_LOG="/tmp/ota-pre-reboot-$TS.log"
POST_REBOOT_LOG="/tmp/ota-post-reboot-$TS.log"

if curl -fsS --max-time 2 "$TARGET/log" > "$PRE_REBOOT_LOG" 2>/dev/null; then
  echo "Captured pre-reboot /log snapshot -> $PRE_REBOOT_LOG ($(wc -c < "$PRE_REBOOT_LOG") bytes)"
else
  : > "$PRE_REBOOT_LOG"
  echo "(no pre-reboot /log available — new fw may have already swapped in, or endpoint removed)"
fi

# Confirm the master came back up on the new image. The ESP takes ~15 s to
# reboot + reconnect to WiFi; poll every 3 s for up to 60 s. A string
# inequality against the pre-flash version works even when we can't parse
# the JSON on one side (e.g. mid-reboot): "unknown (...)" just doesn't
# match the old version so we keep polling.
echo "Master is rebooting — polling /settings for a version change..."
deadline=$(( $(date +%s) + 60 ))
while (( $(date +%s) < deadline )); do
  sleep 3
  now="$(read_version "$TARGET")"
  # Accept any successful parse that differs from the pre-flash version.
  if [[ "$now" != unknown* && "$now" != "$RUNNING_BEFORE" ]]; then
    echo "  [ok] now running: $now"
    exit 0
  fi
done
echo "  [warn] version unchanged after 60 s — still reports: $(read_version "$TARGET")" >&2

# Forensic dump: if the flash didn't take, show both logs side-by-side so
# the root cause is visible without a second curl invocation. Pre-reboot
# log captures the Update.end / reboot messages; post-reboot log (current
# /log) captures whatever the old firmware printed after it came back up
# from the failed swap. Compare the two to figure out whether eboot
# rejected the image, the new firmware crashed, or the reboot never fired.
curl -fsS --max-time 5 "$TARGET/log" > "$POST_REBOOT_LOG" 2>/dev/null \
  || : > "$POST_REBOOT_LOG"

echo "" >&2
echo "=== PRE-REBOOT /log (full snapshot) ===" >&2
if [[ -s "$PRE_REBOOT_LOG" ]]; then
  cat "$PRE_REBOOT_LOG" >&2
else
  echo "(empty)" >&2
fi
echo "" >&2
echo "=== CURRENT /log (full, post-reboot) ===" >&2
if [[ -s "$POST_REBOOT_LOG" ]]; then
  cat "$POST_REBOOT_LOG" >&2
else
  echo "(endpoint unreachable or removed — if removed, new fw IS running and /settings is being stale somehow)" >&2
fi
echo "" >&2
echo "  Saved: $PRE_REBOOT_LOG" >&2
echo "  Saved: $POST_REBOOT_LOG" >&2
echo "" >&2

echo "  Possible causes (read the logs above):" >&2
echo "    - 'Update.end failed:' in pre-reboot log -> OTA staging write failed" >&2
echo "    - 'Rebooting Now... Fairwell!' then a fresh boot banner in current /log -> device rebooted but eboot reverted (image rejected at copy time or new fw crashed fast enough to trip recovery)" >&2
echo "    - 'Split Flap Display Starting' in current /log but no 'Rebooting' in pre -> reboot fired after our snapshot window closed (normal)" >&2
echo "    - neither log has upload messages -> the POST never actually reached the handler; check reverse-proxy rules" >&2
exit 3
