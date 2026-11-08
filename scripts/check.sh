#!/usr/bin/env bash
# The one check (#651): what the CI workflow ran, now that Actions is off.
# Run by agent-check (CHECK=), and as `--touched <base>` by the shared
# agent-pre-push (PREPUSH_CHECK=) and by every agent before its
# local commit.
#
#   scripts/check.sh                   every tree
#   scripts/check.sh unit master ...   some trees (unit master rescue follower py)
#   scripts/check.sh --touched [base]  the trees the diff base..HEAD + the
#                                      working tree touches (base: origin/master)
#
# Python suites run in their own venv with the CI pins (the link schema tests
# skip without grpcio-tools). PlatformIO shares one object cache across every
# checkout and worktree. Builds of different ESP platforms share
# ~/.platformio/packages/tool-esptoolpy and clobber it when they overlap, so the
# ESP8266 build takes an exclusive lock and the ESP32 builds a shared one.
set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
V2="$ROOT/firmware/v2"
CACHE="${XDG_CACHE_HOME:-$HOME/.cache}/split-flap"
export PLATFORMIO_BUILD_CACHE_DIR="${PLATFORMIO_BUILD_CACHE_DIR:-$CACHE/pio-build-cache}"
VENV="${SPLITFLAP_CHECK_VENV:-$CACHE/check-venv}"
LOCK="$CACHE/esptoolpy.lock"
PY_PINS=(pytest==9.2.0 pyserial==3.5 esptool==5.4.0 grpcio-tools==1.85.1 protobuf==7.36.2 lizard==1.25.1)
ALL_TREES=(unit master rescue follower py)

mkdir -p "$CACHE" "$PLATFORMIO_BUILD_CACHE_DIR"

die() { echo "check: $*" >&2; exit 2; }

ensure_venv() {
  local stamp="$VENV/.pins"
  if [[ -x "$VENV/bin/python" && -f "$stamp" && "$(cat "$stamp")" == "${PY_PINS[*]}" ]]; then
    return
  fi
  command -v uv >/dev/null || die "uv not found (needed for the pytest venv)"
  echo "check: building the pytest venv at $VENV"
  uv venv -q --python 3.12 --clear "$VENV"
  uv pip install -q --python "$VENV/bin/python" "${PY_PINS[@]}"
  echo "${PY_PINS[*]}" > "$stamp"
}

FAILED=()
PASSED=()

step() {
  local name="$1" dir="$2"; shift 2
  echo
  echo "=== $name"
  if (cd "$dir" && "$@"); then
    PASSED+=("$name")
  else
    FAILED+=("$name")
  fi
}

pio_locked() {
  local mode="$1"; shift
  flock "$mode" "$LOCK" pio "$@"
}

pytest_in() { "$VENV/bin/python" -m pytest -q "$@"; }

run_unit() {
  step "Unit build" "$V2/Unit" pio run
  step "Unit native" "$V2/Unit" pio test -e native
}

run_esp32() {  # $1 = label, $2 = dir
  step "$1 build" "$V2/$2" pio_locked -s run
  step "$1 native" "$V2/$2" pio test -e native
  step "$1 pytest" "$V2/$2" pytest_in tests/
}

run_master() { run_esp32 Master Master; }
run_rescue() { run_esp32 Rescue Rescue; }

run_follower() {
  step "FollowerEsp01 build" "$V2/FollowerEsp01" pio_locked -x run
  step "FollowerEsp01 native" "$V2/FollowerEsp01" pio test -e native
  step "FollowerEsp01 pytest" "$V2/FollowerEsp01" pytest_in tests/
}

run_py() {
  step "copied-header gate" "$ROOT" pytest_in tests/
  step "buildtools pytest" "$V2/buildtools" pytest_in tests/
  step "wall link pytest" "$V2/link" pytest_in tests/
  step "flasher pytest" "$ROOT/flashing" pytest_in flasher/tests/
  step "campaign script pytest" "$ROOT/flashing" pytest_in tests/
  step "unit bundle drift gate" "$ROOT/flashing" "$VENV/bin/python" flasher/make_manifest.py gate
}

# Maps changed paths to trees. shared/ and buildtools/ reach every firmware
# tree; release/ is Master + Rescue; link/ is Master + FollowerEsp01. The py
# gates always run: they are fast and pin cross-tree drift.
touched_trees() {
  local base="$1" f
  local -A want=([py]=1)
  while IFS= read -r f; do
    case "$f" in
      firmware/v2/Unit/*) want[unit]=1 ;;
      firmware/v2/Master/*) want[master]=1 ;;
      firmware/v2/Rescue/*) want[rescue]=1 ;;
      firmware/v2/FollowerEsp01/*) want[follower]=1 ;;
      firmware/v2/shared/*|firmware/v2/buildtools/*)
        want[unit]=1; want[master]=1; want[rescue]=1; want[follower]=1 ;;
      firmware/v2/release/*) want[master]=1; want[rescue]=1 ;;
      firmware/v2/link/*) want[master]=1; want[follower]=1 ;;
    esac
  done < <({ git -C "$ROOT" diff --name-only "$base"...HEAD; git -C "$ROOT" diff --name-only HEAD; } | sort -u)
  local t
  for t in "${ALL_TREES[@]}"; do [[ -n "${want[$t]:-}" ]] && echo "$t"; done
}

trees=()
if [[ "${1:-}" == "--touched" ]]; then
  base="${2:-origin/master}"
  git -C "$ROOT" rev-parse -q --verify "$base^{commit}" >/dev/null || die "unknown base: $base"
  mapfile -t trees < <(touched_trees "$base")
elif [[ $# -eq 0 ]]; then
  trees=("${ALL_TREES[@]}")
else
  for t in "$@"; do
    [[ " ${ALL_TREES[*]} " == *" $t "* ]] || die "unknown tree: $t (one of: ${ALL_TREES[*]})"
  done
  trees=("$@")
fi

echo "check: trees: ${trees[*]}"
ensure_venv
# The code standard gate runs for every selection, not only with the py tree.
step "code standard gate" "$ROOT" "$VENV/bin/python" -I scripts/check_standard.py
# Unit first: the drift gate and the Master/Follower bundles read its image.
for t in "${ALL_TREES[@]}"; do
  [[ " ${trees[*]} " == *" $t "* ]] && "run_$t"
done

echo
echo "check: ${#PASSED[@]} passed, ${#FAILED[@]} failed"
if [[ ${#FAILED[@]} -gt 0 ]]; then
  printf 'check: FAILED: %s\n' "${FAILED[@]}" >&2
  exit 1
fi
