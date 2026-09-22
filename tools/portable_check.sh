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
#
# A rule may also carry an EXCEPTION pattern: the spelling that is correct and
# happens to contain the bad one. Without it a rule ends up rejecting the very
# form its own message recommends, which is what `sed -i [^.]` did -- it
# matched the quote of the portable `sed -i '' -e ...` and told you to use
# `sed -i ''`. Nothing in the tree used sed -i, so it never fired and nobody
# found out.
matches() {
    local pattern="$1" unless="$2"
    shift 2
    # Comment lines are dropped: naming a construct in a note that explains why
    # it is not used is not using it, and the alternative is that the
    # explanation cannot be written down next to the code it explains.
    local hits
    hits=$(grep -nE "$pattern" "$@" 2>/dev/null | grep -vE ':[0-9]+: *#') || true
    if [ -n "$unless" ]; then
        hits=$(printf '%s\n' "$hits" | grep -vE "$unless") || true
    fi
    printf '%s' "$hits"
}

check_unless() {
    local pattern="$1" unless="$2" what="$3" instead="$4"
    local hits
    # shellcheck disable=SC2086  # $FILES is a deliberately word-split list
    hits=$(matches "$pattern" "$unless" $FILES)
    if [ -n "$hits" ]; then
        echo "  FAIL  $what"
        printf '%s\n' "$hits" | sed 's/^/          /'
        echo "        instead: $instead"
        return 1
    fi
    return 0
}

check() {
    check_unless "$1" "" "$2" "$3"
}

fails=0

# The GNU-only spelling, and the portable one that contains it.
SED_BAD='sed +-i'
SED_OK="sed +-i +(''|\"\")"

# --- the checker checks itself ---------------------------------------------
#
# "A test you have not seen go red is not a test, it is a hope with a name",
# and it applies to the checkers too. The sed rule above had been in the tree
# for months rejecting the exact spelling it recommends, and nothing noticed,
# because no file in tools/ uses sed -i at all -- a rule that never fires is
# indistinguishable from a rule that works.
#
# So: one line that must be flagged and one that must not, every run. It costs
# a temp file and two greps.
selftest() {
    local tmp bad good
    tmp=$(mktemp) || return 1
    # shellcheck disable=SC2064  # $tmp is wanted now, not at trap time
    trap "rm -f '$tmp'" EXIT
    {
        printf "%s\n" "sed -i '' -e 's/a/b/' file"
        printf "%s\n" "sed -i \"\" -e 's/a/b/' file"
        printf "%s\n" "sed -i 's/a/b/' file"
        printf "%s\n" "sed -i -e 's/a/b/' file"
    } > "$tmp"
    good=$(matches "$SED_BAD" "$SED_OK" "$tmp" | grep -cE '^[0-9]+:|:[0-9]+:' || true)
    bad=$(grep -cE "$SED_BAD" "$tmp" || true)
    if [ "$bad" -ne 4 ] || [ "$good" -ne 2 ]; then
        echo "  FAIL  the sed -i rule does not do what it says:"
        echo "          4 lines contain 'sed -i', the rule saw $bad"
        echo "          2 of them are the GNU-only spelling, the rule flagged $good"
        return 1
    fi
    rm -f "$tmp"
    trap - EXIT
    return 0
}
selftest || fails=$((fails + 1))

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
check_unless "$SED_BAD" "$SED_OK" \
      "sed -i needs an empty argument on BSD and macOS: sed -i ''" \
      "sed -i '' -e 's/a/b/' file, a temp file and mv, or python3" || fails=$((fails + 1))

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

# Every tools/*.sh a script or the Justfile NAMES has to exist. This is the
# same family of problem as the rules above -- an instruction that does not
# work on the machine reading it -- and it had a live instance:
# tools/glibc_check.sh's failure message said "Build it with
# tools/zig_toolchain.sh", a file that has never existed under that name, and
# it said so at the exact moment somebody needed the instruction to be right.
# shellcheck disable=SC2086  # $FILES is a deliberately word-split list
missing_scripts=$(grep -ohE 'tools/[a-z0-9_]+\.sh' $FILES 2>/dev/null | sort -u \
                  | while IFS= read -r ref; do
                        if [ ! -f "$ref" ]; then echo "$ref"; fi
                    done)
if [ -n "$missing_scripts" ]; then
    echo "  FAIL  a script that does not exist is named in tools/ or the Justfile:"
    printf '%s\n' "$missing_scripts" | sed 's/^/          /'
    echo "        instead: name the file that is there, or add the one that is not."
    fails=$((fails + 1))
fi

# The other property of that block that has broken CI, and the one nothing was
# measuring: its LENGTH. cross-platform-actions carries the script into the VM
# through cpa.sh and cuts it off somewhere between 4.8 KB (worked) and 5.6 KB
# (did not), and both cuts were silent in their own way -- one landed inside a
# quoted string and came back as "Unterminated quoted string" on an unplaceable
# line, the other landed somewhere that parsed, so the shell reached EOF and
# EXITED 0 with half the step unrun. 4096 is a deliberate margin under the
# smallest size known to have worked. The fix when this fires is not to golf
# the script: move the work into a committed file the way tools/render_check.sh
# did, and leave one line behind.
BSD_LIMIT=4096
bsd_bytes=$(LC_ALL=C awk '
    /^      - / { step_shell = ""; if (inrun) { if (total > max) max = total; inrun = 0 } }
    /^        shell:/ { step_shell = $2 }
    /^        run: \|/ { if (step_shell == "") { inrun = 1; total = 0 } ; next }
    inrun {
        if ($0 !~ /^[ \t]*$/ && $0 !~ /^          /) {
            if (total > max) max = total; inrun = 0; next
        }
        n = length($0); if (n >= 10) n = n - 10
        total += n + 1
    }
    END { if (inrun && total > max) max = total; print max + 0 }
' .github/workflows/_bsd.yml)
if [ "${bsd_bytes:-0}" -eq 0 ]; then
    echo "  FAIL  no cpa.sh \`run:\` block found in .github/workflows/_bsd.yml"
    echo "        Either the file changed shape or this rule stopped matching it."
    echo "        A size rule that measures nothing is the same as no size rule."
    fails=$((fails + 1))
elif [ "$bsd_bytes" -gt "$BSD_LIMIT" ]; then
    echo "  FAIL  the cpa.sh script in .github/workflows/_bsd.yml is $bsd_bytes bytes"
    echo "        (limit $BSD_LIMIT). 4.8 KB worked once and 5.6 KB was truncated,"
    echo "        silently, twice -- once exiting 0 having run half of itself."
    echo "        instead: move the work into tools/<something>.sh, the way"
    echo "                 tools/render_check.sh did, and call it in one line."
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
echo "  ok    the cpa.sh script in _bsd.yml is ${bsd_bytes} bytes (limit ${BSD_LIMIT})"
