#!/usr/bin/env bash

set -euo pipefail

readonly REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
readonly SCRIPT_PATH="$REPO_DIR/.github/scripts/valgrind.sh"
# Keep this distinct from the assembler's ordinary success/failure statuses.
readonly MEMCHECK_ERROR_EXIT_CODE=97

run_under_memcheck() {
  if [ "$#" -eq 0 ]; then
    echo "Assembler Valgrind: --run requires an executable." >&2
    return 2
  fi
  if [ -z "${DIOPTASE_VALGRIND_LOG_DIR:-}" ]; then
    echo "Assembler Valgrind: DIOPTASE_VALGRIND_LOG_DIR is not set." >&2
    return 2
  fi

  local log
  local status
  log="$(mktemp "$DIOPTASE_VALGRIND_LOG_DIR/memcheck.XXXXXX.log")"
  printf '%q ' "$@" > "$log.command"
  printf '\n' >> "$log.command"

  set +e
  "${VALGRIND:-valgrind}" \
    --tool=memcheck \
    --quiet \
    --leak-check=full \
    --show-leak-kinds=definite,indirect \
    --errors-for-leak-kinds=definite,indirect \
    --track-origins=yes \
    --error-exitcode="$MEMCHECK_ERROR_EXIT_CODE" \
    --log-file="$log" \
    "$@"
  status=$?
  set -e

  # Invalid-input tests expect the assembler to return nonzero. Record Memcheck
  # diagnostics out-of-band so such a test cannot hide a memory error.
  if [ "$status" -eq "$MEMCHECK_ERROR_EXIT_CODE" ] || [ -s "$log" ]; then
    touch "$log.finding"
  fi
  return "$status"
}

if [ "${1:-}" = "--run" ]; then
  shift
  run_under_memcheck "$@"
  exit $?
fi

if [ "$#" -ne 0 ]; then
  echo "Usage: $0" >&2
  exit 2
fi

valgrind_bin="${VALGRIND:-valgrind}"
if ! command -v "$valgrind_bin" >/dev/null 2>&1; then
  echo "Assembler Valgrind: '$valgrind_bin' is required; install Valgrind or set VALGRIND to its path." >&2
  exit 1
fi
if ! "$valgrind_bin" --version >/dev/null 2>&1; then
  echo "Assembler Valgrind: '$valgrind_bin --version' failed." >&2
  exit 1
fi

work_dir="$(mktemp -d)"
trap 'rm -rf "$work_dir"' EXIT
export DIOPTASE_VALGRIND_LOG_DIR="$work_dir"
export VALGRIND="$valgrind_bin"

suite_log="$work_dir/suite.log"
runner="$SCRIPT_PATH --run $REPO_DIR/build/debug/basm"
set +e
make -C "$REPO_DIR" test TEST_EXEC="$runner" 2>&1 | tee "$suite_log"
make_status=${PIPESTATUS[0]}
set -e

failed=0
if [ "$make_status" -ne 0 ]; then
  echo "Assembler Valgrind: make test exited with status $make_status." >&2
  failed=1
fi

summary="$(grep -E '^Summary: [0-9]+ / [0-9]+ tests passed\.$' "$suite_log" | tail -n 1 || true)"
if [[ ! "$summary" =~ ^Summary:\ ([0-9]+)\ /\ ([0-9]+)\ tests\ passed\.$ ]]; then
  echo "Assembler Valgrind: expected an aggregate test summary, observed '${summary:-none}'." >&2
  failed=1
elif [ "${BASH_REMATCH[1]}" != "${BASH_REMATCH[2]}" ]; then
  echo "Assembler Valgrind: functional tests failed under Memcheck: $summary" >&2
  failed=1
fi

shopt -s nullglob
findings=("$work_dir"/*.log.finding)
if [ "${#findings[@]}" -ne 0 ]; then
  echo "Assembler Valgrind: Memcheck reported ${#findings[@]} failing invocation(s)." >&2
  for marker in "${findings[@]}"; do
    log="${marker%.finding}"
    echo "--- ${log##*/} ---" >&2
    echo "  Command: $(cat "$log.command")" >&2
    sed 's/^/  /' "$log" >&2
  done
  failed=1
fi

if [ "$failed" -ne 0 ]; then
  exit 1
fi
echo "Assembler Valgrind: all debug-suite invocations passed Memcheck."
