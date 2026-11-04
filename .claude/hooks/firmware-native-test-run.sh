#!/usr/bin/env bash
# Stop hook: run `pio test -e native` once per env touched this session
# (recorded by firmware-native-test-mark.sh), then clear the record.
# Failure blocks the stop so the model fixes it; success adds context.
set -uo pipefail

INPUT=$(cat)
SID=$(jq -r '.session_id // empty' <<<"$INPUT" 2>/dev/null)
[[ "$SID" =~ ^[A-Za-z0-9_-]+$ ]] || exit 0
REPO=${CLAUDE_PROJECT_DIR:?}
TOUCHED="$REPO/.claude/.state/sf-native-touched-${SID}"
[ -f "$TOUCHED" ] && [ ! -L "$TOUCHED" ] && [ -O "$TOUCHED" ] && [ -s "$TOUCHED" ] || exit 0

PIO=$(command -v pio || true)
[ -z "$PIO" ] && [ -x "$HOME/.platformio/penv/bin/pio" ] && PIO="$HOME/.platformio/penv/bin/pio"
if [ -z "$PIO" ]; then
  jq -n '{additionalContext: "native tests skipped: pio not found"}'
  exit 0
fi

mapfile -t ENVS < <(sort -u "$TOUCHED")
rm -f "$TOUCHED"

PASSED=()
for e in "${ENVS[@]}"; do
  [[ "$e" =~ ^[A-Za-z0-9_-]+$ ]] || continue
  case "$e" in Master|FollowerEsp01|Rescue|Unit) ;; *) continue ;; esac
  [ -d "$REPO/firmware/v2/$e" ] || continue
  OUT=$("$PIO" test -e native -d "$REPO/firmware/v2/$e" 2>&1)
  if [ $? -ne 0 ]; then
    jq -n --arg r "$e: $(tail -n 30 <<<"$OUT")" '{decision: "block", reason: $r}'
    exit 0
  fi
  PASSED+=("$e")
done

[ "${#PASSED[@]}" -gt 0 ] || exit 0
jq -n --arg c "native tests passed: ${PASSED[*]}" '{additionalContext: $c}'
