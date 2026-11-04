#!/usr/bin/env bash
# PostToolUse hook: after an edit to a v2 firmware source file, run that
# project's native (host) test suite so logic breakage surfaces immediately.
#
# Design notes:
#  - Only fires for .cpp/.h/.hpp/.ino under firmware/v2/{Unit,Master,Rescue,FollowerEsp01}.
#  - Debounced per-project (DEBOUNCE_S): rapid batch edits trigger at most one run,
#    so a 10-file edit burst costs one native build, not ten.
#  - Silent on PASS (exit 0). On FAIL prints a short report to stderr and exits 2,
#    which surfaces the failure back to the agent.
#  - Disable: remove the PostToolUse block from .claude/settings.json,
#    or `touch /tmp/sf-native-test.off` to mute without editing config.

set -uo pipefail

[ -e /tmp/sf-native-test.off ] && exit 0

PIO=/home/lucas/.local/bin/pio
REPO=/home/lucas/coding/split-flap
DEBOUNCE_S=120

# file_path from the tool call (Edit/Write/MultiEdit all carry it)
FILE=$(jq -r '.tool_input.file_path // empty' 2>/dev/null)
[ -z "$FILE" ] && exit 0

# Only v2 firmware source files
case "$FILE" in
  "$REPO"/firmware/v2/*.cpp|"$REPO"/firmware/v2/*.h|"$REPO"/firmware/v2/*.hpp|"$REPO"/firmware/v2/*.ino) ;;
  *) exit 0 ;;
esac

# Map to owning project dir
PROJ=""
for p in Unit Master Rescue FollowerEsp01; do
  case "$FILE" in "$REPO"/firmware/v2/"$p"/*) PROJ="$p"; break ;; esac
done
[ -z "$PROJ" ] && exit 0   # shared/ headers etc. — no single owning env

DIR="$REPO/firmware/v2/$PROJ"
STAMP="/tmp/sf-native-test-$PROJ.stamp"

# Debounce
if [ -f "$STAMP" ]; then
  AGE=$(( $(date +%s) - $(stat -c %Y "$STAMP") ))
  [ "$AGE" -lt "$DEBOUNCE_S" ] && exit 0
fi
touch "$STAMP"

OUT=$("$PIO" test -e native -d "$DIR" 2>&1)
if [ $? -ne 0 ]; then
  echo "❌ native tests FAILED for firmware/v2/$PROJ (edited $(basename "$FILE"))" >&2
  echo "$OUT" | grep -iE 'fail|error|assert|expected' | tail -15 >&2
  echo "(run: pio test -e native  in $DIR)" >&2
  exit 2
fi
exit 0
