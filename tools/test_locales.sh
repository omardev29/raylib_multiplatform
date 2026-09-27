#!/usr/bin/env bash
# Build the test locales in tests/fixtures/locale/ into <dir>, for LOCPATH.
#
# tests/save_test.cpp checks that saves read and write the same under a locale
# whose decimal point is not '.', and the CI build image has no locale but C --
# so the test skipped there, and the fix it guards ran only on machines that
# happened to have a German or Spanish locale installed. These two are built
# from source: a comma, and U+066B, two bytes in UTF-8, which is what cJSON's
# own ENABLE_LOCALES got wrong. Each defines LC_NUMERIC and nothing else.
#
# Needs glibc's localedef. Where there is none (macOS) this builds nothing and
# exits 0, and the tests skip -- unless RMP_REQUIRE_TEST_LOCALES=1, which the
# CI lint job sets, turns the skip into a failure.
#
# Usage:  tools/test_locales.sh <dir>        then  LOCPATH=<dir> ./build/unit_test

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

out=${1:?usage: tools/test_locales.sh <dir>}
if ! command -v localedef >/dev/null 2>&1; then
  exit 0
fi
mkdir -p "$out"
for name in rmp_comma rmp_twobyte; do
  # -c: the sources define LC_NUMERIC only, which localedef warns about and
  # exits non-zero for even though the locale it writes is complete.
  localedef -c -i "tests/fixtures/locale/$name" -f tests/fixtures/locale/rmp.charmap \
    "$out/$name" >/dev/null 2>&1 || true
  if [ ! -f "$out/$name/LC_NUMERIC" ]; then
    echo "test_locales: could not build $name" >&2
  fi
done
exit 0
