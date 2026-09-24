## What is now true

<!-- One or two sentences: the rule this change makes true, as in the commit subject (`area: what is now true`). -->

## Why

<!-- The symptom, what was measured, the mechanism, and the fix. -->

## Test

<!-- The test that fails without this change and passes with it, and where it is registered. -->

## Checklist

- [ ] A test pins the change: it fails before and passes after, with expectations taken from a real run under Rosetta or natively.
- [ ] `make check` is green on my machine (it runs `make apis` first).
- [ ] The rebuilt `tests/unit/bin` binaries are committed separately as `tests: rebuild unit binaries`.
- [ ] No inline comments; the prose block at the top of each changed file is updated instead.
- [ ] The README and `docs/` are still true after this change.
- [ ] Every commit is signed off (`git commit -s`) under the Developer Certificate of Origin.
- [ ] Nothing here comes from disassembling, decompiling or otherwise reading Apple's proprietary binaries. Behaviour was found by running programs, public documentation and headers, or Apple's open-source releases.
- [ ] No files derived from the macOS SDK (such as `runtime/apis`) are added.
