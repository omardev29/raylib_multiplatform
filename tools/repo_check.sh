#!/usr/bin/env bash
# The repository tracks sources, not what a build produces.
#
# This exists because 78 files of a CMake build directory -- CMakeCache.txt with
# absolute paths from one laptop, .ninja_deps, object files, a 20 MB tree --
# were committed by a `git add -A` and sat there unnoticed through several
# sessions, along with a log file that tools/render_check.sh rewrites on every
# run. Neither was caught by anything, because .gitignore only listed `build/`
# and the directory happened to be called `build-memory/`.
#
# A .gitignore stops files being added; it does nothing about files already
# tracked, and it only knows the names somebody thought of. This checks the
# other direction: whatever the ignore rules say, does the index contain
# something that a build made? Cheap enough to run in `just test`.
#
# ASCII only and no GNU-only constructs: tools/portable_check.sh covers this
# directory, and macOS ships bash 3.2.
set -euo pipefail

cd "$(dirname "$0")/.."

# Signatures of generated files, as grep -E patterns against tracked paths.
# thirdparty/ is vendored source and is checked too: a vendored build directory
# is exactly as wrong as one of ours.
PATTERNS='(^|/)CMakeCache\.txt$
(^|/)CMakeFiles/
(^|/)cmake_install\.cmake$
(^|/)build\.ninja$
(^|/)\.ninja_(log|deps)$
(^|/)compile_commands\.json$
\.log$
\.(o|obj|a|lib|so|so\.[0-9]|dylib|dll|exe|pdb|ilk|exp)$
(^|/)core$
(^|/)\.DS_Store$
(^|/)Thumbs\.db$'

FOUND=0
while IFS= read -r pat; do
  [ -n "$pat" ] || continue
  # git ls-files is the index, which is the only thing that matters here.
  HITS=$(git ls-files | grep -E "$pat" || true)
  if [ -n "$HITS" ]; then
    if [ "$FOUND" -eq 0 ]; then
      echo "FALLA: the repository tracks files that a build produces."
      echo "       Take them out with \`git rm -r --cached <path>\` and add the"
      echo "       name to .gitignore. If one of these is genuinely a source"
      echo "       file, narrow the pattern in tools/repo_check.sh and say why."
      echo
    fi
    echo "  pattern $pat"
    echo "$HITS" | sed 's/^/    /'
    FOUND=1
  fi
done <<EOF
$PATTERNS
EOF

[ "$FOUND" -eq 0 ] || exit 1

# The second half: a tracked file that .gitignore also claims to ignore. That
# combination is always a mistake -- either it was added before the rule existed
# (and git keeps tracking it, ignore rules do not apply to tracked files), or
# the rule is wrong. Both need a person, and neither announces itself.
IGNORED_BUT_TRACKED=$(git ls-files --cached --ignored --exclude-standard || true)
if [ -n "$IGNORED_BUT_TRACKED" ]; then
  echo "FALLA: these files are tracked AND matched by .gitignore."
  echo "       Ignore rules do not apply to files git already tracks, so this"
  echo "       is either a leftover \`git add\` or a wrong rule."
  echo "$IGNORED_BUT_TRACKED" | sed 's/^/    /'
  exit 1
fi

echo "  ok    the index holds no build output ($(git ls-files | wc -l | tr -d ' ') files tracked)"
