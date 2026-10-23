# bl+4 continuation experiment — handoff

## Context

This branch starts from tier2 at `5537a07e`. The goal is to remove the taken
branch after a direct call: Ocerz ends the caller block at `bl` and returns
through a continuation block, so the hardware return lands at `bl+4` and
executes a `b` to that block.

The earlier investigation measured a ceiling of roughly 5% on call-heavy code
and 2–2.5% on `str` using a throwaway fixed-hole placement hack. That hack was
rejected: it failed `fcntl_locks` after fork and `jit_misc2` after arena
flush/static-hole reuse.

## Status (2026-10-23)

Phase 1 (place hot continuations during normal translation,
`OCERZ_NO_CONT_PLACE=1` to turn it off) is complete, gated and measured, and
worth keeping. Phase 2 is not needed for anything measured. Nesting was tried
and dropped. Nothing is pushed.

Commits on top of the WIP (`38211eba`, `af069598`):

- `jit: a placed copy is told from its caller without reading the layout its
  caller frees` — the caller frees the shared `JitLayout` in `block_destroy`,
  and `ocerz_jit_destroy` frees blocks in no particular order, so a copy
  destroyed after its caller read freed memory (`lay->caller`). A copy is the
  only block with prologue words, so `pro_w` now decides.
- `tests: build the continuation placement tests` — `run_guest_tests.sh`
  fails a test whose binary is missing and every other guest binary is
  tracked, so a fresh checkout failed `cont_place`, `cont_fork`, `cont_spin`.

## Gate (MacBook Air M5, macOS 27, low load)

`make check` and `make check-m32` both pass with no known failures:

- unit: all pass, `test_apidb` included once `make apis32` has generated
  `runtime/apis` (a fresh worktree has none, which is what the VM's
  `test_apidb` failure was);
- guest 142/142 under both engines, including every `cont_*` variant
  (ordered, placed at once, tiny arena);
- differential 103/103, diff32 40,044/40,044 in each layout;
- dynamic 410/410 (the VM's `ddlopen_cryptex`, `dyldslots`,
  `dmetal_nocopy_low`, `dtest_jcc_gap_low` failures do not occur here);
- native 87/87 and the native C++, framework and format suites;
- m32 77 plus the two speed-knob reruns, `steambridge` skipped as always
  (the VM's `audio`, `audioqueue`, `glarb`, `glmirror` failures do not occur
  here).

## Measurements

Placement on against `OCERZ_NO_CONT_PLACE=1` on the same build, interleaved,
medians of 9; two-point workloads report the time per unit from the
difference of a large and a small run.

| workload | on | off | on/off | Rosetta |
|---|---|---|---|---|
| fibn (per node) | 0.569 ns | 0.619 ns | 0.919 | 0.556 ns |
| xbench str | 6.62 ns | 6.76 ns | 0.979 | 6.54 ns |
| xbench leafcall | 1.50 ns | 1.49 ns | 1.004 | 3.03 ns |
| xbench icall | 7.21 ns | 7.18 ns | 1.005 | 7.88 ns |
| perl startup, cache off | 274 ms | 277 ms | 0.990 | |
| perl startup, cache warm | 170 ms | 170 ms | 1.002 | |
| gzip -6, 18 MB | 493 ms | 493 ms | 1.000 | |
| bzip2, 18 MB | 950 ms | 950 ms | 0.999 | |

A second run gave str 0.951 and fibn 0.911. There is no x86_64 python3 on
this Mac (the Command Line Tools' one is arm64 only), so the cold python3
start was replaced by perl's (`perl -MData::Dumper -MFile::Temp`).

Promotion threshold (`OCERZ_CONT_PROMOTE`), perl startup with the cache off,
on/off: 256 → 1.047, 1024 → 1.032, 4096 → 1.015, 16384 → 1.012; a perl loop
calling a sub 1.5M times did not move at any of them (its hot calls are
indirect). Low thresholds cost startup; 4096 and 16384 are within noise, so
4096 stays.

Warm runs: callers made hot in a run are retired and translated again, not
loaded from the translation cache, so they are placed with the cache on as
well (fibn 2 placements, perl 28, in every run). Phase 2 (persisting placed
pairs) would only matter for callers the cache keeps, which `cont_reserve`
never counts or places; nothing measured here needed it.

## Nesting: tried, dropped

A copy cannot reserve for its own call, so in a block that calls twice in a
row the second call's return still lands on a branch. A prototype let a copy
at depth 0 own a reservation inside its own (`lay` and `own` per block, three
code ranges, a two-level walk in `fault_block`, no replacement over a copy
that owns a placed copy, promotion of a copy only where it can be placed again
at once, outer reservations sized for the inner copy). It passed the
`cont_*` tests and a new `nest` shape in `cont_place` (fault and flip inside
the inner copy), but it bought nothing:

| workload | nest | no nest | placement off |
|---|---|---|---|
| tree2: `t(n-1)`, `t(n-2)` not accumulated, per node | 1.100 ns | 1.097 ns | 1.088 ns |
| fibn | 0.573 ns | 0.561 ns | 0.629 ns |

clang turns fib's second call into a loop, so fibn has one return point and
was fully placed already; tree2, which has two, gains nothing from placement
at either depth. Small callees, leaf or not, are spliced and never reach
placement at all. The prototype diff is kept outside the repository.

## Baseline and known failures

On the VM the tier2 baseline had these known failures; none occur on the M5:

- `test_apidb` fixture/API database mismatch (no `runtime/apis`).
- `test_jit_order_transition` also failing at `4156d0f5` and `1d2057b`.
- m32: `audio`, `audioqueue`, `glarb`, `glmirror`.
- dynamic: `ddlopen_cryptex`, `dyldslots`, `dmetal_nocopy_low`,
  `dtest_jcc_gap_low`.
