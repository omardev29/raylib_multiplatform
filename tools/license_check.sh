#!/usr/bin/env bash
# Fail if a vendored component has a licence we cannot ship, no licence at all,
# or an alteration nobody marked.
#
# The work is in tools/license_db.py (standard library, no network, no
# compiler): it walks thirdparty/ to the depths where components live, reads
# the machine-readable block of THIRD_PARTY_LICENSES.md, fingerprints every
# licence text, and applies the rules written at the top of that file. This
# wrapper exists so the gate has the same shape as the other *_check.sh
# scripts: one line in `just test`, one step in the lint job, exit code 0 or 1.
#
# Usage:  tools/license_check.sh          (from the repo root)
# Exit:   0 = every component known and marked, 1 = a problem, named

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 tools/license_db.py --check
