#!/usr/bin/env bash
# PreToolUse (Bash) guard (#651).
#  --agent (run from the agent files' own hooks): agents are build-only, so any
#    command that reaches a board, publishes a release or writes to a wall API
#    is denied: ota-flash.sh, the unit campaign scripts, release.py, USB upload,
#    esptool writes, curl with a body or a write method.
#  always: git checkout -b/-B, switch or reset in the MAIN checkout answers
#    "ask" (agents work in .claude/worktrees/*; main only merges ff-only).
# Fails open on unparsable input.
set -u
mode="${1:-}"
input=$(cat 2>/dev/null || true)
cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // empty' 2>/dev/null || true)
cwd=$(printf '%s' "$input" | jq -r '.cwd // empty' 2>/dev/null || true)
[ -n "$cmd" ] || exit 0

decide() {
  jq -cn --arg d "$1" --arg r "$2" \
    '{hookSpecificOutput:{hookEventName:"PreToolUse",permissionDecision:$d,permissionDecisionReason:$r}}'
  exit 0
}

if [ "$mode" = --agent ]; then
  bench='(ota-flash\.sh|commission-units\.sh|restore-unit-offsets\.sh|ext-diag-benchcheck\.py|release\.py|make-release-key\.sh)'
  upload='pio[^|;&]*(-t|--target)[[:space:]]*upload'
  esptool='esptool(\.py)?[^|;&]*(write[-_]flash|erase[-_]flash|erase[-_]region|write[-_]mem|burn)'
  curlw='curl[^|;&]*(-X[[:space:]]*(POST|PUT|PATCH|DELETE)|--request[[:space:]]*(POST|PUT|PATCH|DELETE)|(^|[[:space:]])(-d|-F|--data[a-z-]*|--form|-T|--upload-file)([[:space:]]|=))'
  if printf '%s' "$cmd" | grep -qE "$bench|$upload|$esptool|$curlw"; then
    decide deny "Agents are build-only: boards, releases and wall APIs belong to the main session. Stop and report what you would have run."
  fi
fi

main_checkout="${CLAUDE_PROJECT_DIR:-}"
top=$(git -C "${cwd:-.}" rev-parse --show-toplevel 2>/dev/null || true)
if [ -n "$main_checkout" ] && [ "$top" = "$main_checkout" ] &&
   printf '%s' "$cmd" | grep -qE '(^|[;&|][[:space:]]*)git[[:space:]]+(-C[[:space:]]+[^[:space:]]+[[:space:]]+)?(checkout[[:space:]]+-[bB]\b|switch\b|reset\b)'; then
  case "$cmd" in *"/.claude/worktrees/"*) exit 0 ;; esac
  decide ask "git checkout -b/-B, switch or reset in the MAIN checkout: agents work in .claude/worktrees/*, main only merges ff-only. Confirm if deliberate."
fi
exit 0
