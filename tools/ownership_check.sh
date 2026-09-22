#!/usr/bin/env bash
# Fail if the framework owns memory by hand.
#
# The rule (GUIDELINES.md, "Punteros y propiedad"): no bare new/delete, no
# malloc/free, under include/rmp/ or src/rmp/. What we own is a unique_ptr or
# a value; what we refer to is a Handle or a non-owning pointer that says so.
# This is a ratchet, like seam_check.sh: the files below are allowed to keep
# their calls, each with the reason, and the list only ever shrinks. Adding a
# file to it is a review question, not a fix.
#
# Usage:  tools/ownership_check.sh          (from the repo root)
# Exit:   0 = clean, 1 = a new owning call outside the list (with file:line)

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# `new` as an allocation (`new T`, `new T[`, `new (place) T`), `delete`, and
# the C allocator. Placement new is `new (` and is caught on purpose: it is an
# allocation decision too.
PATTERN='(^|[^A-Za-z0-9_:])(new|delete)([^A-Za-z0-9_]|$)|(^|[^A-Za-z0-9_])(malloc|calloc|realloc|free)[[:space:]]*\('

# Files allowed to keep their calls, and why. THE LIST ONLY SHRINKS.
ALLOWED=(
  "src/rmp/loader_hook.cpp"     # raylib's LoadFileData contract: the buffer is RL_MALLOC'd
                                # by us and RL_FREE'd by raylib's caller. C ABI, not ours to own.
  "src/rmp/pack.cpp"            # the same contract on the way out of the pack: pack_read()
                                # returns bytes UnloadFileData() frees.
)

fails=0
while IFS= read -r hit; do
  file="${hit%%:*}"
  allowed=no
  for a in "${ALLOWED[@]}"; do
    if [ "$file" = "$a" ]; then allowed=yes; break; fi
  done
  if [ "$allowed" = no ]; then
    echo "  FAIL  $hit"
    fails=$((fails + 1))
  fi
done < <(grep -rnE "$PATTERN" include/rmp src/rmp \
           --include='*.h' --include='*.cpp' \
           | grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' \
           | grep -vE '_impl\.cpp:' \
           | grep -vE 'delete;|= delete|new_|_new|newline|renew|delete\(' \
           | grep -vE '"[^"]*(^|[^A-Za-z0-9_])(new|delete)([^A-Za-z0-9_]|$)[^"]*"' \
           || true)

if [ "$fails" -ne 0 ]; then
  echo "FAIL: $fails owning call(s) outside the allowed list. Own it with std::unique_ptr"
  echo "      or a value; refer to it with rmp::Handle<T> or a non-owning pointer that says so."
  exit 1
fi
echo "  ok    no bare new/delete/malloc/free outside the ${#ALLOWED[@]} allowed files"
