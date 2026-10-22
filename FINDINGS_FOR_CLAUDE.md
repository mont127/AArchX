# bl+4 continuation experiment — handoff

## Context

This branch starts from tier2 at `5537a07e`. The goal is to remove the taken
branch after a direct call: Ocerz currently ends the caller block at `bl` and
returns through a continuation block, so the hardware return lands at `bl+4`
and executes a `b` to that block.

The earlier investigation is in `/Users/devin/child-reports/5-bl4.md`. It
measured a ceiling of roughly 5% on call-heavy code and 2–2.5% on `str` using
a throwaway fixed-hole placement hack. That hack was rejected: it failed
`fcntl_locks` after fork and `jit_misc2` after arena flush/static-hole reuse.

## Current handoff state

Opus was asked to implement the production design in two phases:

1. Phase 1: place only hot continuations during normal translation, with full
   split-range/fault/retire/flush/fork invariants. Intended switch:
   `OCERZ_NO_CONT_PLACE=1`.
2. Phase 2: persist and replay placed pairs in tcache so warm runs retain the
   placement. Intended separate switch if useful.

The working tree contains an **uncommitted WIP** of phase 1 plus partial phase
2 work. It has not been fully gated, measured, committed, or pushed. Do not
assume it is correct. Review `git diff` before continuing.

The WIP currently touches `src/jit.c`, `src/tcache.c`, guest test build files,
three new guest tests (`cont_place`, `cont_fork`, `cont_spin`), their expected
outputs, and the guest/diff test runners. Some generated guest binaries are
present in the tree; do not commit generated `tests/unit/bin/*` artifacts.

## What was learned during this run

- Reserving a continuation after every call caused about **+12.7%** cold
  `python3 -c pass` time with tcache disabled. This is unacceptable.
- Lazy placement by itself did not remove that startup cost.
- Promotion-on-heat was then added experimentally: on `python3` startup it
  reduced reservations from about **11.6k to 17**. The current threshold was
  experimented with around 4096 returns; remeasure and choose deliberately.
- A phase-1 tree built and the targeted placement path exercised, but the full
  gates were interrupted/noisy. One no-JIT timeout (`datomic_counter-no-jit`)
  occurred while load was high; it was not attributed to this change.
- The VM had booted iOS simulators whose `diagnosticd`/`apsd` processes consumed
  roughly 2.4 cores and made benchmark results invalid. They were shut down
  with `xcrun simctl shutdown all`; wait for load average below 8 before timing.
- No x86_64 OpenSSL binary was available on this VM, so that workload was not
  measured.

## Immediate next steps

1. Inspect and clean the WIP. Confirm whether the phase-1 implementation is
   complete; do not continue phase 2 until the phase-1 diff is coherent.
2. Run `make -j12 ocerz` and targeted guest tests first. Run with tcache off and
   on. Then run the full gates at low load, comparing every failure with the
   baseline.
3. Measure call/fib and `str` interleaved against Rosetta and the kill switch.
   Also measure cold `python3 -c pass`, warm Python/perl/gzip/bzip2, and cache
   off to detect reservation/I-cache regressions. Use medians, not one run.
4. Only keep the change if it is correct and gives a real call win without a
   material startup regression. If phase 1 does not clear roughly 2% on call,
   revert it and document why.
5. Phase 2 (tcache pair records/replay) is optional and should be implemented
   only after phase 1 is independently correct and worthwhile.
6. If abandoning the experiment, reset the WIP files and retain this handoff
   document on the branch so the result is discoverable.

## Baseline and known failures

The tier2 combined baseline passed all substantive gates. Known failures are:

- `test_apidb` fixture/API database mismatch on this VM.
- `test_jit_order_transition` also fails at `4156d0f5` and `1d2057b`.
- m32: `audio`, `audioqueue`, `glarb`, `glmirror` (same as baseline).
- dynamic: the known `ddlopen_cryptex`, `dyldslots`, `dmetal_nocopy_low`, and
  `dtest_jcc_gap_low` cases.

Anything else is a regression until explained.
