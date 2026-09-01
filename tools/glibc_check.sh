#!/usr/bin/env bash
# The binary must not need a glibc newer than [linux] glibc says it may.
#
# WHY THIS IS A GATE AND NOT A NOTE. A Linux binary records, per imported
# symbol, the glibc version that symbol was introduced in. The loader on the
# target machine refuses to start anything asking for a version it does not
# have, and the message is `version GLIBC_2.38 not found`. Nothing about the
# build says this is coming: it compiles, it links, it runs on the machine that
# built it, and it fails on somebody else's.
#
# Building on ubuntu-24.04 means glibc 2.39, and 2.39 excludes Debian 12,
# Ubuntu 22.04, RHEL 8, and Steam's sniper runtime. That is most of Linux.
#
# What this project actually needs is much older -- every high symbol version in
# our binary is libm (powf, hypot, fmod) or the pthread/dl functions glibc 2.34
# folded into libc, not a single new API -- so the floor is a build setting, not
# a rewrite. See tools/zig_toolchain.sh.
#
# Usage: tools/glibc_check.sh <binary> [max-version]
#        max-version defaults to [linux] glibc from the .toml.

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

BINARY="${1:?usage: glibc_check.sh <binary> [max-version]}"
WANT="${2:-}"
if [ -z "$WANT" ]; then
  WANT=$(python3 tools/configure.py --print-glibc 2>/dev/null || true)
fi

[ -f "$BINARY" ] || { echo "FALLA: $BINARY does not exist"; exit 1; }

if [ -z "$WANT" ]; then
  echo "  skip  [linux] glibc is empty — built against the host, nothing to check"
  exit 0
fi

# objdump over readelf: readelf's output for version requirements is laid out
# differently between binutils versions, and objdump -T is one line per symbol
# on every one of them.
command -v objdump > /dev/null 2>&1 || {
  echo "  skip  objdump not installed"; exit 0; }

VERSIONS=$(objdump -T "$BINARY" 2>/dev/null | grep -oE "GLIBC_[0-9]+\.[0-9]+" | sort -uV)
if [ -z "$VERSIONS" ]; then
  echo "  ok    $BINARY imports no versioned glibc symbols at all (static?)"
  exit 0
fi

HIGHEST=$(printf '%s\n' "$VERSIONS" | tail -1 | sed 's/GLIBC_//')

# sort -V puts 2.9 before 2.10, which plain string or float comparison does not.
NEWER=$(printf '%s\n%s\n' "$WANT" "$HIGHEST" | sort -V | tail -1)
if [ "$NEWER" = "$WANT" ] || [ "$HIGHEST" = "$WANT" ]; then
  echo "  ok    needs glibc $HIGHEST, floor is $WANT"
  exit 0
fi

echo "  FAIL  $BINARY needs glibc $HIGHEST, and [linux] glibc says $WANT"
echo
echo "        The symbols asking for it:"
# Version-aware, not string-aware: "GLIBC_2.9" > "GLIBC_2.38" as strings, which
# would print the wrong list and look like a bug in the check rather than in the
# comparison. sort -V knows; awk does not.
# objdump prints the version in parentheses -- "(GLIBC_2.43) asinf" -- so the
# two fields to take are the last two, with the brackets stripped.
objdump -T "$BINARY" 2>/dev/null | grep "(GLIBC_" \
  | awk '{ v = $(NF-1); gsub(/[()]/, "", v); print v, $NF }' | sort -u \
  | while read -r ver sym; do
      top=$(printf '%s\n%s\n' "$WANT" "${ver#GLIBC_}" | sort -V | tail -1)
      if [ "$top" != "$WANT" ]; then printf '          %-14s %s\n' "$ver" "$sym"; fi
    done | sort -uV | head -20
echo
echo "        This binary will not start on anything older, with"
echo "        \"version GLIBC_$HIGHEST not found\" and nothing else."
echo "        Build it with tools/zig_toolchain.sh, or lower the floor."
exit 1
