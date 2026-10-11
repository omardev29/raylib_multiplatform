"""The Hypothesis settings every tests/*_property_test.py shares, and what
happens where Hypothesis is not installed.

    import hypothesis_profiles  # first: it skips the module, or fails it
    from hypothesis import given, strategies as st

Two profiles, chosen by RMP_HYPOTHESIS_PROFILE:

  ci        the default -- `rmp test property` and every push. Derandomized:
            the same 200 examples a test on every run and every machine, so a
            red is the commit's and not the seed's. No example database, no
            deadline (ASan and a loaded runner make timings meaningless), and
            the blob that reproduces a failure printed with it.
  nightly   .github/workflows/deep.yml: 5000 examples, random, and the
            database under build/hypothesis/ -- where a counterexample found
            once is tried first on the next run in the same checkout. The
            nightly job starts from a fresh one every time (nothing carries
            build/ from one run to the next); what it finds reaches the issue
            as the @reproduce_failure blob print_blob prints.

Without Hypothesis -- `pip install hypothesis`, the version in
thirdparty/FROZEN_VERSIONS.md -- a property test module says so and is
skipped, unless RMP_REQUIRE_HYPOTHESIS=1, which CI sets: the pinned image has
it, and a skip there would be a green that tested nothing.

Standard library and Hypothesis only.
"""

from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

try:
    import hypothesis
except ImportError:
    if os.environ.get("RMP_REQUIRE_HYPOTHESIS") == "1":
        raise
    # Said once a run, however many property modules import this.
    if not os.environ.get("RMP_HYPOTHESIS_SKIP_SAID"):
        os.environ["RMP_HYPOTHESIS_SKIP_SAID"] = "1"
        print("  skip  the Hypothesis property tests: hypothesis is not installed "
              "(pip install hypothesis; RMP_REQUIRE_HYPOTHESIS=1 makes this a failure)",
              file=sys.stderr)
    raise unittest.SkipTest("hypothesis is not installed") from None

# differing_executors is off in both: unittest makes a TestCase per test, and
# mutmut (tools/mutation.sh) runs the same suite twice in one process, so the
# same test meets a second `self` -- the check's whole trigger -- with nothing
# shared between the two.
_QUIET = [hypothesis.HealthCheck.differing_executors]

PROFILES = {
    "ci": dict(derandomize=True, max_examples=200, database=None, deadline=None,
               print_blob=True, suppress_health_check=_QUIET),
    # derandomize=False said, not left to the default: Hypothesis turns
    # derandomize on by itself where the CI variable is set -- every GitHub
    # runner -- so the nightly ran the same 5000 examples every night.
    "nightly": dict(derandomize=False, max_examples=5000, deadline=None,
                    print_blob=True, suppress_health_check=_QUIET,
                    database=hypothesis.database.DirectoryBasedExampleDatabase(
                        str(REPO / "build" / "hypothesis"))),
}

for _name, _settings in PROFILES.items():
    hypothesis.settings.register_profile(_name, **_settings)

PROFILE = os.environ.get("RMP_HYPOTHESIS_PROFILE", "ci")
if PROFILE not in PROFILES:
    raise SystemExit(f"RMP_HYPOTHESIS_PROFILE={PROFILE!r}: one of {', '.join(PROFILES)}")
hypothesis.settings.load_profile(PROFILE)
