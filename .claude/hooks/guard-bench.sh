#!/usr/bin/env bash
# PreToolUse (Bash) guard for split-flap agents (#651, #655, #656), run with
# --agent from the agent files' own hooks next to the global
# deny-agent-side-effects.sh. It adds this repo's bench tools: ota-flash.sh,
# the unit campaign scripts, release.py, USB upload, esptool writes and
# curl/wget/httpie/requests writes to a wall API are denied. Fails closed
# (no jq, unparsable input). Without --agent it does nothing: the global
# guard-main-checkout.sh owns branch moves in the main checkout. A tripwire for
# honest mistakes, not a sandbox.
set -u
mode="${1:-}"
[ "$mode" = --agent ] || exit 0
input=$(cat 2>/dev/null || true)

agent_deny() {
  printf '%s\n' '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"'"$1"'"}}'
  exit 0
}

if ! command -v jq >/dev/null 2>&1; then
  [ "$mode" = --agent ] && agent_deny "guard-bench.sh needs jq and cannot check this command. Stop and report."
  echo "guard-bench.sh: jq missing, not checking" >&2
  exit 0
fi
if ! cmd=$(printf '%s' "$input" | jq -er '.tool_input.command' 2>/dev/null); then
  [ "$mode" = --agent ] && agent_deny "guard-bench.sh could not read the command. Stop and report."
  exit 0
fi

decide() {
  jq -cn --arg d "$1" --arg r "$2" \
    '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:$d,permissionDecisionReason:$r}}'
  exit 0
}

if [ "$mode" = --agent ]; then
  w='(POST|PUT|PATCH|DELETE)'
  bench='(ota-flash\.sh|commission-units\.sh|restore-unit-offsets\.sh|ext-diag-benchcheck\.py|release\.py|make-release-key\.sh)'
  upload='(pio|platformio)[^|;&]*(-t|--target)[[:space:]=]*upload'
  esptool='esptool(\.py)?[^|;&]*(write[-_]flash|erase[-_]flash|erase[-_]region|write[-_]mem|burn)'
  curlw="curl[^|;&]*((^|[[:space:]])-[A-Za-z]*X[[:space:]]*$w|--request[[:space:]=]*$w|(^|[[:space:]])-[A-Za-z]*[dFT]|--data|--form|--json|--upload-file)"
  wgetw='wget[^|;&]*--(post|method|body)'
  httpiew="(^|[;&|[:space:]])(http|https|xh)[[:space:]]+(-[^[:space:]]+[[:space:]]+)*$w([[:space:]]|$)"
  pyw='requests\.(post|put|patch|delete)|urlopen\('
  if printf '%s' "$cmd" | grep -qE "$bench|$upload|$esptool|$curlw|$wgetw|$httpiew|$pyw"; then
    decide deny "Agents are build-only: boards, releases and wall APIs belong to the main session. Stop and report what you would have run."
  fi
fi

exit 0
