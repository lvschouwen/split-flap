#!/usr/bin/env bash
# Links the tracked git hooks into .git/hooks (#651). Leaves the graphify
# post-* hooks alone. Safe to run again.
set -euo pipefail
ROOT="$(git rev-parse --show-toplevel)"
HOOKS="$(git rev-parse --git-common-dir)/hooks"
for h in pre-push; do
  ln -sfn "$ROOT/scripts/$h" "$HOOKS/$h"
  echo "install-hooks: $HOOKS/$h -> scripts/$h"
done
