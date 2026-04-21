#!/usr/bin/env bash
# Upload a master firmware .bin to a running Split-Flap master via the
# /firmware/master endpoint. Computes MD5 locally so the master rejects a
# truncated/corrupted upload (Update.setMD5 on the device side). Fails
# loud on anything other than HTTP 200.
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

echo "Target   : $TARGET"
echo "Firmware : $FW"
echo "Size     : $SIZE bytes"
echo "MD5      : $MD5"
read -rp "Proceed? [y/N] " ans
[[ "$ans" == "y" || "$ans" == "Y" ]] || { echo "aborted"; exit 1; }

BODY=$(mktemp)
trap 'rm -f "$BODY"' EXIT

HTTP_CODE=$(curl --silent --show-error --max-time 180 \
  --output "$BODY" --write-out '%{http_code}' \
  -F "firmware=@$FW" \
  "$TARGET/firmware/master?md5=$MD5")

echo "HTTP $HTTP_CODE"
cat "$BODY"; echo

[[ "$HTTP_CODE" == "200" ]] || { echo "flash failed" >&2; exit 1; }
echo "Master is rebooting (~15 s). Watch the log panel when it comes back."
