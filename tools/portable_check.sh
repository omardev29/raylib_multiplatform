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
# tools/ and the Justfile are what a developer runs. _bsd.yml and _apple.yml are
# in here for the same reason from the other end: their steps execute ON a BSD
# and ON a Mac, so a GNU-only flag there fails on the runner rather than on
# somebody's laptop. Every other workflow declares a Linux runner and is
# entitled to GNU everything.
FILES=$(find tools Justfile .github/workflows/_bsd.yml .github/workflows/_apple.yml \
             -type f 2>/dev/null | grep -v 'portable_check.sh' | sort)

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

# The BSD jobs run their script through cross-platform-actions' cpa.sh shell,
# which carries it into the VM and re-parses it there. A NON-ASCII BYTE does not
# survive that: an em dash inside a double-quoted echo came back as
#
#   sh: 83: Syntax error: Unterminated quoted string
#
# with a line number belonging to the generated script, in a step whose actual
# job was to build and run the game. The block that had worked for months
# contained exactly zero non-ASCII characters, which is the kind of thing nobody
# notices until they add one.
# LC_ALL=C is load-bearing: in a UTF-8 locale a bracket range is collation
# order, not byte order, and [^ -~] then matches almost every line. With the C
# locale it is bytes, which is the question being asked.
non_ascii=$(awk '/shell: cpa.sh/,/^      - name: Package/' .github/workflows/_bsd.yml 2>/dev/null \
            | LC_ALL=C grep -n '[^ -~]' || true)
if [ -n "$non_ascii" ]; then
    echo "  FAIL  a non-ASCII character in the cpa.sh block of .github/workflows/_bsd.yml"
    printf '%s\n' "$non_ascii" | sed 's/^/          /'
    echo "        instead: plain ASCII. Two hyphens for a dash. It does not survive"
    echo "                 the trip into the VM, and the error names the wrong line."
    fails=$((fails + 1))
fi

if [ "$fails" -ne 0 ]; then
    echo
    echo "FALLA: $fails rule(s). These scripts are what a new contributor runs first,"
    echo "       and this framework ships for macOS and three BSDs — so they have to"
    echo "       work there. CI will not tell you: the Linux jobs have GNU everything."
    exit 1
fi
echo "  ok    the scripts avoid $((9)) GNU-only and bash-4 constructs"
