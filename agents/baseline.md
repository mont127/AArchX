# Baseline: pristine C tree (97a9247) on this VM

Reference worktree: `~/AArchX-c` (detached at 97a9247, built with `make -j12 ocerz`
plus all unit binaries and `make apis`). macOS 26.6 arm64, SDK 26.5.

## Results

| phase | result |
|---|---|
| unit (each tests/unit/bin/*, OCERZ_NO_ARM_EXEC=1) | all pass except test_apidb (2 checks fail) |
| guest --no-jit | 134/134 pass |
| guest jit | 134/134 pass |
| diff | 100/100 pass |
| diff32 | pass (40044 checks, 0 failed) |
| dynamic | 280 passed, 7 failed |
| native (run_native_tests.sh) | 86 passed, 1 failed |
| guest library | pass |
| native cxx | SKIP (needs tools/build_guest_cxx.sh) |
| native frameworks | pass |
| native formats | pass |
| native swift | SKIP (needs make guest-swift) |

## Known failures on this VM

- `test_apidb` — 2 checks: cannot read `runtime/apis/macos/27.0/*.api`.
  The test hardcodes macOS 27.0; this VM runs macOS 26.6 / SDK 26.5, so the
  generated database lives under `macos/26.x`. Environment issue, not a bug.
- `test_bridge` and `test_vdylib` fail ONLY when `runtime/apis` has not been
  generated yet (bridge descriptors / vdylib database come from `make apis`).
  With apis generated both pass; `test_bridge` segfaulted at exit when run
  without apis — still environment, not code.
- dynamic, 7 failures: `ddlopen_image_list` (jit+no-jit, libxml2 already
  listed), `ddlopen_cryptex` (jit+no-jit, no cryptex-only library on this
  machine), `dyldslots` (slot 0x450 unanswered), `dmetal_nocopy_low`
  (jit+no-jit, 16384/16384 wrong). Marked KNOWN-PENDING in the suite output.
- native, 1 failure: `sys_proc` — the arm64 fixture fails its own checks on
  this host, so the native run can prove nothing either way.

The exact expected FAIL lines live in `agents/expected/*.fail`; the gate
(`tools/rust_gate.sh`) diffs new runs against them.
