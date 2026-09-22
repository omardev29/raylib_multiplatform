#!/usr/bin/env bash
# Fail if a public header has grown past its budget.
#
# The work is in tools/header_cost.py: it preprocesses a translation unit that
# includes only that header and counts the lines, against the budget in
# tools/header_budget.txt that was written inside the pinned build image. The
# count is deterministic for a toolchain; milliseconds are not, so they are
# reported and never gated. Outside the image this reports and exits 0.
#
# Usage:  tools/header_cost_check.sh          (from the repo root)
# Exit:   0 = within budget (or not in the image), 1 = a header grew or a
#         budget went stale, named

set -uo pipefail
cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

python3 tools/header_cost.py --check
