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

_(none yet — the canary has not found anything)_
