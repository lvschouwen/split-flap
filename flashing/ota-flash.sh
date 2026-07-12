#!/usr/bin/env bash
# Fetch the latest staged v2 firmware from the build server and OTA it to a
# running v2 master (ESP32-S3) — the v2 counterpart of ota-master.sh.
#
# "Latest" = the newest firmware-<rev>.bin (or rescue-<rev>.bin with -r) in
# the build server's staging directory, found over ssh and copied down with
# scp. MD5 is computed locally and passed as ?md5= so the device rejects a
# truncated/corrupted upload.
#
# Verdict (master flash): the device reboots itself after a 200 and the new
# image boots PENDING_VERIFY (native A/B rollback — deliberately no v1
# RTC-cookie machinery, see CLAUDE.md). The script polls /settings until the
# device is back and checks that `version` equals the rev stamped in the
# filename and `otaReverted` is false. A same-rev reflash cannot be told
# apart from a rollback to the same rev by /settings alone — the script
# warns instead of claiming success in that case.
#
# Verdict (rescue install, -r): the rescue image goes into the factory slot
# via raw partition writes and the device does NOT reboot — the HTTP
# response is the whole verdict (MD5 is checked device-side before the
# final sector is committed).
#
# Usage: ota-flash.sh [-s user@host] [-d dir] [-r|-a] [-l file.bin] [-y] <device>
#
#   <device>        IP, hostname, or http://host[:port] of the running master
#   -s user@host    build server to fetch from (default: $SPLITFLAP_BIN_HOST)
#   -d dir          staging directory on the server (default:
#                   $SPLITFLAP_BIN_DIR, else bench-bins in the remote home)
#   -r              install the latest rescue image instead of the master
#                   firmware (POST /firmware/rescue)
#   -a              flash the latest master firmware, then — once the new
#                   image is confirmed running — install the latest rescue
#   -l file.bin     skip the download and upload this local file (its
#                   firmware-<rev>/rescue-<rev> name still supplies the
#                   expected rev; not valid with -a)
#   -y              don't ask for confirmation
#
# Requires: curl, md5sum, python3, and (unless -l) ssh + scp access to the
# build server.
#
# The device needs to be reachable from wherever this runs. No auth/TLS
# today; don't run this against a master exposed on the open internet.

set -euo pipefail

SERVER="${SPLITFLAP_BIN_HOST:-}"
REMOTE_DIR="${SPLITFLAP_BIN_DIR:-bench-bins}"
MODE="master"
LOCAL_FILE=""
ASSUME_YES=0

usage() {
  echo "usage: $0 [-s user@host] [-d dir] [-r|-a] [-l file.bin] [-y] <device>" >&2
  exit 2
}

while getopts "s:d:ral:yh" opt; do
  case "$opt" in
    s) SERVER="$OPTARG" ;;
    d) REMOTE_DIR="$OPTARG" ;;
    r) MODE="rescue" ;;
    a) MODE="all" ;;
    l) LOCAL_FILE="$OPTARG" ;;
    y) ASSUME_YES=1 ;;
    h|*) usage ;;
  esac
done
shift $((OPTIND - 1))

DEVICE="${1:-}"
[[ -n "$DEVICE" ]] || usage

if [[ -n "$LOCAL_FILE" && "$MODE" == "all" ]]; then
  echo "-l flashes a single file; it cannot combine with -a" >&2
  exit 2
fi
if [[ -z "$LOCAL_FILE" && -z "$SERVER" ]]; then
  echo "no build server: pass -s user@host or set SPLITFLAP_BIN_HOST" >&2
  exit 2
fi
# A leading "-" would be parsed by ssh/scp as an option, not a hostname.
if [[ "$SERVER" == -* ]]; then
  echo "build server must be [user@]host, got: $SERVER" >&2
  exit 2
fi

# Accept a bare IP/hostname or a full http://host[:port].
if [[ "$DEVICE" =~ ^https?:// ]]; then
  TARGET="$DEVICE"
else
  TARGET="http://$DEVICE"
fi
[[ "$TARGET" =~ ^https?://[^/]+$ ]] || {
  echo "device must be an IP/hostname or http://host[:port] (no trailing path)" >&2
  exit 2
}

DOWNLOAD_DIR=""
cleanup() { [[ -n "$DOWNLOAD_DIR" ]] && rm -rf "$DOWNLOAD_DIR"; rm -f "${BODY:-}"; }
trap cleanup EXIT
BODY=$(mktemp)

# Fail fast instead of hanging on an unreachable server or an interactive
# auth prompt (this script may run unattended with -y).
SSH_OPTS=(-o ConnectTimeout=10 -o BatchMode=yes)

# Newest matching bin in the server's staging dir. ls -t (not name order):
# rev stamps are hashes, only mtime says which build is latest. .factory.bin
# is a merged USB-at-0x0 image, never a valid OTA payload — filter it out.
# REMOTE_DIR/prefix travel as positional args to a fixed remote script, never
# interpolated into the remote command line (shell-injection surface).
find_latest() {
  local prefix="$1"
  ssh "${SSH_OPTS[@]}" -- "$SERVER" bash -s -- "$REMOTE_DIR" "$prefix" <<'REMOTE'
dir="$1"; prefix="$2"
ls -t -- "$dir/$prefix"-*.bin 2>/dev/null | grep -v '\.factory\.bin$' | head -1
exit 0
REMOTE
}

# Download the newest <prefix>-<rev>.bin; prints the local path.
fetch_latest() {
  local prefix="$1"
  local remote
  if ! remote=$(find_latest "$prefix"); then
    echo "cannot reach build server $SERVER (ssh failed — see above)" >&2
    return 3
  fi
  if [[ -z "$remote" ]]; then
    echo "no $prefix-*.bin found in $SERVER:$REMOTE_DIR" >&2
    return 3
  fi
  [[ -n "$DOWNLOAD_DIR" ]] || DOWNLOAD_DIR=$(mktemp -d)
  scp "${SSH_OPTS[@]}" -q -- "$SERVER:$remote" "$DOWNLOAD_DIR/" >&2
  echo "$DOWNLOAD_DIR/$(basename "$remote")"
}

# firmware-<rev>.bin / rescue-<rev>.bin → <rev> (keeps any -dirty suffix so a
# dirty build is called out in the verdict rather than silently matching).
rev_from_name() {
  local base
  base=$(basename "$1")
  if [[ "$base" =~ ^(firmware|rescue)-([a-f0-9]+(-dirty)?)([.-].*)?\.bin$ ]]; then
    echo "${BASH_REMATCH[2]}"
  fi
}

# Fetch a single JSON field from /settings. Soft-fails (returns "?") if the
# device is unreachable or the field is missing — caller decides what "?"
# means in context. python3 (stdlib) so there's no jq dependency.
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

confirm() {
  (( ASSUME_YES )) && return 0
  local ans
  read -rp "Proceed? [y/N] " ans
  [[ "$ans" == "y" || "$ans" == "Y" ]] || { echo "aborted"; exit 1; }
}

# --- master firmware -------------------------------------------------------

flash_master() {
  local fw="$1"
  local size md5 rev pre_version
  size=$(stat -c%s "$fw")
  md5=$(md5sum "$fw" | awk '{print $1}')
  rev=$(rev_from_name "$fw")
  if [[ -z "$rev" ]]; then
    echo "WARNING: cannot derive git rev from '$(basename "$fw")' —" >&2
    echo "         post-flash version check degrades to reachability only." >&2
  fi

  pre_version="$(fetch_setting version)"
  if [[ "$pre_version" == "?" ]]; then
    echo "device unreachable at $TARGET" >&2
    return 3
  fi

  echo "Master flash  : $fw"
  echo "Size / MD5    : $size bytes / $md5"
  echo "Rev           : ${rev:-<unknown>} (device runs: $pre_version)"
  if [[ -n "$rev" && "$pre_version" == "$rev" ]]; then
    echo "NOTE: device already runs $rev — a rollback to the same rev would"
    echo "      be indistinguishable from success; verdict will say so."
  fi
  confirm

  local url="$TARGET/firmware/master?md5=$md5"
  [[ -n "$rev" ]] && url="$url&v=$rev"

  local http_code
  http_code=$(curl --progress-bar --show-error --max-time 180 \
    --output "$BODY" --write-out '%{http_code}' \
    -F "firmware=@$fw" "$url" || echo "000")
  echo "HTTP $http_code"
  cat "$BODY"; echo

  if [[ "$http_code" != "200" ]]; then
    echo "[fail] upload rejected — see response above" >&2
    return 3
  fi

  # New image boots PENDING_VERIFY; OtaService confirms it on first netif-up,
  # so "reachable over HTTP" and "confirmed" arrive together. The S3 reboots
  # in a few seconds; WiFi rejoin dominates the wait.
  echo "rebooting — polling /settings (up to 120 s)..."
  local deadline=$(( $(date +%s) + 120 ))
  local post_version="?" reverted="?"
  while (( $(date +%s) < deadline )); do
    sleep 3
    post_version="$(fetch_setting version)"
    if [[ "$post_version" != "?" ]]; then
      reverted="$(fetch_setting otaReverted)"
      break
    fi
  done

  echo "  version     : $pre_version -> $post_version"
  echo "  otaReverted : $reverted"

  if [[ "$post_version" == "?" ]]; then
    echo "[fail] device did not come back within 120 s — check it on the bench" >&2
    return 3
  fi
  if [[ "$reverted" == "True" ]]; then
    echo "[fail] ROLLED BACK — image booted but was reverted (bootloader A/B)" >&2
    return 3
  fi
  if [[ -z "$rev" ]]; then
    echo "[ok] device is back up (no rev to verify against)"
    return 0
  fi
  if [[ "$post_version" == "$rev" ]]; then
    if [[ "$pre_version" == "$rev" ]]; then
      echo "[ok] device runs $rev — but it already did; if this was a forced"
      echo "     reflash, confirm via GET $TARGET/debug/ota which slot booted."
    else
      echo "[ok] SUCCESS — $rev is running and confirmed."
    fi
    return 0
  fi
  echo "[fail] device came back on '$post_version', expected '$rev'" >&2
  return 3
}

# --- rescue image ----------------------------------------------------------

install_rescue() {
  local fw="$1"
  local size md5 rev
  size=$(stat -c%s "$fw")
  md5=$(md5sum "$fw" | awk '{print $1}')
  rev=$(rev_from_name "$fw")

  echo "Rescue install: $fw"
  echo "Size / MD5    : $size bytes / $md5"
  echo "Rev           : ${rev:-<unknown>}"
  confirm

  local http_code
  http_code=$(curl --progress-bar --show-error --max-time 180 \
    --output "$BODY" --write-out '%{http_code}' \
    -F "firmware=@$fw" "$TARGET/firmware/rescue?md5=$md5" || echo "000")
  echo "HTTP $http_code"
  cat "$BODY"; echo

  if [[ "$http_code" == "200" ]]; then
    echo "[ok] SUCCESS — rescue image in the factory slot (no reboot;"
    echo "     POST $TARGET/firmware/rescue-boot to test it)."
    return 0
  fi
  echo "[fail] rescue install rejected — see response above" >&2
  return 3
}

# --- main ------------------------------------------------------------------

echo "Device        : $TARGET"
if [[ -n "$LOCAL_FILE" ]]; then
  [[ -f "$LOCAL_FILE" ]] || { echo "not a file: $LOCAL_FILE" >&2; exit 2; }
  if [[ "$(basename "$LOCAL_FILE")" == *.factory.bin ]]; then
    echo "$(basename "$LOCAL_FILE") is a merged USB image (flash at 0x0 over" >&2
    echo "USB with esptool) — it is not a valid OTA payload" >&2
    exit 2
  fi
  # The filename prefix is the image type — never let it contradict the
  # mode: a master image POSTed to /firmware/rescue would overwrite the
  # break-glass factory slot with a non-rescue image.
  case "$(basename "$LOCAL_FILE")" in
    rescue-*)
      if [[ "$MODE" == "master" ]]; then
        echo "note: $(basename "$LOCAL_FILE") is a rescue image — installing to the factory slot"
        MODE="rescue"
      fi
      ;;
    firmware-*)
      if [[ "$MODE" == "rescue" ]]; then
        echo "$(basename "$LOCAL_FILE") is a master image — refusing to" >&2
        echo "install it into the rescue/factory slot (drop -r, or pass a" >&2
        echo "rescue-<rev>.bin)" >&2
        exit 2
      fi
      ;;
  esac
  echo "Source        : local file"
else
  echo "Source        : $SERVER:$REMOTE_DIR (latest by mtime)"
fi
echo "Mode          : $MODE"
echo

case "$MODE" in
  master)
    FW="${LOCAL_FILE:-$(fetch_latest firmware)}"
    flash_master "$FW"
    ;;
  rescue)
    FW="${LOCAL_FILE:-$(fetch_latest rescue)}"
    install_rescue "$FW"
    ;;
  all)
    FW_MASTER=$(fetch_latest firmware)
    FW_RESCUE=$(fetch_latest rescue)
    flash_master "$FW_MASTER"
    echo
    install_rescue "$FW_RESCUE"
    ;;
esac
