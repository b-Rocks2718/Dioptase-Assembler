#!/usr/bin/env bash

set -euo pipefail

readonly REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

run_suite() {
  local target="$1"
  local log
  local summary
  log="$(mktemp)"

  if ! make -C "$REPO_DIR" "$target" 2>&1 | tee "$log"; then
    rm -f "$log"
    echo "Assembler CI: make target '$target' failed in '$REPO_DIR'." >&2
    return 1
  fi

  summary="$(grep -E '^Summary: [0-9]+ / [0-9]+ tests passed\.$' "$log" | tail -n 1 || true)"
  rm -f "$log"

  if [[ ! "$summary" =~ ^Summary:\ ([0-9]+)\ /\ ([0-9]+)\ tests\ passed\.$ ]]; then
    echo "Assembler CI: expected an aggregate summary from make target '$target', observed '${summary:-none}'." >&2
    return 1
  fi

  if [ "${BASH_REMATCH[1]}" != "${BASH_REMATCH[2]}" ]; then
    echo "Assembler CI: make target '$target' reported failing tests: $summary" >&2
    return 1
  fi
}

run_suite test
run_suite test-release
