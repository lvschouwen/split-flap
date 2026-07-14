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
# Usage: ota-flash.sh [-s user@host] [-d dir] [-r|-a] [-l file.bin] [-y] <device> [<device>...]
#
#   <device>        IP, hostname, or http://host[:port] of a running board.
#                   Several devices flash sequentially with a per-device
#                   verdict summary (#299) — exit is non-zero if any failed.
#   -s user@host    build server to fetch from (default: $SPLITFLAP_BIN_HOST)
#   -d dir          staging directory on the server (default:
#                   $SPLITFLAP_BIN_DIR, else bench-bins in the remote home)
#   -r              install the latest rescue image instead of the master
#                   firmware (POST /firmware/rescue)
#   -a              flash the latest master firmware, then — once the new
#                   image is confirmed running — install the latest rescue
#   -l file.bin     skip the download and upload this local file (its
#                   firmware-<rev>/follower-<rev>/rescue-<rev> name still
#                   supplies the expected rev; not valid with -a)
#   -y              don't ask for confirmation
#
# Platform autodetect (#299): each device's GET /settings is read first —
# `plat":"esp01"` (the #298 ESP-01 follower row) gets the latest
# follower-<rev>.bin; anything else is an S3 master and gets
# firmware-<rev>.bin as before. -r/-a are refused for esp01 targets (no
# factory slot there), and a local file whose prefix contradicts the
# device's platform is refused — an S3 image POSTed at an ESP-01 (or vice
# versa) would brick-until-rollback for nothing.
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
  echo "usage: $0 [-s user@host] [-d dir] [-r|-a] [-l file.bin] [-y] <device> [<device>...]" >&2
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

DEVICES=("$@")
[[ ${#DEVICES[@]} -ge 1 ]] || usage

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

# Accept a bare IP/hostname or a full http://host[:port]. Sets the global
# TARGET every helper below reads; validated up-front for every device so a
# typo fails before the first board is touched.
set_target() {
  local device="$1"
  if [[ "$device" =~ ^https?:// ]]; then
    TARGET="$device"
  else
    TARGET="http://$device"
  fi
  [[ "$TARGET" =~ ^https?://[^/]+$ ]] || {
    echo "device must be an IP/hostname or http://host[:port] (no trailing path): $device" >&2
    exit 2
  }
}
for dev in "${DEVICES[@]}"; do set_target "$dev"; done

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

# Staleness warning (#262): this script lives on the operator machine but
# evolves in the repo — a stale copy silently misses fixes. Compare against
# the copy staged next to the bins; warn-only (never blocks a flash, never
# self-modifies). Soft-fails silently: no staged copy / no ssh = no verdict.
check_script_freshness() {
  local remote_md5 local_md5
  remote_md5=$(ssh "${SSH_OPTS[@]}" -- "$SERVER" bash -s -- "$REMOTE_DIR" <<'REMOTE'
dir="$1"
md5sum -- "$dir/ota-flash.sh" 2>/dev/null | awk '{print $1}'
exit 0
REMOTE
  ) || return 0
  [[ -n "$remote_md5" ]] || return 0
  local_md5=$(md5sum -- "$0" | awk '{print $1}')
  if [[ "$remote_md5" != "$local_md5" ]]; then
    echo "NOTE: this script differs from the copy staged on the build server —"
    echo "      update with: scp $SERVER:$REMOTE_DIR/ota-flash.sh $0"
  fi
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

# firmware-/follower-/rescue-<rev>.bin → <rev> (keeps any -dirty suffix so a
# dirty build is called out in the verdict rather than silently matching).
rev_from_name() {
  local base
  base=$(basename "$1")
  if [[ "$base" =~ ^(firmware|follower|rescue)-([a-f0-9]+(-dirty)?)([.-].*)?\.bin$ ]]; then
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

# One multipart upload attempt; sets HTTP_CODE and CURL_RC. `Expect:` is
# suppressed: curl's interim "100 continue" used to leak into %{http_code},
# so a dropped connection printed "HTTP 100000" and was misread as a device
# rejection (#248).
upload_bin() {
  local fw="$1" url="$2"
  CURL_RC=0
  HTTP_CODE=$(curl --progress-bar --show-error --max-time 180 \
    -H 'Expect:' \
    --output "$BODY" --write-out '%{http_code}' \
    -F "firmware=@$fw" "$url") || CURL_RC=$?
}

# Upload with ONE retry on a network-level failure (curl exit 7 refused /
# 28 timeout / 52 empty reply / 55 send / 56 recv). Bench 2026-07-13: a
# mid-upload connection reset succeeded on a plain re-run — the device
# aborts the stale half-session on the next Update.begin, so an immediate
# retry is safe. HTTP-level rejections are never retried.
upload_with_retry() {
  local fw="$1" url="$2"
  upload_bin "$fw" "$url"
  case "$CURL_RC" in
    7|28|52|55|56)
      echo "[warn] network failure during upload (curl exit $CURL_RC) — retrying once..." >&2
      sleep 3
      upload_bin "$fw" "$url"
      ;;
  esac
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

  upload_with_retry "$fw" "$url"
  if (( CURL_RC != 0 )); then
    echo "[fail] upload failed at the network level (curl exit $CURL_RC —" >&2
    echo "       see curl's message above); the running image is untouched" >&2
    return 3
  fi
  echo "HTTP $HTTP_CODE"
  cat "$BODY"; echo

  if [[ "$HTTP_CODE" != "200" ]]; then
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

  upload_with_retry "$fw" "$TARGET/firmware/rescue?md5=$md5"
  if (( CURL_RC != 0 )); then
    echo "[fail] upload failed at the network level (curl exit $CURL_RC —" >&2
    echo "       see curl's message above); the factory slot is untouched" >&2
    return 3
  fi
  echo "HTTP $HTTP_CODE"
  cat "$BODY"; echo

  if [[ "$HTTP_CODE" == "200" ]]; then
    echo "[ok] SUCCESS — rescue image in the factory slot (no reboot;"
    echo "     POST $TARGET/firmware/rescue-boot to test it)."
    return 0
  fi
  echo "[fail] rescue install rejected — see response above" >&2
  return 3
}

# --- main ------------------------------------------------------------------

if [[ -n "$LOCAL_FILE" ]]; then
  [[ -f "$LOCAL_FILE" ]] || { echo "not a file: $LOCAL_FILE" >&2; exit 2; }
  if [[ "$(basename "$LOCAL_FILE")" == *.factory.bin ]]; then
    echo "$(basename "$LOCAL_FILE") is a merged USB image (flash at 0x0 over" >&2
    echo "USB with esptool) — it is not a valid OTA payload" >&2
    exit 2
  fi
  # The filename prefix is the image type — never let it contradict the
  # mode: a master image POSTed to /firmware/rescue would overwrite the
  # break-glass factory slot with a non-rescue image. The per-device
  # platform guard below additionally matches firmware- vs follower-
  # against what each board reports (#299).
  case "$(basename "$LOCAL_FILE")" in
    rescue-*)
      if [[ "$MODE" == "master" ]]; then
        echo "note: $(basename "$LOCAL_FILE") is a rescue image — installing to the factory slot"
        MODE="rescue"
      fi
      ;;
    firmware-*|follower-*)
      if [[ "$MODE" == "rescue" ]]; then
        echo "$(basename "$LOCAL_FILE") is a master/follower image — refusing" >&2
        echo "to install it into the rescue/factory slot (drop -r, or pass a" >&2
        echo "rescue-<rev>.bin)" >&2
        exit 2
      fi
      ;;
  esac
  echo "Source        : local file ($(basename "$LOCAL_FILE"))"
else
  echo "Source        : $SERVER:$REMOTE_DIR (latest by mtime)"
fi
echo "Mode          : $MODE"
if [[ -n "$SERVER" ]]; then check_script_freshness; fi

# Lazy per-prefix fetch cache (#299): with mixed-platform device lists each
# bin downloads once, on first need. Runs in the PARENT shell (result via
# FW_RESULT, never a command substitution) so the cache — and the
# DOWNLOAD_DIR the EXIT trap cleans — actually persist.
FW_CACHE_FIRMWARE=""
FW_CACHE_FOLLOWER=""
FW_CACHE_RESCUE=""
FW_RESULT=""
fetch_cached() {
  local prefix="$1" cached
  case "$prefix" in
    firmware) cached="$FW_CACHE_FIRMWARE" ;;
    follower) cached="$FW_CACHE_FOLLOWER" ;;
    rescue)   cached="$FW_CACHE_RESCUE" ;;
  esac
  if [[ -z "$cached" ]]; then
    # Create the download dir here so the fetch subshell inherits it and
    # the cleanup trap sees it.
    [[ -n "$DOWNLOAD_DIR" ]] || DOWNLOAD_DIR=$(mktemp -d)
    cached=$(fetch_latest "$prefix") || return $?
    case "$prefix" in
      firmware) FW_CACHE_FIRMWARE="$cached" ;;
      follower) FW_CACHE_FOLLOWER="$cached" ;;
      rescue)   FW_CACHE_RESCUE="$cached" ;;
    esac
  fi
  FW_RESULT="$cached"
}

# One device's full flow: platform detect → image pick/guard → flash.
run_device() {
  local device="$1"
  set_target "$device"
  echo
  echo "Device        : $TARGET"

  # Platform autodetect (#299): esp01 = the #298 follower row; anything
  # else (incl. pre-#299 firmware without the key) = S3 master.
  local plat
  plat="$(fetch_setting plat)"
  if [[ "$plat" == "?" ]]; then
    echo "device unreachable at $TARGET" >&2
    return 3
  fi
  local prefix="firmware"
  if [[ "$plat" == "esp01" ]]; then
    prefix="follower"
    echo "Platform      : esp01 (ESP-01 follower row)"
    if [[ "$MODE" != "master" ]]; then
      echo "an ESP-01 follower has no rescue/factory slot — drop -r/-a for $device" >&2
      return 2
    fi
  else
    echo "Platform      : ${plat:-esp32} (master)"
  fi

  if [[ -n "$LOCAL_FILE" && "$MODE" != "rescue" ]]; then
    # firmware- vs follower- must match the board (#299) — the wrong image
    # flashes fine and then rolls back (S3) or bricks-until-reflash (ESP-01).
    case "$(basename "$LOCAL_FILE")" in
      follower-*)
        if [[ "$prefix" != "follower" ]]; then
          echo "$(basename "$LOCAL_FILE") is an ESP-01 follower image but $device is not an esp01 board" >&2
          return 2
        fi
        ;;
      firmware-*)
        if [[ "$prefix" != "firmware" ]]; then
          echo "$(basename "$LOCAL_FILE") is an S3 master image but $device reports plat=esp01" >&2
          return 2
        fi
        ;;
    esac
  fi

  local fw
  case "$MODE" in
    master)
      if [[ -n "$LOCAL_FILE" ]]; then
        fw="$LOCAL_FILE"
      else
        fetch_cached "$prefix" || return 3
        fw="$FW_RESULT"
      fi
      flash_master "$fw"
      ;;
    rescue)
      if [[ -n "$LOCAL_FILE" ]]; then
        fw="$LOCAL_FILE"
      else
        fetch_cached rescue || return 3
        fw="$FW_RESULT"
      fi
      install_rescue "$fw"
      ;;
    all)
      fetch_cached firmware || return 3
      fw="$FW_RESULT"
      local fw_rescue
      fetch_cached rescue || return 3
      fw_rescue="$FW_RESULT"
      flash_master "$fw" || return $?
      echo
      install_rescue "$fw_rescue"
      ;;
  esac
}

FAILED=0
RESULTS=()
for dev in "${DEVICES[@]}"; do
  if run_device "$dev"; then
    RESULTS+=("[ok]   $dev")
  else
    RESULTS+=("[FAIL] $dev")
    FAILED=1
  fi
done

if [[ ${#DEVICES[@]} -gt 1 ]]; then
  echo
  echo "Summary:"
  for line in "${RESULTS[@]}"; do echo "  $line"; done
fi
exit $(( FAILED ? 3 : 0 ))
