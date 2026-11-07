#!/usr/bin/env bash
# PreToolUse (Bash) guard (#651, #655). A tripwire for honest mistakes, not a
# sandbox: the agents are ours.
#  --agent (run from the agent files' own hooks): agents are build-only, so any
#    command that reaches a board, publishes a release or writes to a wall API
#    is denied: ota-flash.sh, the unit campaign scripts, release.py, USB upload,
#    esptool writes, curl/wget/httpie/requests with a body or a write method.
#    Fails closed: without jq or on unparsable input the command is denied.
#  always: git checkout -b/-B, switch or reset whose target is the MAIN
#    checkout answers "ask" (agents work in .claude/worktrees/*; main only
#    merges ff-only). Only a `git -C <pool slot>` is exempt.
set -u
mode="${1:-}"
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
cwd=$(printf '%s' "$input" | jq -r '.cwd // empty' 2>/dev/null || true)

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

main_checkout="${CLAUDE_PROJECT_DIR:-}"
[ -n "$main_checkout" ] || exit 0
pool="$main_checkout/.claude/worktrees/"
top=$(git -C "${cwd:-.}" rev-parse --show-toplevel 2>/dev/null || true)
# Fails closed: a move whose target is unknown asks. Matching is per shell
# segment and deliberately loose (`git log --grep reset` asks too): only an
# explicit -C that resolves into the pool, with nothing redirecting git, skips.
move='(^|[[:space:](])git([[:space:]].*)?[[:space:]](switch|reset)([[:space:]]|$)|(^|[[:space:](])git([[:space:]].*)?[[:space:]]checkout[[:space:]]+-[bB]([[:space:]]|$)'
redirect='(^|[[:space:];&|(])(cd|pushd)[[:space:]]|GIT_DIR=|GIT_WORK_TREE=|--git-dir|--work-tree'
ask_main() {
  decide ask "git checkout -b/-B, switch or reset whose target is the MAIN checkout or unclear: agents work in .claude/worktrees/* via git -C, main only merges ff-only. Confirm if deliberate."
}
redirected=0
printf '%s' "$cmd" | grep -qE -- "$redirect" && redirected=1
while IFS= read -r seg || [ -n "$seg" ]; do
  printf '%s' "$seg" | grep -qE -- "$move" || continue
  [ "$redirected" = 1 ] && ask_main
  dir=$(printf '%s' "$seg" | grep -oE -- '(^|[[:space:]])-C[[:space:]]+[^[:space:]]+' | tail -n 1 | sed -E 's/^[[:space:]]*-C[[:space:]]+//')
  if [ -z "$dir" ]; then
    target="$top"
  else
    case "$dir" in /*) ;; *) dir="${cwd:-.}/$dir" ;; esac
    target=$(git -C "$dir" rev-parse --show-toplevel 2>/dev/null) || ask_main
  fi
  case "$target/" in "$pool"*) continue ;; esac
  if [ -z "$target" ] || [ "$target" = "$main_checkout" ]; then ask_main; fi
done < <(printf '%s' "$cmd" | sed -E 's/(&&|\|\||[;&|])/\n/g')
exit 0
