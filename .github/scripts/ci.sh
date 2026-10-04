#!/usr/bin/env bash

set -euo pipefail

readonly REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if ! command -v clang >/dev/null 2>&1; then
  echo "Assembler CI: clang is required for static analysis." >&2
  exit 1
fi
if ! command -v "${VALGRIND:-valgrind}" >/dev/null 2>&1; then
  echo "Assembler CI: '${VALGRIND:-valgrind}' is required for dynamic memory analysis." >&2
  exit 1
fi

# The keyword hash table in src/keyword.c is generated; reject hand edits
# that leave it out of step with tools/gen_keywords.py.
if ! python3 "$REPO_DIR/tools/gen_keywords.py" --check; then
  echo "Assembler CI: src/keyword.c does not match tools/gen_keywords.py output." >&2
  exit 1
fi

# Run suite.
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

cleanup_test="$(mktemp)"
cleanup_objects=()
for object in "$REPO_DIR"/build/debug/objfiles/*.o; do
  case "${object##*/}" in
    main.o|preprocessor.o) continue ;;
  esac
  cleanup_objects+=("$object")
done
if ! gcc -std=c11 -Wall -I"$REPO_DIR/src" \
    "$REPO_DIR/tests/preprocessor_allocation_test.c" \
    "${cleanup_objects[@]}" -o "$cleanup_test"; then
  rm -f "$cleanup_test"
  echo "Assembler CI: failed to build the preprocessor allocation regression test." >&2
  exit 1
fi
if ! "$cleanup_test"; then
  rm -f "$cleanup_test"
  echo "Assembler CI: preprocessor allocation regression test failed." >&2
  exit 1
fi
rm -f "$cleanup_test"

for source in "$REPO_DIR"/src/*.c; do
  clang --analyze -Wall -Wextra -Werror -ferror-limit=0 \
    --analyzer-output text -Xanalyzer -analyzer-werror "$source"
done

bash "$REPO_DIR/.github/scripts/valgrind.sh"
