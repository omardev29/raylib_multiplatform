#!/usr/bin/env bash
# Our own scripts have to run on a Mac and on the BSDs, not just on this Linux.
#
# WHY THIS EXISTS. `just test` is the thing a new contributor runs first, and it
# is made of shell. The failure mode is nasty because it is invisible from here:
# every one of these works perfectly on the machine it was written on and dies
# on somebody else's, with an error about a builtin or a flag rather than about
# the thing being checked.
#
# The one that was actually in the tree: `mapfile`. It is bash 4, and macOS
# ships bash 3.2 and always will — bash went GPLv3 and Apple stopped updating
# it. `just test config` would have died on `mapfile: command not found` on
# every Mac, and the framework builds for macOS.
#
# CI does not catch this. The Linux jobs run GNU everything, and the macOS job
# runs the workflow's own steps, not the Justfile.
#
# Usage: tools/portable_check.sh          (from the repo root)

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# What to look in: the things a developer runs, not the workflows. A workflow
# step declares the OS it runs on, so GNU flags in a Linux job are correct.
# This file is excluded: its rules necessarily CONTAIN the patterns they look
# for, and a checker that fails on its own definitions is a checker nobody keeps.
FILES=$(find tools Justfile -type f 2>/dev/null | grep -v 'portable_check.sh' | sort)

# Each rule is: a pattern, and what to do instead. The second half is the point
# — "do not use mapfile" without "use a while-read loop" is a rule people work
# around rather than follow.
check() {
    local pattern="$1" what="$2" instead="$3"
    local hits
    # Comment lines are dropped: naming a construct in a note that explains why
    # it is not used is not using it, and the alternative is that the
    # explanation cannot be written down next to the code it explains.
    hits=$(grep -nE "$pattern" $FILES 2>/dev/null | grep -vE ':[0-9]+: *#') || true
    if [ -n "$hits" ]; then
        echo "  FAIL  $what"
        printf '%s\n' "$hits" | sed 's/^/          /'
        echo "        instead: $instead"
        return 1
    fi
    return 0
}

fails=0
check 'mapfile|readarray' \
      "mapfile/readarray is bash 4; macOS ships bash 3.2" \
      "files=(); while IFS= read -r f; do files+=(\"\$f\"); done < <(...)" || fails=$((fails + 1))
check 'stat -c' \
      "stat -c is GNU; BSD and macOS use stat -f" \
      "wc -c < file" || fails=$((fails + 1))
check 'grep -P|grep [-a-zA-Z]*P[a-zA-Z]* ' \
      "grep -P is GNU only" \
      "grep -E, or awk" || fails=$((fails + 1))
check 'readlink -f' \
      "readlink -f is not in macOS's readlink" \
      "cd \"\$(dirname \"\$0\")\" && pwd" || fails=$((fails + 1))
check '\bnproc\b' \
      "nproc is GNU coreutils" \
      "getconf _NPROCESSORS_ONLN" || fails=$((fails + 1))
check 'xargs -r' \
      "xargs -r is GNU; BSD xargs already skips empty input" \
      "drop the -r" || fails=$((fails + 1))
check 'find [^|]*-printf' \
      "find -printf is GNU only" \
      "find ... -exec, or a while-read loop" || fails=$((fails + 1))
check 'date \+%s%N' \
      "date has no %N on BSD or macOS" \
      "seconds, or python3 -c 'import time; print(time.time_ns())'" || fails=$((fails + 1))
check 'sed -i [^.]' \
      "sed -i needs an argument on BSD: sed -i '' " \
      "a temp file and mv, or python3" || fails=$((fails + 1))

if [ "$fails" -ne 0 ]; then
    echo
    echo "FALLA: $fails rule(s). These scripts are what a new contributor runs first,"
    echo "       and this framework ships for macOS and three BSDs — so they have to"
    echo "       work there. CI will not tell you: the Linux jobs have GNU everything."
    exit 1
fi
echo "  ok    the scripts avoid $((9)) GNU-only and bash-4 constructs"
