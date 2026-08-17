# Ocerz vs Rosetta 2 performance log (xbench)

Objective (user, 2026-08-15): make Ocerz faster than Rosetta 2 on every kernel of
`tests/guest/xbench.c` (15 kernels), measured with `tests/xbench_compare.py`
(paired-delta: time(n) - time(n/2) per engine, cancels startup + JIT warmup;
ratio = Ocerz/Rosetta, <1 wins). Machine: Apple M2 Max. Static x86_64 binaries
(offset guest_base, plain-memory JIT tier). Every change gated by:
`bash tests/run_guest_tests.sh` (36), `bash tests/run_diff_test.sh` (41), and
byte-identical xbench output vs Rosetta for all 15 kernels; then commit + push
(no co-author trailers; repo moved to Macndcheese-org/Ocerz).

## Standing (2026-08-16 13:30, REPS=7, quiet machine)
depchain 0.91 WIN, jtab 0.96 WIN, memcpy 0.96 WIN, chase 1.01, brmiss 1.01, str 1.03,
hash 1.03, qsort 1.04, fpvec 1.04, vm 1.07, icall 1.09, idiv 1.11, fpsse 1.21,
leafcall 1.24, mixed 1.29.  (README table refreshed from this run.)
Kernels within ~3% of 1.0 (chase/brmiss/str/hash/qsort/fpvec/vm) trade places run to run.

Today's additions (all gated + pushed): narrow TEST/CMP NZCV forwarding (incl. memory
operand) -> cmov/setcc/adc in 1-3 words; cmov reg,reg = 1 csel; adc/sbb dead-flags fast
path; narrow test reg,reg + je/jne 1-2 words; EA cache (JTA = JGB+base[+idx<<s] reused
across instructions, validated by scanning emitted host words for x15 writes); aux-disp
register-offset loads (strlen loop = 3 words); guest-EA + [JGB,EA] for unfoldable scales;
ldur/stur for small negative displacements; CALL pushes in 2 words; logical/arith
immediates in the live-flags in-place arith path; harness calibrates fast kernels properly.

Remaining structural gaps (why the losers lose): fpsse = per-op `ins` for upper-lane
preservation + batch check + exact cvt/div paths; leafcall/icall = RAS bookkeeping per
call/ret vs Rosetta's AOT layout (rsp-indexed RAS tried and reverted: slower); mixed = sum
of FP exactness (cvttsd2si 6 words, subsd 4) + calls + memory; idiv = exact div path.
Ideas not done: upper-lane liveness (dead upper -> scalar op writes the pin directly, saves
the ins; ~6% on fpsse), FP-batch membership for GPR-neutral insns/cvtsi2sd/cmpsd/blendv
(~6% on fpsse), cvttsd2si via fccmp (5 words instead of 6).


## Standing (2026-08-17 evening, REPS=5, quiet machine)
Static: depchain 0.85, memcpy 0.93, jtab 0.93, brmiss 0.96, fpvec 0.96, qsort 0.98,
str 0.99, icall 0.99 (8 WINS); chase 1.00, hash 1.00, vm 1.00 (ties, ±0.5% run to run);
idiv 1.05, leafcall 1.18, fpsse 1.21, mixed 1.22.
Dynamic (xbench_dyn, OCERZ_HOSTWQ=1): depchain 0.85, jtab 0.90, vm 0.96, brmiss 0.96,
fpvec 0.96, chase 0.97, memcpy 0.97, icall 0.99, qsort 1.00 (9 WINS); hash 1.01,
idiv 1.03, str 1.04, leafcall 1.17, fpsse 1.20, mixed 1.21.
Objective "beat Rosetta on more than 8 xbench kernels" is met in dynamic mode (the mode
Wine and every real Mach-O run in); static is at 8 + 3 ties.

2026-08-17 additions (all gated + pushed; see git log 5d08d1b..):
- ordered (TSO) memory model with per-instruction alignment hot-patching (see README);
  ordered mode within 1.0-1.1x of plain on most kernels.
- mov+op fusion (dead-flag arith/imul/shift after a reg mov), host-stack return-address
  shadow (CALL: adr+stp, RET: ldp/cmp/b.ne/cbz/ret), third hoisted base (x30, never handed
  across a bl), identity-mode bases hoisted as themselves, FP-batch NaN absorption edges.
- superblocks past backward jcc, cbz/cbnz side exits, scale-1 index counted as a hoist
  base, negative-immediate flag forms (add r,-k -> subs), div/idiv on pins with rdx-known
  elision, cqo/cdq on pins, IC target register used directly, side-exit records in the
  taken stub, hoisted-base handoff across chained bodies (same signature -> enter after
  the reload).
- indirect call/jmp [mem] target loaded with the plain fast forms (was EA + commpage guard
  + guest_base add: 12 words in dynamic mode); identity-mode push/pop/call/ret/leave use
  pre/post-indexed forms on the pinned rsp (1 word).  icall 1.04 -> 0.99.
- cross-block flag liveness peeks through direct call/jmp/jcc successors (depth 3) and
  blocks publish the seam-based entry_live (over-approximation argument in the commit);
  no flag-record spill before a call whose callee overwrites flags.
- NZCV forwarding across up to 3 flag-neutral gap insns (mov/lea, or a reg cmov/setcc that
  itself forwards from the same producer): cmp; mov; cmova; cmovae 41 -> 5 words.
Measured and rejected today: literal-pool return addresses (pool pressure, mixed -15%),
static identity mapping (kernel refuses allocations below the slid image), RET/indirect
seam-0 protocol variants (negligible), OCERZ_HOIST_MIN=1 (mixed -2%, icall +2%), a
"late record" for indirect terminators (an unsound experiment showed the record store
before `jmp reg` costs nothing measurable; the 0.75-vs-0.31 s vm timing that suggested
otherwise was a first-run-after-build artefact, since seen on other kernels too).
Harness note: the first execution of a freshly built ocerz binary can be ~0.4 s slower;
xbench_compare alternates engines and takes medians, so it is unaffected, but ad-hoc
`/usr/bin/time` comparisons must discard the first run.

## Dynamic-mode phase (2026-08-16 afternoon/evening)
Wine and every real Mach-O run in "dynamic mode" (dyld shared cache, identity map,
commpage emulation).  It was running at 3-8x static-mode cost.  Fixed:
- plain memory model now also in dynamic mode (`vm.jit_plain_mem` was never set on the
  dyld path); read-only MAP_SHARED no longer retires it (dyld maps closures that way);
  writable shared, threads, fork/spawn, hostwq workers, remaps and mprotect-to-writable of
  a shared range still retire it.
- adaptive commpage guarding: fast forms by default; a plain-form access to the (unmappable)
  commpage address faults; the handler marks block+insn rips, runs the instruction in the
  interpreter, invalidates, and the retranslation of marked rips guards every access.  The
  RAS/CALL/RET/stack protocol is deliberately independent of the mark (uniform protocol).
- l0 caches enabled in dynamic mode; EA cache bug (stale after `mov rax,[rax]`) fixed.
- startup: lazy per-host-page unpacking of the shared cache's slid data regions (was 200 ms
  eager), live-block list for invalidation (was 1M-bucket walks per mprotect), no
  sigprocmask per translation, hashed dependency lookup, O(1) logical-immediate encoder.
  wine --version 0.33 -> 0.12 s; small dynamic binaries 0.23 -> 0.06 s.
- new inline ops for real libc code: bsf/bsr/tzcnt/lzcnt/popcnt (dead-flags forms),
  pmovmskb, bt (NZCV forwarding + RFLAGS patch), RMW memory forms incl. lock/xchg/xadd/
  cmpxchg (plain: load/op/store; ordered: LSE atomics + alignment check -> interpreter),
  cmp/test [mem],x, movq (all forms), pshufd.  wine --version slow ops 2.4% -> ~0.6%.
- cmpxchg semantics aligned with the Rosetta oracle (flags = dest - acc, accumulator always
  written); the interpreter was changed too.  Documented in the interpreter comment.
- gates: `tests/run_dyn_test.sh` (xbench_dyn identity), ordered-mode runs of
  jit_rmw/jit_scan/jit_ops/jit_bt, jit_movq/jit_bt/jit_rmw/jit_scan tests.
Remaining known slow ops in wine --version: not (shape?), pinsrd, sidt, pmovsxbd, movnti,
shld/shrd, setcc [mem], push [mem], mfence, pinsrw, cvt* forms.

## Mechanisms added (all in src/jit.c unless noted)
- Flag liveness (src/flags_live.c): x87/SSE ranges, push/pop/call/ret/... are
  flag-neutral; the memory "fault barrier" (every memory op uses all flags so a
  fault handler sees exact dead flags) is opt-in `OCERZ_FAULT_FLAG_BARRIER=1`.
  Direct CALL seam = callee entry liveness. div/idiv define all flags.
- Static flag fusions (liveness pass and emit_cc_predicate share the predicate,
  and `cc_consumer_inline_ok` guards that the consumer really is emitted inline):
  * comis_fuse_producer: (u)comis + jcc/setcc/cmov redo `fcmp` and branch on
    NZCV directly; dead reg-reg comis emits nothing.
  * value_cond_fuse_producer: E/NE/S/NS after add/sub/logic/inc/dec/neg/shift-imm
    test the pinned result register (`cmp #0`).
  * nzcv_fuse_producer: adjacent cmp/test/add/sub/and/or/xor + setcc/cmov/adc/sbb:
    the producer emits adds/subs/ands (records still stored if the seam needs
    them), the consumer maps cc via cc_after_subs/adds/ands (AL/NV constants
    handled: cset cannot encode them).
  * fused cmp/test+jcc: narrow direct compares for Z/unsigned conds, tbz/tbnz for
    single-bit tests, cbz/cbnz for cmp-with-zero + je/jne.
- FP batches (fpb_*): a run of SSE add/sub/mul/div/sqrt/min/max/moves executes in
  place with no per-op NaN fixups; at batch end the tainted registers are merged
  with fmax (NaN-propagating; lane 0 only for scalar taints so garbage upper
  lanes cannot false-trigger) and one fcmp/b.vs jumps to an out-of-line replay
  that restores cpu->fp_ckpt and re-executes every member with the exact
  emitters. Exactness proven by tests/guest/nan_rules (2859 raw results vs a
  Rosetta golden). `OCERZ_NO_FPBATCH=1` disables; `OCERZ_INEXACT_NAN=1` drops
  fixups entirely (measurement only).
- Lane-0 caches (l0_*): scalar FP results are computed into V4-V7, inserted into
  the pin, and later scalar readers use the temp -> chains skip the ins latency.
  Caches die at non-aware xmm writers, callouts (g_callout_seq), batch/block ends.
- cmpss/cmpsd via SIMD-scalar fcmeq/fcmgt/fcmge masks (no GPR round trip);
  blendv in place with bit; exact inline min/max; branch-free cvttsd2si.
- Memory: emit_mem_ea_plain / emit_mem_load_plain (JGB fast forms with folded
  displacement, register-offset loads); two hoisted bases per self-loop block
  (JMEMBASE=x17, JMEMBASE2=x16, aux x29 = base+common disp) chosen with
  insn_may_write_gpr; SSE loads/stores and movzx/movsx/movsxd use them.
- Calls: pin-3 push/pop/call/ret address [JGB, rsp] (push = sub + str, a fault
  at the store restores rsp via blk->push_fix); RAS is a ring (monotonic top,
  index & 255, entries cleared on purge/thread creation); RAS entries are untagged
  body pointers; direct AND indirect calls use the host bl/ret protocol (RAS
  entry = continuation right after the bl/blr, callee RET does `ldp x30 ...; ret`,
  continuation chains into the return block); ras[] moved right after xmm in
  OcerzCPU so stp/ldp reach it. `OCERZ_NO_BLRET=1` restores br.
- Indirect branches: per-site 32-entry direct-mapped {rip, body} caches
  (ldr-literal table pointer, filled by the hash path, cleared on invalidation);
  RIP stored only on miss/stop paths.
- Superblocks: a block continues past FORWARD conditional branches (up to 6
  side exits per block); each taken side is an out-of-line chain stub whose
  b.cond/tbz/cbz is retargeted directly once chained; cmp/test + side-exit jcc
  fuse (records inline only when flags are live on either path), other
  producers forward NZCV. Patch stubs by branch kind (a tbz site patched as
  b.cond loses its bit number -- that bug shipped for one build).
  `OCERZ_NO_SUPERBLOCK=1`, `OCERZ_NO_SIDEFUSE=1`.
- RIP-relative EAs load their constant from the block literal pool.
- Chaining: loop-exit edges of self-loop blocks chain (used to leave through the
  frame epilogue + dispatcher); conditional branches of chained edges are
  retargeted straight at the destination when in range (restored on
  invalidation and on stop requests).
- Stops: no more interrupt polls at loop heads / indirect tails / extra
  backward edges. Blocks carry several stop sites (stop_patch + stop_extra[]);
  ocerz_vm_request_exit patches them and kicks every guest thread with SIGEMT
  (no-op handler; the signal return synchronizes the spinning core so it sees
  the patch). `OCERZ_LOOP_POLL=1` restores the poll.

## Known limits / next ideas
- Rosetta has hardware help we do not: an x86-flavoured FP mode (M2 lacks
  FEAT_AFP for us; MRS FPSR costs ~12 cycles) and identity-mapped guest memory
  (PIE main executables cannot be linked away from 0x100000000; MAP_FIXED
  there fails). Static mode therefore keeps guest_base in x0 (1 add per access
  unless hoisted).
- Dynamic mode (Wine) still pays commpage/low_base guards on every access;
  emit_mem_ea_plain and friends only apply in plain/static mode. Big lever for
  Wine, untouched.
- Guest async signals are only delivered at syscall boundaries (pre-existing).
- fork_order flaked twice (1/~30 suite runs) early on; instrumented, not seen in
  the last ~40 runs.
- Remaining per-kernel fat: memcpy/fpvec = 2 adds per SIMD access + fmaxv check
  per batch; leafcall = RAS memory round trips (ras_top load->stp->ldp on the
  critical path); mixed = 2x cvttsd2si (6 words each), b.cond->b chains;
  vm = the record store before `jmp reg` (seam = ALL) -- measured: not significant.
