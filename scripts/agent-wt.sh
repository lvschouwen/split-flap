#!/usr/bin/env bash
# Warm worktree pool for agents (#651), under .claude/worktrees/wt-N.
# A cold pioarduino build is slow, so slots are reset and reused, never
# removed: each tree's .pio stays warm, and the shared PlatformIO build cache
# (scripts/check.sh) keeps a new slot's first build cheap. Every git write goes
# through `git -C <slot>`, never against the main checkout.
#
#   agent-wt.sh list
#   agent-wt.sh new --next | <slot> [base]
#   agent-wt.sh reset [--force] <slot> <branch> [base]
#   agent-wt.sh done <slot>          (after the ff-only merge into master)
set -euo pipefail

ROOT="${AGENT_WT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && git rev-parse --show-toplevel)}"
POOL="$ROOT/.claude/worktrees"
MAIN=master

die() { echo "agent-wt: $*" >&2; exit 1; }

usage() {
  sed -n '8,11p' "${BASH_SOURCE[0]}" | sed 's/^# //' >&2
  exit 2
}

slot_dir() {
  [[ "$1" =~ ^[A-Za-z0-9._-]+$ ]] || die "bad slot name: $1"
  echo "$POOL/$1"
}

need_slot() {
  local d
  d="$(slot_dir "$1")"
  [[ -e "$d/.git" ]] || die "no such slot: $1 (use 'new')"
  echo "$d"
}

is_dirty() { [[ -n "$(git -C "$1" status --porcelain)" ]]; }
branch_exists() { git -C "$ROOT" show-ref --verify --quiet "refs/heads/$1"; }

cmd_list() {
  local d slot br sha ab dirty warm
  mkdir -p "$POOL"
  for d in "$POOL"/*/; do
    d="${d%/}"
    [[ -e "$d/.git" ]] || continue
    slot="$(basename "$d")"
    br="$(git -C "$d" symbolic-ref -q --short HEAD || echo '(detached)')"
    sha="$(git -C "$d" rev-parse --short HEAD)"
    ab="$(git -C "$d" rev-list --left-right --count "$MAIN...HEAD" | awk '{print "+" $2 "/-" $1}')"
    if is_dirty "$d"; then dirty=yes; else dirty=no; fi
    if [[ -d "$d/firmware/v2/Master/.pio" ]]; then warm=warm; else warm=cold; fi
    printf '%-8s %-28s %s  ahead/behind %s  dirty=%s  %s\n' "$slot" "$br" "$sha" "$ab" "$dirty" "$warm"
  done
}

next_slot() {
  local n=1
  while [[ -e "$POOL/wt-$n" ]]; do n=$((n + 1)); done
  echo "wt-$n"
}

cmd_new() {
  [[ $# -ge 1 && $# -le 2 ]] || usage
  local slot="$1" base="${2:-$MAIN}" d
  [[ "$slot" == --next ]] && slot="$(next_slot)"
  d="$(slot_dir "$slot")"
  [[ ! -e "$d" ]] || die "slot $slot already exists"
  mkdir -p "$POOL"
  git -C "$ROOT" worktree add -q --detach "$d" "$base"
  echo "agent-wt: the first build in $slot is cold (the shared build cache helps)" >&2
  echo "$d"
}

cmd_reset() {
  local force=0
  [[ "${1:-}" == --force ]] && { force=1; shift; }
  [[ $# -ge 2 && $# -le 3 ]] || usage
  local slot="$1" branch="$2" base="${3:-$MAIN}" d
  d="$(need_slot "$slot")"
  [[ "$branch" != "$MAIN" ]] || die "an agent branch is never $MAIN"
  if is_dirty "$d" && [[ $force -eq 0 ]]; then
    die "slot $slot is dirty (use --force to discard)"
  fi
  if branch_exists "$branch" && ! git -C "$d" merge-base --is-ancestor "$branch" "$base"; then
    die "branch $branch exists and is not merged into $base"
  fi
  # Untracked build output (.pio) survives both forms.
  if [[ $force -eq 1 ]]; then
    git -C "$d" checkout -q -f -B "$branch" "$base"
  else
    git -C "$d" checkout -q -B "$branch" "$base"
  fi
  echo "$d"
}

cmd_done() {
  [[ $# -eq 1 ]] || usage
  local d br
  d="$(need_slot "$1")"
  br="$(git -C "$d" symbolic-ref -q --short HEAD)" || die "slot $1 is already detached"
  is_dirty "$d" && die "slot $1 is dirty"
  git -C "$d" merge-base --is-ancestor "$br" "$MAIN" || die "branch $br is not merged into $MAIN"
  git -C "$d" checkout -q --detach "$MAIN"
  git -C "$d" branch -q -d "$br"
  echo "$d"
}

[[ $# -ge 1 ]] || usage
sub="$1"; shift
case "$sub" in
  list) cmd_list "$@" ;;
  new) cmd_new "$@" ;;
  reset) cmd_reset "$@" ;;
  done) cmd_done "$@" ;;
  *) usage ;;
esac
