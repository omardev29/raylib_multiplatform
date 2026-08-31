#!/usr/bin/env bash
# Every public header compiles on its own, and every one of them carries the
# .toml values.
#
# TWO PROMISES, ONE CHECK. For each include/rmp/*.h this compiles a translation
# unit that includes ONLY that header and then uses APP_WINDOW_WIDTH:
#
#   self-contained  a header that needs something included before it works by
#                   accident of whatever the .cpp happened to include first,
#                   and breaks for the next person who includes it alone.
#   config is there rmp/config.h is in every public header on purpose, so that
#                   the values from raylib_multiplatform.toml are simply present
#                   wherever the user is writing. If a new header forgets it,
#                   nothing fails loudly — the user just gets an undefined macro
#                   in a file that looks like every other file. This is what
#                   makes that a build failure here instead.
#
# Compiling is the check rather than grepping for the include line, because the
# include line is not the promise: reaching the macros is.
#
# Usage: tools/header_check.sh          (from the repo root)

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The generated header has to exist, or every header fails for a reason that
# has nothing to do with this check.
python3 tools/configure.py > /dev/null || {
  echo "FALLA: tools/configure.py could not generate the config"; exit 1; }

CXX="${CXX:-g++}"
INCLUDES=(-Iinclude -Ithirdparty/raylib/src -Ithirdparty/rres -Ithirdparty/raymob
          -Ithirdparty/clay -Ithirdparty)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fails=0
checked=0
for header in include/rmp/*.h; do
  name=$(basename "$header")
  # config.h is the one being propagated, so it proves nothing about itself.
  [ "$name" = "config.h" ] && continue

  cat > "$TMP/one.cpp" <<CPP
#include <rmp/$name>
int main() { return APP_WINDOW_WIDTH > 0 ? 0 : 1; }
CPP
  checked=$((checked + 1))
  if out=$("$CXX" -fsyntax-only -std=c++20 "${INCLUDES[@]}" \
             -DRESOURCES_PATH='"./resources/"' -DPRODUCTION_BUILD=0 \
             "$TMP/one.cpp" 2>&1); then
    echo "  ok    rmp/$name"
  else
    echo "  FAIL  rmp/$name — alone, it does not compile or does not reach the config"
    echo "$out" | head -5 | sed 's/^/          /'
    fails=$((fails + 1))
  fi
done

if [ "$fails" -ne 0 ]; then
  echo
  echo "FALLA: $fails header(s). Every public header must compile on its own AND"
  echo "       include <rmp/config.h> — that include is the one exception to the"
  echo "       rule in rmp/app.h, and it is what lets the user never type it."
  exit 1
fi
echo "  ok    $checked public headers, each self-contained and carrying the config"
