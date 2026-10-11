<!--
Every section is filled in, or says why it does not apply. This text is what
Omar reads before the diff: write what happened, not what should have.
CLAUDE.md, "How an agent works here", is the process; this is its record.
-->

## What changed, and why

<!--
One paragraph: what was wrong or missing, what this does about it, and why
this way. `Fixes #N` for the issue it closes -- an issue labelled
`nightly-finding` is taken before anything else.
-->

## The ladder

<!--
Each stage you ran, where, and what it said. Where is one of: the laptop (the
machine or the cloud session you worked in), or the pinned image (podman or
docker run --init on the digest in thirdparty/FROZEN_VERSIONS.md, or
tools/dev_shell.sh). Some gates are real only in the image -- coverage, the
header budget, the e2e goldens -- and a pass elsewhere is not one of theirs.
-->

| Stage | Where | Result |
|---|---|---|
| `rmp fmt check` | | |
| `rmp lint` | | |
| `rmp test` | | |

## Seen red first

<!--
Every test or gate this adds or changes, and the mistake it was seen red on:
the change that made it fail, and the line it printed. A test that has not
been seen red is not a test yet.
-->

## Not verified

<!--
What you could not run, and why: no image, no GPU, a platform only CI builds,
a run you did not wait for. "Nothing" only when it is true.
-->

## Checklist

- [ ] `rmp fmt check` is clean.
- [ ] `rmp lint` is clean: no new warning, and a `NOLINT` only with its reason.
- [ ] `rmp test` passes.
- [ ] `rmp test examples` passes -- when the public API, an example, a scene, the input or the UI changed.
- [ ] `rmp test sanitize` passes -- when something that parses, casts or frees changed.
- [ ] `rmp test fuzz` passes -- when a parser changed.
- [ ] `rmp test coverage` passes in the pinned image -- when `src/rmp` changed; a new file has its floor line.
- [ ] The docs say what the code now does: when a page of rmp-docs describes what changed, rmp-docs changes with it and its `FRAMEWORK_REF` names this commit.
- [ ] An API change -- any change to `include/`, a key or a default of the `.toml` -- is labelled `api-change` by the `api` job and waits for Omar's `api-approved`. The author never adds or removes that label.
- [ ] No gate was weakened to get green: no floor lowered, no ratchet list grown, no test skipped or deleted, no tolerance added.
- [ ] No secret anywhere: not in the `.toml`, a workflow, a log or this text.
- [ ] Commits are plain, in Omar's name: no `Co-Authored-By`, no trailers.
