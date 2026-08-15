# Known canary breakage

The canary builds against floating versions every Monday, so it goes red for
things we already know about and have decided not to chase yet. Without a
ledger, that red becomes background noise and the one week it means something
gets ignored along with the rest.

An entry here means: **seen, understood, not acting yet.** The canary still
runs the check and still reports it, but it is marked `known` and the autofix
agent leaves it alone.

## Format

One bullet per breakage, keyed exactly as `canary_triage.py` computes it:

```
<family>/<pin>-<observed-version>/<failing-step-slug>
```

The key names the *cause*, not the symptom, so the same breakage across
different weeks collapses to one entry.

- `` `apple/xcode-27.0/build-raylib-xcframework` `` — one-line reason, and what
  would have to happen for it to be worth fixing.

Delete an entry when the underlying cause is fixed. A stale entry silences a
real failure, which is worse than no ledger at all.

## Open

- `` `apple/xcode-99.9/select-xcode` `` — not real drift. This run
  (`workflow_dispatch`, not the Monday cron; triggered by @omardev29) passed
  `xcode_version: 99.9` on purpose — the `workflow_dispatch` input for
  `xcode_version` literally says "use a bogus one to test triage". `99.9`
  isn't an Xcode release; `Select Xcode` correctly failed before it ever got
  to echo `CANARY_OBSERVED`, so the report's "observed" value is just the
  bogus input value falling back through `canary_triage.py`'s
  "what we asked for" path, not something actually seen on the runner. On
  this runner the newest installed Xcode is 26.6.0 and `xcodebuild -version`
  reports 26.6 — the frozen pin (`xcode 26.6` in
  `thirdparty/FROZEN_VERSIONS.md`) already matches, so there is nothing to
  bump. Re-open only if this key shows up from an actual scheduled
  (`schedule`) canary run — that would mean a real Xcode 99.9 exists, which
  is not going to happen.
