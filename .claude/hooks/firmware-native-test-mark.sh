#!/usr/bin/env bash
# PostToolUse hook (Edit|Write): record which firmware/v2 envs an edit touches.
# Cheap and silent; firmware-native-test-run.sh (Stop hook) runs the tests.
# Mute: `touch /tmp/sf-native-test.off`.
set -uo pipefail

[ -e /tmp/sf-native-test.off ] && exit 0

INPUT=$(cat)
FILE=$(jq -r '.tool_input.file_path // empty' <<<"$INPUT" 2>/dev/null)
SID=$(jq -r '.session_id // empty' <<<"$INPUT" 2>/dev/null)
[ -z "$FILE" ] && exit 0
[[ "$SID" =~ ^[A-Za-z0-9_-]+$ ]] || exit 0

REPO=${CLAUDE_PROJECT_DIR:?}
STATE="$REPO/.claude/.state"
(umask 077; mkdir -p "$STATE") && chmod 700 "$STATE"
V2="$REPO/firmware/v2"

case "$FILE" in
  "$V2"/*.cpp|"$V2"/*.h|"$V2"/*.hpp|"$V2"/*.ino|"$V2"/*.c|"$V2"/*.ini) ;;
  *) exit 0 ;;
esac

# Every env that has native tests: a test dir and a native env in platformio.ini.
native_envs() {
  local d
  for d in "$V2"/*/; do
    [ -d "${d}test" ] && grep -q '^\[env:native\]' "${d}platformio.ini" 2>/dev/null \
      && basename "$d"
  done
}

REL=${FILE#"$V2"/}
TOP=${REL%%/*}
case "$TOP" in
  shared|link|buildtools) ENVS=$(native_envs) ;;
  *)
    if [ -d "$V2/$TOP/test" ] && grep -q '^\[env:native\]' "$V2/$TOP/platformio.ini" 2>/dev/null; then
      ENVS=$TOP
    else
      exit 0
    fi
    ;;
esac

OUT="$STATE/sf-native-touched-${SID}"
printf '%s\n' $ENVS >>"$OUT"
exit 0
