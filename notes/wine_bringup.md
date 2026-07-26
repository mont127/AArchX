# Wine bring-up under ocerz — architecture & phased plan

Goal: run real x86_64 Wine (Wine Devel 11.8, `~/Wine Devel.app/Contents/Resources/wine`)
under ocerz faithfully — `./ocerz bin/wine notepad` produces a notepad window, with
every x86_64 process (wine loader, ntdll, wineserver, the PE chorus) emulated by ocerz.
No pokes, no skipped semantics.

Status today (2026-06-10): ocerz runs `bin/wine` end-to-end (PIE launcher), dlopens
`ntdll.so` from disk, runs Wine's unix-side init through `init_paths()` (the dladdr
slot-0x60 fix), and with `WINEARCH=wow64` execs the right loader. It stops at mapping
`lib/wine/x86_64-unix/wine` ("cannot map segments"). Everything below is grounded in a
7-agent audit (2026-06-10) with kernel-level A/B tests; load-bearing claims were
adversarially re-verified.

---

## 1. Verified ground truth

### The wine loader binary (lib/wine/x86_64-unix/wine, Wine 11.8)
- Non-PIE MH_EXECUTE (flags NOUNDEFS|DYLDLINK|TWOLEVEL, **no MH_PIE**), LC_MAIN
  (entryoff 0x750), classic LC_DYLD_INFO with **rebase_size 0** and absolute pointers
  baked into __DATA (incl. the `wine_main_preload_info` array {0x1000,0x1fffff000},
  {0x7ff000000000,0x1ff0000}). **Slide must be 0. Nothing can relocate this binary.**
- Segments: __PAGEZERO 4KB; WINE_RESERVE vmaddr 0x1000 vmsize 0x1fffff000 (zero-fill,
  filesize 0, initprot 0x3 — NOT prot-NONE in the load command); __TEXT @0x200000000;
  __DATA @0x200001000; __LINKEDIT @0x200002000; WINE_TOP_DOWN @0x7ff000000000
  vmsize 0x1ff0000 (~32MB only).
- Linked with `-no_pie,-image_base,0x200000000,-no_huge,-no_fixup_chains,
  -segaddr,WINE_RESERVE,0x1000,-segaddr,WINE_TOP_DOWN,0x7ff000000000,-pagezero_size,0x1000`
  (configure.ac:940-948). No wine-preloader exists in this build
  (HAVE_WINE_PRELOADER undef; preloader_exec == plain execv).

### Native runtime layout (vmmap of live notepad.exe, pid-verified)
- The kernel maps each segment at its own vmaddr; **no contiguous hull exists** —
  the ~140TB [0x1000, 0x7ff001ff0000) span ocerz computes is an ocerz artifact
  (src/dyld.c:370 one-span design), not a Wine requirement.
- WINE_RESERVE lives as `---/rwx SM=NUL` (8GB, PROT_NONE reservation); Wine punches
  real content in afterwards. notepad.exe PE at its native base 0x140000000 inside it.
- Unix .so dylibs land just above 8GB (ntdll.so @0x208677000 etc. — dyld-chosen,
  position-independent, anywhere is fine). PE builtin DLLs file-mapped in
  0x6ffffe410000–0x6ffffffe9000 (25 DLLs; ntdll.dll @0x6ffffff40000).
- user_shared_data at fixed **0x7ffe0000**, syscall-dispatcher page at 0x7ffe1000
  (absolute asm refs to 0x7ffe028a); first TEBs forced below 2GB.
- Process model: every Windows process (wineserver, services.exe, 2x winedevice.exe,
  plugplay.exe, svchost.exe, explorer.exe, rpcss.exe, notepad.exe) is a separate host
  process reparented to launchd. 15 threads in notepad.exe at +6s.

### Host arm64 address-space law (kernel A/B-tested on this machine, Darwin 25.5)
- An arm64 process can NEVER have VA below ~4GB: binaries with __PAGEZERO < 4GB are
  killed at exec (EBADMACHO/SIGKILL — tested 0x1000..0x10000000); runtime MAP_FIXED
  below ~0x108000000 fails ENOMEM. **There is no linker flag escape.**
- The host arm64e dyld shared region occupies ~[0x182000000, 0x2E0000000) → EACCES.
  **Wine's __TEXT address 0x200000000 is inside it.**
- Mappable (verified): 0x300000000+, 0x500000000 (12GB block ok), 0x6ffffe410000 (PE
  arena — identity OK), 0x7ff000000000 (WINE_TOP_DOWN — identity OK), 0x8000000000,
  0x10000000000, 0x600000000000. NOT mappable: 0x7ffe0000, 0x4000000000 (EACCES band).
- Consequence: **pure identity mapping cannot serve Wine's guest VA below
  0x2E0000000.** Identity remains valid for everything ocerz does today (arena
  0x300000000+, guest cache 0x7ff8…, PE arena, WINE_TOP_DOWN).

### TEB / %gs (installed binary, not the source tree)
- The installed ntdll.so contains **5 `movl $0x3000003` sites** (_thread_set_tsd_base)
  → Wine Devel 11.8 is mainline-style: it swaps gs.base between pthread-TSD and TEB
  around syscalls. ocerz already implements machdep trap 3 (`cpu->gs_base = rdi`,
  src/syscall.c:1732-1744, per-thread OcerzCPU field) — the swap mechanism is FREE.
- Caveat: the ~/wine-11-d3dmetal SOURCE tree is MNC-patched (HACK 22: no swap, TEB
  mirrored to TSD slots 6/11). Use the source tree for orientation only; the runtime
  target is the installed 11.8 semantics.
- `mac_thread_gsbase` learns the pthread TSD base via
  thread_info(mach_thread_self(), THREAD_IDENTIFIER_INFO).thread_handle — under ocerz
  this MIG call is forwarded to the HOST arm64 thread and returns the wrong value.
  Must be intercepted to return the guest gs_base.

### wine ↔ wineserver IPC (source-verified, MNC tree ≈ mainline here)
- AF_UNIX socket /tmp/.wine-<uid>/server-<dev>-<ino>/socket; per-thread request pipe
  (from server via SCM_RIGHTS), reply+wait pipes (client-created, write ends passed
  via sendmsg SCM_RIGHTS, dlls/ntdll/unix/server.c:968/974/1008-1014). Requests are
  plain write()/writev() structs. No request shm. /dev/ntsync fast-sync is Linux-only.
- Two shared mmaps: KUSER_SHARED_DATA (server keeps a MAP_SHARED file mapping it
  writes; client maps the same file at 0x7ffe0000) and the shared session block
  (win32u winstation.c:106-131).
- Real Mach IPC exists: wineserver bootstrap_register2's a port named by server_dir;
  each client bootstrap_look_up's it and mach_msg_send's its TASK PORT
  (server.c:1478-1518). Server uses it for: per-thread signal delivery
  (mach_port_extract_right(task, client mach_thread_self name) + __pthread_kill —
  thread suspend is SIGUSR1-based, no thread_suspend), ReadProcessMemory/
  WriteProcessMemory (mach_vm_read_overwrite/mach_vm_write + task_suspend/resume),
  and debug registers ONLY via thread_get/set_state(x86_DEBUG_STATE) — which
  wineserver SKIPS when sysctl.proc_translated=1 (is_rosetta(), server/mach.c:77-85).
  Precedent: under real Rosetta, debug registers are zeros. ocerz can do the same.
- wineserver itself: plain x86_64 PIE binary, single-threaded kqueue/kevent loop,
  self-pipe signal handlers, fcntl lock file.
- Client thread IDs given to the server are mach_thread_self() NAMES — ocerz guest
  threads are 1:1 host threads and forwards thread_self, so the right extraction
  works on real host threads. Any future thread-multiplexing design would break this.

### Wine's unix-side signal contract
- sigaction handlers for INT/FPE/ABRT/QUIT/USR1/TRAP/SEGV/ILL/BUS (+SIGSYS on Apple),
  all SA_ONSTACK with per-thread sigaltstack. NO Mach exception handling anywhere.
  PAGE_GUARD/stack growth/Win32 exceptions all ride on guest SEGV delivery.

### ocerz gaps found (src/syscall.c audit)
- Guest signal delivery essentially nonexistent: sigaction records 8 bytes (handler)
  only (:1056-1070); sigprocmask/sigaltstack are no-ops; **no sigreturn (#184)**; no
  frame construction; guest SEGV = _exit(139) via vm.c crash_handler. Single biggest
  missing subsystem.
- sendmsg/recvmsg (#27/#28 cancellable) are HARD-FATAL; only the _nocancel variants
  forward, and only the top-level msghdr pointer is translated (interior msg_iov/
  msg_name/msg_control rely on identity).
- posix_spawn (#244) ignores the XNU args-desc entirely → POSIX_SPAWN_SETEXEC
  silently becomes spawn-and-continue, and guest argv[0] is dropped (:546-582).
- kevent64 (#369) missing (fatal); kevent_qos a success no-op; __semwait_signal/
  sem_wait_nocancel return 0 instantly (sleeps are no-ops — latent spin/race source).
- mmap/mach_vm MAP_FIXED accepted only inside the 4GB identity arena (kernel-chosen
  base, observed 0x300000000); commit bitmap covers only the arena and pg_index
  UNDERFLOWS for addresses below arena_lo (wild indexing).
- fork is real and faithful (:513-533); execve self-re-exec is solid (:618-670);
  dlopen-from-disk + classic fixups already proven by ntdll.so.
- DYLDAPI_DISK_MAX=64 vs Wine's 29 unix .so files + deps — audit capacity.
- map_segments excludes only `vmaddr==0 && initprot==0` (__PAGEZERO) from its span;
  WINE_RESERVE (initprot 0x3, filesize 0) is included → the 140TB span. Zero-fill
  reservation segments must key off **filesize==0**, not initprot.

---

## 2. The architecture decision: split address space (identity + low shadow)

Guest VA spans [0x1000 .. 0x7ff8xxxxxxxx] (WINE_RESERVE bottom to shared cache top) —
nearly the whole 47-bit space, so no single global guest_base offset can fit; and the
host forbids guest-identity below 0x2E0000000. Therefore:

- **Identity window (unchanged): guest VA >= 0x300000000.** Arena, guest dyld cache
  (0x7ff8…), PE builtin arena (0x6ffffe…, verified mappable), WINE_TOP_DOWN
  (0x7ff000000000, verified mappable) all stay guest==host.
- **Low shadow window: guest VA [0, 0x300000000) is backed by a 12GB host block**
  reserved PROT_NONE at a fixed, probed host base (0x8000000000 candidate; 0x500000000
  and 0x10000000000 verified fallbacks; 0x4000000000 is EACCES — probe at startup).
  `g2h(g) = g < 0x300000000 ? g + low_delta : g` — one predictable branch; h2g
  mirrors it for the shadow host range. This covers WINE_RESERVE (0x1000–0x200000000),
  the loader __TEXT/__DATA/__LINKEDIT (0x200000000–0x20000a000), user_shared_data
  (0x7ffe0000), the dispatcher page (0x7ffe1000), and sub-2GB TEBs.
- **The shadow base must be deterministic and inherited**: first ocerz picks it,
  children get it via env (e.g. OCERZ_LOWBASE) across the execve/posix_spawn re-exec,
  so every cooperating ocerz process (wine clients AND wineserver) shares one
  low_delta. That makes cross-process address translation (mach_vm_read/write
  emulation, remote-thread signal targeting) a constant-offset rule.
- Faithfulness note: WINE_RESERVE must be REALLY reserved (12GB PROT_NONE host block
  exists; guest mmap bookkeeping marks [0x1000,0x200000000) reserved), because ntdll's
  VM layer trusts wine_main_preload_info; lying about reservations is exactly the
  class of hack this project forbids.

Why not alternatives:
- Rebuilding ocerz with small __PAGEZERO: kernel kills small-pagezero arm64 binaries
  at exec. Dead.
- Running ocerz as an x86_64/Rosetta host binary: defeats the project.
- Software MMU for everything: unnecessary — only the low 12GB needs translation.

---

## 3. What must change, by subsystem

### M — Memory (src/mem.c, src/dyld.c, src/syscall.c mmap paths)
- M1. Reserve the low shadow block at init (probe fixed candidates; export
  OCERZ_LOWBASE; PROT_NONE; MAP_NORESERVE).
- M2. g2h/h2g branch + commit bitmap generalized to two ranges (arena + shadow);
  fix pg_index underflow for sub-arena addresses.
- M3. map_segments: per-segment mapping mode (no contiguous hull); zero-fill segments
  (filesize==0) become PROT_NONE reservations committed lazily; slide=0 mode for
  non-PIE mains (drop the is_pie fatal when fixed placement succeeds); accept
  WINE_TOP_DOWN-style identity-fixed reservations.
- M4. sys_mmap/sys_munmap/sys_mprotect + mach_vm_allocate/map/protect/deallocate:
  accept MAP_FIXED in the shadow window and in registered guest reservations
  (WINE_TOP_DOWN at identity); keep arena behavior unchanged.
- M5. Audit every raw `(uintptr_t)` guest-pointer cast (dyldapi.c, dyld.c, syscall.c
  ptr_mask single-level translation) for shadow-window correctness; route through
  ocerz_g2h. Interior-pointer translation needed where guest structs can live in low
  memory (iovec arrays, msghdr internals).

### L — Loader/dyld surface
- L1. dlsym("wine_main_preload_info") on the main-executable handle (export-trie of a
  non-PIE main) must work — virtual_init depends on it.
- L2. Disk-dylib capacity: raise/dynamize DYLDAPI_DISK_MAX (29 wine .so + deps).
- L3. ntdll.so dlopens siblings lazily (win32u.so, winemac.so …) — already-working
  path, re-verify under the shadow split.

### S — Syscalls (src/syscall.c)
- S1. sendmsg/recvmsg #27/#28: forward with full msghdr deep-translation (msg_name,
  msg_iov array + each iov_base, msg_control) — SCM_RIGHTS fd passing is the
  wineserver lifeline. Same deep-translation for the _nocancel pair.
- S2. posix_spawn: parse the XNU _posix_spawn_args_desc; honor POSIX_SPAWN_SETEXEC
  (degenerate to execve semantics), preserve guest argv[0], pass OCERZ_LOWBASE.
- S3. kevent64; real __semwait_signal/sem_wait_nocancel (actual waiting); keep
  kevent/kqueue forwarding.
- S4. sigprocmask with real per-thread state (needed once signals exist).

### G — Guest signal delivery (the big one; src/syscall.c + src/vm.c + src/cpu)
- G1. Full sigaction record (handler, sa_mask, sa_flags, sa_tramp), per-thread
  sigaltstack, per-thread mask.
- G2. Host→guest async delivery: install host handlers for guest-registered async
  signals (SIGUSR1/USR2/INT/QUIT…); on arrival, interrupt the target guest thread's
  interpreter loop, build the x86_64 signal frame (ucontext64+mcontext from OcerzCPU,
  sigaltstack switch, mask application), run the guest handler via the guest
  sa_tramp protocol.
- G3. sigreturn (#184): restore OcerzCPU from the guest mcontext.
- G4. Synchronous fault conversion: guest SEGV/BUS/TRAP/ILL/FPE in guest code →
  guest handler (same frame machinery) instead of _exit(139); keep the crash dump
  for unhandled faults. This unlocks PAGE_GUARD stack growth and Win32 exceptions —
  notepad will not survive without it.
- G5. SIGSYS handler registration (Wine registers it on Apple).

### K — Mach/MIG guest-thread semantics
- K1. Intercept thread_info(THREAD_IDENTIFIER_INFO) (MIG over mach_msg) for guest
  threads → thread_handle = that thread's cpu->gs_base.
- K2. thread_get/set_state(x86_DEBUG_STATE): return zeros / accept-and-ignore,
  matching the real-Rosetta precedent (wineserver skips it for translated processes
  anyway — consider surfacing sysctl.proc_translated=1 to guests; decide in phase).
- K3. mach_vm_read_overwrite/mach_vm_write on a REMOTE task (wineserver →client):
  translate remote low-window addresses by the shared low_delta (both sides run under
  ocerz with the same OCERZ_LOWBASE). task_suspend/resume forward as-is.
- K4. bootstrap_register2/bootstrap_look_up/mach_msg task-port send: forwarded
  mach_msg should already carry these; verify the task port the server receives is
  usable for K3 (it's the real host task port — it is).

### P — Multi-process
- P1. wineserver under ocerz: needs S1, S3, fork (have), fcntl locks (verify),
  kqueue loop (have), self-pipe signals (needs G2 minimal: handlers + pipe write).
- P2. Scaffold option for de-risking client work: temporarily run wineserver native
  under Rosetta (plain x86_64 binary; is_rosetta()=true conveniently disables the
  debug-register path). End state remains everything-under-ocerz; the scaffold also
  breaks K3 (native server would read un-translated low-window addresses), so it is
  only viable until the first ReadProcessMemory — use deliberately, then retire.

### U — GUI (winemac.drv)
- winemac.so links AppKit/Metal/OpenGL/QuartzCore/Carbon. The Cocoa window milestone
  (2026-06-10) already proves AppKit-under-ocerz; apple_main_thread parks the main
  thread in CFRunLoop and runs Wine on a second thread — matches ocerz's proven
  HOSTWQ/run-loop capabilities. Expect new walls (CGS/Metal surfaces) but no known
  architectural blocker.

---

## 4. Phases, milestones, exit criteria

Each phase ends make-check-green with the GUI window test still 6/6.

- **Phase 0 — Groundwork (small).** Startup probe choosing the shadow base;
  OCERZ_LOWBASE plumbing through execve/posix_spawn; decide guest-visible
  proc_translated answer; capacity audit (DYLDAPI_DISK_MAX, fd tables).
  Exit: probes logged, env inherited across re-exec, design constants fixed.
- **Phase 1 — Memory architecture (M1–M5, M-L1/L2).** The wine loader maps at
  slide 0, WINE_RESERVE/TOP_DOWN reserved, init_reserved_areas re-mmaps them,
  ntdll.so dlopens, virtual_init completes (preload_info consumed, user_shared_data
  + dispatcher page at 0x7ffe0000/0x7ffe1000 in the shadow window).
  Exit: `ocerz bin/wine notepad` advances past "cannot map segments" to a
  server/signal-era wall; a minimal fixed-address non-PIE test binary maps+runs.
- **Phase 2 — Syscall surface (S1–S4).** Exit: wine reaches server_init_process,
  spawns wineserver (under ocerz, or scaffold if P2 chosen), completes the socket +
  SCM_RIGHTS handshake; `wineserver` under ocerz idles in its kqueue loop.
- **Phase 3 — Guest signals (G1–G5).** Exit: targeted tests — guest sigaltstack
  SEGV handler runs and sigreturns; SIGUSR1 suspend round-trip; then wineboot.exe
  completes prefix init under ocerz (the real integration test).
- **Phase 4 — Mach guest-thread semantics (K1–K4).** Exit: services.exe/
  winedevice.exe/explorer.exe startup chorus runs; GetThreadContext-class APIs
  behave per Rosetta precedent; cross-process R/W verified via the low_delta rule.
- **Phase 5 — GUI (U).** Exit: notepad.exe window visible from inside ocerz
  (`ps` shows it as an ocerz process). Stretch: typing + saving a file.
- **Phase 6 — Purity & hardening.** Retire any scaffold; wineserver under ocerz
  if deferred; perf passes (JIT coverage of hot Wine paths); 16-probe suite stays
  green; document the wine recipe.

Sequencing notes: Phases 1→2→3 are strictly ordered; 4 overlaps 3; 5 only needs
1–4 partially (winemac loads late). Phase 3 is the largest single body of new code
and has value far beyond Wine (any program using signals).

---

## 5. Risk register

- R1 (high, architectural): low-shadow g2h breaks hidden identity assumptions
  (raw casts, single-level ptr_mask). Mitigation: M5 audit + a debug mode poisoning
  the low guest range mapping until explicitly mapped.
- R2 (high): guest signal frames must match Darwin x86_64 layout exactly
  (ucontext64/mcontext64/sigtramp ABI); Wine's handlers read mcontext deeply.
  Mitigation: differential tests against a native x86_64 signal program (Rosetta
  ground truth) before integrating.
- R3 (medium): wineserver↔client Mach flows under double-ocerz (task ports, port
  right extraction) — believed pass-through-clean since guest threads are 1:1 host
  threads; verify early with a 2-process ocerz IPC probe.
- R4 (medium): instant-return sleep stubs cause spins/races in Wine's startup;
  fix in S3 before debugging "mystery hangs".
- R5 (medium): performance — Wine startup executes orders of magnitude more code
  than the Cocoa test (which takes ~20s). Expect minutes at first; JIT hot paths.
- R6 (low): this Wine install is MNC/D3DMetal-patched (winemac.so/opengl32.so
  replaced); if its hacks assume real-Rosetta quirks, behavior may diverge —
  compare against a stock Wine if confusing walls appear.
- R7 (low): macOS updates move the protected VA bands (0x4000000000 EACCES today);
  the Phase-0 probe must re-run every boot, never hard-code.

## 6. Cross-checks for the implementer

- The native loader was watched mapping WINE_RESERVE as `---/rwx SM=NUL` — ocerz's
  PROT_NONE reservation matches native semantics, not an approximation.
- WINE_TOP_DOWN is ~32MB, not gigabytes; identity-map it.
- bin/wine (PIE launcher) needs none of this — only the x86_64-unix loader does.
- i386-unix does not exist; i386 PE coverage is new-WoW64 (wow64.dll/wow64cpu.dll
  present, i386-windows has 821 PE files) — 32-bit apps come much later via WoW64,
  not via a 32-bit unix loader.
- Audit transcripts: /tmp/a5_vmmap.txt, /tmp/a5_pstree.txt, /tmp/a5_lsof.txt
  (native run), /tmp/shadow_probe.c, /tmp/ocerz_pz_test.c (kernel A/B tests);
  workflow wf_c8a882d2-4c9 (2026-06-10).

---

## 7. Phase-1 DONE + the two real walls past it (2026-06-11, committed as aed9c16)

Phase 0/1 (the split address space) is implemented, committed (`aed9c16 phase 0/1`),
and make-check-green. The Wine x86_64-unix loader now MAPS at slide 0 and RUNS;
ntdll.so loads and reaches deep framework initialization. Two walls remain, both
ROOT-CAUSED (3-agent RCA workflow wf_53d7e202-ce4 + empirical tracing). Neither is a
memory-architecture problem — both are init-ordering / threading-identity correctness.

### Wall A — the malloc-proxy SIGBUS during ntdll dependency init (CF init ordering)
- Symptom: infinite recursion `_malloc_type_zone_malloc_outlined ↔
  _malloc_zone_malloc_instrumented_or_legacy`, rdi=`kCFAllocatorSystemDefault`
  (0x7ff840095a00), stack-overflow SIGBUS at icount ~0x2dc000.
- MECHANISM (verified by disassembly): on modern macOS a CFAllocatorRef IS a
  malloc_zone_t. `__CFInitialize` (CF's single `__init_offsets` entry, runtime
  0x7ff802eb4556 = CF_base 0x7ff802eb3000 + 0x1556) populates kCFAllocatorSystemDefault's
  zone vtable. BEFORE it runs, the zone's `malloc` slot (offset 0x18) is the
  self-referential stub, so `malloc_zone_malloc(kCFAllocatorSystemDefault,…)` →
  slot 0x18 → dispatcher → slot 0x18 → … forever. (The plain default zone =
  libsystem_malloc `virtual_default_zone` proxy works fine — it redispatches to the
  real nanov2 zone in malloc_zones[0], which `__malloc_init` installed via
  libSystem_initializer at boot. Verified: `malloc_zone_malloc(default,64)` returns
  a valid nano pointer under ocerz. So the bug is specifically CF's un-initialized
  allocator, NOT a broken default zone.)
- WHY CF doesn't init first: ntdll links CoreFoundation DIRECTLY, so dyld (and ocerz's
  post-order `run_init_phase`) should init CF before ntdll/its dependents. `__CFInitialize`
  is never reached because ocerz's init walk has the wrong discipline (see fix).
- THE FIX (per dyld4 RuntimeState::recursiveInitialization): a proper 3-state
  per-image init discipline — `inited` (permanent) and `beingInited` (cycle guard)
  kept SEPARATE from any cheap closure-build "visited" flag. recursiveInit(img):
  if inited return; if beingInited return; set beingInited; recurse hard+reexport
  deps NOW, **defer UPWARD edges (LC_LOAD_UPWARD_DYLIB 0x80000022) — dyld does NOT
  follow them for init ordering** (that's their entire purpose: breaking init
  cycles); run init sections unless already run; set inited; clear beingInited.
  Do NOT blanket-mark cache images "done" at boot without running their inits — that
  pre-mark is what lets a later ntdll→CF edge skip CF. `__CFInitialize` is internally
  idempotent (`__CFInitialized`/`__CFInitializing` guards), so err toward running the
  full un-initialized closure. IMPLEMENT ITERATIVELY (explicit work-stack), NOT via
  deep C recursion: the experimental `OCERZ_DLINIT` force-init recurses thousands of
  frames deep and corrupts the host C stack — that produced PHANTOM dependency edges
  (the in-run INITEDGE trace showed CF→Foundation/CoreServicesInternal edges that the
  authoritative `lcdump` proves do NOT exist; CF's real 6 deps are libobjc REEXPORT,
  liboah WEAK, libfakelink, libicucore, libSystem, SoftLinking). `OCERZ_DLINIT` is the
  WRONG mechanism and is gated OFF by default; the rewrite replaces it.

### Wall B — SkyLight initializer dispatch_once UD2 (gs_base/TSD identity collision)
- Symptom (the OTHER variant, when CF is forced first): SkyLight ctor
  `__GLOBAL__sub_I_PKGMenuBarContext.mm` (0x7ff8095b0ba6) → libdispatch UD2 at
  0x7ff802ce9487.
- MECHANISM (corrected by disassembly): that UD2 is `__dispatch_once_wait.cold.1`
  "BUG IN CLIENT OF LIBDISPATCH: trying to lock recursively" — NOT
  `_dispatch_assert_queue_fail` (a different fn at 0x7ff802ce9841). The SkyLight ctor
  body is trivial (CGColor globals); the abort is one level down in CoreGraphics's
  lazy `dispatch_once` color-space init. `dispatch_once` reads the owning-thread
  identity from `%gs:0x18` (pthread/dispatch thread-self TSD slot); when the gate's
  stored owner == current `%gs:0x18`, it declares (false) recursion and ud2s.
- ROOT CAUSE: framework initializers run on a context whose `gs_base` was INHERITED
  from a caller/HOSTWQ worker (vm.c documents nested `ocerz_vm_call` inheriting the
  caller's gs_base for thread identity/TSD/errno). Two different guest contexts then
  read the SAME `%gs:0x18` → dispatch_once sees gate.owner == self → false recursion.
  This is the SAME `%gs.base`/TSD identity family as the project's known Rosetta
  whitelist fixups, surfacing during initializer execution.
- THE FIX: run dyld initializers on a single consistent guest thread that owns a
  fully-formed pthread + correct unique TSD base (route initializer execution onto the
  guest main thread that already parks in CFRunLoop under HOSTWQ), so `%gs:0x18`
  returns a per-thread unique/stable/non-zero identity. Confirm in a future run by
  logging gate.owner vs `%gs:0x18 & ~3` right before the ud2.

### Not a regression: the GUI test under host load
- The Cocoa window test (`~/ocerztests/win`) started failing 5/5 at STEP2 with a guest
  under-read SIGBUS (access 0x10 below a 16 MB CF buffer, into the bump-allocator guard
  gap; guest_rip 0x7ff802e803af, a CF/libdispatch memmove path). It fails identically on
  the CLEAN committed `aed9c16` (make-check still green), so it is NOT a code regression.
  Cause: the machine rebooted and ran at load average 8+ (Roblox at 93% CPU + post-reboot
  Spotlight `mdworker` storm); the host contention changes HOSTWQ async-worker timing and
  surfaces a latent guard-gap-sensitive guest under-read. Re-verify the GUI when host load
  is low before treating it as a real bug. (If it persists at low load, the latent fix is
  to not leave 16 KB unmapped guard gaps between bump allocations, or to map the arena
  readable — both reduce faithfulness, so prefer understanding the actual under-read first.)

### 2026-06-11 UPDATE — BOTH WALLS DESTROYED; now at the wineserver IPC layer

Wall A and Wall B are both gone at normal host load. `init_closure` (src/dyld.c) is the
fix: collect the not-yet-initialized hard-dependency closure (DFS, `g_init_being` cycle
guard, **UPWARD edges excluded**), then run initializers in **ascending cache address**
— the dyld cache lays foundational dylibs out first, so address order reproduces its
baked initializer order and breaks the real `CF↔libobjc↔libswiftCore↔Foundation` cycle
the way native does (CF's `__CFInitialize` runs before its dependents). Wired into the
disk-dlopen path; cache-dlopen stays map-only unless `OCERZ_CACHEINIT` (preserves the
proven GUI path). libSystem umbrella pre-marked done (`init_mark_done_closure`, bounded
to `/usr/lib/system/`). Wall B (the SkyLight/CoreGraphics `dispatch_once` "lock
recursively" UD2) turned out to be a **host-load timing artifact** — isolated to
`/tmp/cgtest`, it only reproduces under load avg 8-11, passes 8/8 at normal load and
under `-no-jit`. Added BSD `poll` (syscall 230). Result: **wine runs 258M instructions**
and reaches **`recvmsg` (BSD 27)** — the wineserver socket handshake. The two walls in
sections 7.A/7.B are closed; the entry point below is the live frontier.

### 2026-06-11 UPDATE #2 — wineserver IPC works; wineboot.exe runs

`recvmsg`/`sendmsg` (#27/#28/#401/#402) now deep-translate the guest msghdr (every
interior pointer via `ocerz_g2h`; SCM_RIGHTS fds pass untranslated since guest fd ==
host fd) — the wineserver socket handshake completes and wine `posix_spawn`s
`wineboot.exe --init`. Fixed the child-process shadow reservation (one contiguous
host block, top base derived from low base, only `OCERZ_LOWBASE` inherited). Added
segment-register MOV decode (`0x8c` → constant CS=0x2b/SS=0x23/else 0; `0x8e` → nop).
**wineboot's Windows PE code now executes under ocerz** and stops at `iretq`
(`0x48 0xcf`) — Wine's NtContinue/context-return. That's the live frontier: decode
`iretq` + sweep the PE-side instruction/syscall gaps, in the NT exception-return path.

### 2026-06-11 UPDATE #3 — Phase 3 (guest signals) started

Prereqs landed: `iret`/`iretq` (0xcf) decode+emulate, syscalls `poll`(230) and
`getattrlistat`(476). wineboot now runs multi-threaded to ~2.25M instructions and
stops at threads **executing at rip=0** — genuine faults that on native dispatch to
Wine's SIGSEGV handler. ocerz has no guest signal delivery yet (it crash-dumps), so
that delivery IS Phase 3. Full plan + the exact probed Darwin x86_64 signal-frame ABI
(ucontext 56B, mcontext64 712B with __ss GPRs at +16.., rip@+144, faultvaddr@+8;
siginfo addr@+24) is in the auto-memory `ocerz-project-state.md` (latest entry) and the
groundwork workflow `wf_60d9837f-6e5`. Core = sys_sigaction full record + per-thread
sigaltstack + crash_handler frame-build/redirect via a run-loop sigsetjmp/siglongjmp +
sys_sigreturn(184); the only subtlety is JIT-block faults needing rip-sync (the rip=0
fetch-fault case is already cpu-synced).

### 2026-06-11 UPDATE #4 — Phase 3 DONE: rip=0 walls deliver to Wine's handler

Guest signal delivery is implemented and validated end-to-end, against the real kernel
AND in situ under wine. The rip=0 walls are gone: wineboot threads that execute at
rip=0 now receive a Darwin SIGSEGV delivered to Wine's own `_sigtramp` and run continues.

- **Frame ABI correction:** the live mcontext is **AVX64, uc_mcsize=1032** (not the older
  712 probe); ucontext is **768B** (mcontext ptr @+48), siginfo **104B**. Verified by
  disassembling this host's `libsystem_platform` `_sigtramp`: the kernel enters it with
  **handler in RDI** (the note's earlier "rax=handler" was wrong — first insn is
  `mov rax,rdi`), `edx`=signo, `rcx`=siginfo*, `r8`=ucontext*, `r9`=token; it calls
  `handler(edi=signo, rsi=siginfo, rdx=uctx)` then `__sigreturn(rdi=uctx, esi=0x1e,
  rdx=token)`. `ocerz_signal_deliver` builds `[siginfo][ucontext][mcontext]` top-down on
  the altstack-or-rsp, maps OcerzCPU.gpr→__ss (Darwin order), sets RDI=handler/RDX=signo/
  RCX=si/R8=R9=uc/RSP=si-8/rip=tramp, blocks the sig, and returns 1. `sys_sigreturn`(184)
  restores __ss GPRs + rip/rflags + xmm/mxcsr + mask, does NOT touch gs_base/fs_base.
- **Recovery:** `g_sig_recover` (thread-local `sigjmp_buf*`) set via `sigsetjmp` at the top
  of both run loops (`ocerz_vm_run_cpu`, `ocerz_vm_call`), saved/restored for nesting.
  `crash_handler` builds the frame then `siglongjmp`s back; the loop resumes at the tramp.
- **Faithful host↔guest signal mapping:** a translated guest memory fault is delivered as
  guest **SIGSEGV** regardless of whether the arm64 host raised SIGSEGV or **SIGBUS**
  (the host SIGBUS/ADRALN on the shadow window is an artifact; x86_64 Darwin = SIGSEGV).
  si_code = SEGV_ACCERR if the page is committed else SEGV_MAPERR.
- **Host-bug guard (critical):** delivery only fires when the faulting host address is in a
  guest window (`ocerz_host_in_guest_space()` in mem.h: low-shadow / top / commpage /
  affine `[guest_base, guest_base+arena_hi)`). An ocerz host-code bug (wild/NULL host
  pointer) lands outside and still surfaces as a real crash — it is NOT masked as a guest
  signal. A re-entry guard in `ocerz_signal_deliver` (sig already blocked → return 0) keeps
  a handler that re-faults from looping.
- **Test:** `tests/guest/signal_test.c` — freestanding, installs SIGSEGV via raw
  `__sigaction`(46) with its own `_sigtramp`-ABI trampoline, null-stores, handler rewrites
  the saved rip, sigreturns to a recovery routine. Output is **byte-identical** running
  natively under Rosetta and under ocerz (interp + jit). Wired into make check (13/13,
  14/14 diff, 2/2 dyn green). Diagnostic: `OCERZ_SIGTRACE=1` logs each delivery.
- **In situ:** `OCERZ_SIGTRACE=1 … WINEARCH=wow64 … ./ocerz "$W/bin/wine" notepad` shows
  `SIG deliver addr=0x0 rip=0x0 ->tramp=0x7ff802e7d3a0` for **two** wineboot threads
  (icount ~0x195e95 and ~0x21c6d3); after both, the process advances and **blocks in
  `__psynch_mutexwait`** (0% CPU, healthy IPC wait) — the new frontier, past rip=0.
- **Next walls (not Phase 3):** (a) the one-time `selpool_canonical`/`selpool_build`
  selector-pool scan (dyldapi.c) is slow under load — finite but eats ~10-30s before
  wineboot reaches the threads; (b) the post-rip=0 `__psynch_mutexwait` blocked state
  (pthread/IPC) is where wineboot now parks.

### 2026-06-11 UPDATE #5 — Phase 4 START: jit_lock deadlock fixed, K1 (thread_info gs_base) done, nested faults deliverable

The `__psynch_mutexwait` "park" was NOT a healthy IPC wait — it was an **ocerz-internal
`jit_lock` deadlock** exposed by multi-threaded wineboot. Chain (lldb-confirmed: 2 threads,
neither holding the lock, worker blocked on it = orphaned): a worker's `ocerz_jit_step` ->
`translate()` decodes guest code (`ocerz_decode(g2h(pc))`) **while holding `jit_lock`**; when
a thread jumps to rip=0 that decode faults, and the Phase-3 `crash_handler` `siglongjmp`'d
OUT of `translate()` **skipping `pthread_mutex_unlock(&jit_lock)`** -> deadlock.
- **FIX (decode-probe):** `ocerz_jit_decode_recover` (`__thread sigjmp_buf*`, jit.h/jit.c)
  wraps translate()'s decode loop; `crash_handler` (vm.c) checks it FIRST and `siglongjmp`s
  INTO translate (which ends the block and lets jit_step unlock normally) instead of
  delivering. A real at-rip fault -> 0-insn block -> EUNSUP -> interpreter re-faults with NO
  lock held -> real signal delivered. Regression test `tests/guest/signal_jump0.c` (jumps to
  rip=0) is byte-identical native/interp/jit. Workflow `wf_a21c4720-ead` confirmed the ONLY
  other same-class lock is `g_load_lock` (dyld.c) — SAFE-BY-INVARIANT (guest code under it
  always runs via ocerz_vm_call's own sigsetjmp); map_lock/g_wl_lock/hostwq mutex are SAFE.

Past the deadlock, wineboot's two rip=0 threads deliver to Wine's `_sigtramp` (Phase 3 ✓),
then Wine's handler faulted again. **Workflow re-attribution (3/5 auditors initially wrong):
because the interp advances cpu->rip BEFORE the memory access, the crash rip 0x7ff802e6f4d6
is the insn AFTER the faulting one — the real fault is `mov r8,gs:[-8]` at +0x4cd, so fault
addr = gs_base-8.** Live-confirmed: `SIG nohandler addr=...0d8 gs=...0e0 [==gs-8] comm(gs)=0`.
- **ROOT CAUSE = Phase 4 K1.** Wine's SEGV handler calls `thread_info(THREAD_IDENTIFIER_INFO)`
  to get the thread's unix cthread_self; ocerz forwarded it raw so the host kernel returned
  the **HOST arm64** thread handle (~0x16e......, in the host pthread band). Wine installs it
  via `thread_fast_set_cthread_self`; `%gs:-8` then maps to an uncommitted low-shadow page.
  Confirmed with an `OCERZ_MACHLEAK` reply-scan: `reply_id=3712 off=0x30 host_val=0x16e9870e0`
  then `machdep set gs=0x16e9870e0`.
- **K1 FIX (src/syscall.c, mach_msg2 reply path):** reply id **3712** = thread_info reply;
  `thread_handle` at +0x30, `dispatch_qaddr` at +0x38. When +0x30 holds an uncommitted
  host-range value, overwrite it with `cpu->gs_base` (the guest cthread_self set at
  bsdthread_create = pth+0xe0, arena, committed) and carry the qaddr delta. Live: `K1
  thread_info handle 0x16c2fb0e0 -> gs_base 0x3900fd0e0 (comm=1)` — the gs:-8 wall is GONE.
- **Signal-mask fix (faithful synchronous semantics):** the Phase-3 re-entry guard `if (sig
  masked) return 0` is correct for ASYNC signals but WRONG for a synchronous CPU fault (XNU
  force-delivers; Wine deliberately takes nested SEGVs). Removed it; deliveries from
  crash_handler (the only caller, always synchronous) now bypass the mask. Loop protection is
  now a **consecutive-same-fault** counter in crash_handler (`sig_last_fault`/`sig_repeat`,
  cap `OCERZ_SIG_MAX_REPEAT`=16) — robust to Wine's NtContinue returns (which don't sigreturn,
  so a depth-counter would leak). Also added SA_NODEFER honoring (only auto-block the sig when
  NOT SA_NODEFER) and DARWIN_SA_RESETHAND/SA_NODEFER defines.

**NEW WALL (Phase 4 next) — diagnosed deeper, it is a BAD-POINTER cascade, NOT a missing
page.** Past K1 + nested delivery, Wine's handler faults at `0x7ffd0000` (unix rip
`0x381a37f1c`, insn `mov rdi,[rax+0x320]` so **rax=0x7ffcfce0**), repeatedly → loops+dies.
Traced (OCERZ_MEMTRACE) the sub-2GB layout: Wine **reserves [0x7ff60000,0x7ffe0000)** (one
`mmap-anon prot=0` at 0x7ff60000 len 0x80000) and commits ONLY the TEB `[0x7ffd8000,0x7ffe0000)`
+ KUSER_SHARED_DATA 0x7ffe0000 + dispatcher 0x7ffe1000. So **rax=0x7ffcfce0 points into
reserved-but-uncommitted memory** — on native that faults too, i.e. rax is a GARBAGE pointer,
not a page ocerz forgot to commit. Origin: the rip=0 thread's guest stack shows `[rsp]=0`
(return address 0) with 0xb(=SIGSEGV)/0x1 below → it **RET'd to a 0 sentinel = the classic
"thread entry function returned" teardown** (rip=0 is LEGIT thread-exit, matching the Phase-3
note). Wine's SEGV handler then processes the exit and computes the bad rax (likely a TEB/PEB
field set up wrong by ocerz, OR a signal-frame field we deliver that Wine reads beyond what
signal_test exercises). REFUTED: the PEB is fine — TEB@0x7ffd8000, PEB@[gs+0x60]=0x7ffdc000, COMMITTED; rax=0x7ffcfce0
is NOT the PEB. It is a different per-thread struct in the reserved band (TEB-0x8320; likely
the WoW64 32-bit TEB / WOW64_CPURESERVED CPU area, which Wine places below the 64-bit TEB).
NEXT: (1) disassemble the unix handler at 0x381a37f1c (ntdll.so) to see where rax is loaded —
is it [64bitTEB+WowTebOffset] / a WoW64 field, and is that pointer wrong or is the page just
uncommitted? (2) verify every mcontext/ucontext field Wine's handler reads for a rip=0 exit
(FP/AVX/__es), since signal_test only checks handler-runs+sigreturn; (3) if rax IS a real
WoW64 32-bit TEB Wine allocated, find why its commit (somewhere in [0x7ff60000,0x7ffd8000))
never reaches ocerz (no commit there in the memtrace — Wine may set up the 32-bit TEB via a
path ocerz mistranslates). Also still pending: K2 (thread_get/
set_state x86_DEBUG_STATE accept-ignore / decide proc_translated=1 stance — wineserver's
is_rosetta() skips debug regs), K3 (remote mach_vm R/W low_delta + deep MIG body/descriptor
translation — mach_msg2 only translates the top-level buffer today), K4 (bootstrap/task-port,
forwarded-unverified). Diagnostics added (all env-gated, off by default): `OCERZ_SIGTRACE`
now logs gs_base+commit-status at each delivery, machdep-3 gs sets, and bsdthread_create pth;
`OCERZ_MACHLEAK=1` scans mach_msg2 replies for host-address leaks into the guest. Workflow
audit: `wf_a21c4720-ead`.

### 2026-06-11 UPDATE #6 — the bad-rax wall was a SIGALTSTACK bug; fixed, Wine now handles guard-page faults

The `0x7ffd0000` cascade was NOT a bad pointer / missing commit — it was an **sigaltstack
delivery bug**. Disassembly of the unix handler at `0x381a37f1c` showed `mov r14,rsp; and
r14,~0xffff; mov rax,[r14]; mov rdi,[rax+0x320]` — Wine recovers its **thread data pointer
from the 64KB-aligned base of the stack the handler runs on** (`rax = *(rsp & ~0xFFFF)`). That
ONLY works on the unix sigaltstack (whose 64KB base holds the thread ptr); ocerz was
delivering nested faults on the **32-bit WoW64 stack** instead. Why: Wine registers SIGSEGV
with `SA_ONSTACK` and a sigaltstack (`altsp=0x100000858 altsz=0xf7a8`); ocerz used it for the
first delivery (`sig_on_stack=0`) but a pure sticky `sig_on_stack` flag kept nested faults OFF
the altstack even after Wine's handler switched rsp onto the 32-bit stack. **FIX (src/syscall.c
`ocerz_signal_deliver`):** also switch to the altstack when the current rsp has LEFT the
altstack bounds — `use_alt = SA_ONSTACK && altsp && (!sig_on_stack || !(rsp-altsp < altsz))`
(Linux `on_sig_stack(sp)` semantics; the sticky-flag-only form is what Darwin docs imply but
it breaks Wine's `*(rsp&~0xffff)` recovery when the handler leaves the altstack). make-check
green; signal_test/signal_jump0 unaffected (they never switch stacks). **RESULT:** the thread
now correctly recovers its data (`threadptr[base]=0x7ffd8000` committed) and Wine's handler
runs **8 guard-page faults at `0x7ff4d130`** (committed page, ACCERR — Win32 PAGE_GUARD stack
growth; each delivery advances ~0x444 insns, with a clean `set gs=cthread(0x3900fd0e0) …
handler … set gs=TEB(0x7ffd8000)` swap dance around each) then BREAKS OUT to new code — real
forward progress, not a spin. **NEW WALL:** ~35 insns after the 8th handler swaps `%gs` back
to the TEB, code at `0x7fed374c` does `gs:[-8]` while `gs=0x7ffd8000` (TEB) → reads
`0x7ffd7ff8` (TEB-8, uncommitted) → fault, then a deeper crash in libsystem (`0x7ff802e3b6c4`,
guest_addr `0x800732c0bfff`). This is a distinct **WoW64 %gs-timing** issue: macOS-side code
(needs `%gs`=cthread) runs while `%gs`=TEB on the return-to-Windows path, BEFORE the next
swap. NEXT: identify what `0x7fed374c` is (the WoW64 syscall-dispatch / sigreturn return path?)
and whether ocerz mishandles a `%gs` reset the kernel would do, or whether `0x7ff4d130`'s
repeated ACCERR (a COMMITTED RW page faulting) is itself an ocerz protection-bitmap desync
that should be fixed first. Note: `0x7ff4d130` faulting despite `comm=1` is suspicious — verify
ocerz's host mprotect matches the committed bitmap for that page.

### 2026-06-11 UPDATE #7 — page-fault err code added; cascade ROOT = NtContinue to Rip=0

Two follow-ups past the altstack fix:
- **Page-fault error code (faithful fix, kept):** `ocerz_signal_deliver` left mcontext
  `__es.err` (mc+4) = 0, so Wine saw every fault as a not-present READ. Now `crash_handler`
  recovers the real access type from the **host arm64 ESR** (`uc_mcontext->__es.__esr`):
  instruction-abort EC (0x20/0x21) ⇒ fetch (err bit 4); data-abort WnR (ISS bit 6) ⇒ write
  (err bit 1); committed page (SEGV_ACCERR) ⇒ present (err bit 0); user (bit 2) always. Passed
  as a new `err` arg to `ocerz_signal_deliver` → mc+4. make-check green. (Didn't change THIS
  cascade — Wine isn't gating the rip=0 path on err — but it's correct and needed for
  write/guard-page faults generally.)
- **Cascade ROOT-CAUSED:** a per-block rip-history ring (g_riphist, vm.c) showed the block that
  branches to rip=0 is `0x381a33b6d` = Wine's **`__wine_syscall_dispatcher` / NtContinue return
  path**: `mov rsp,[rcx+0x88]; mov rcx,[rcx+0x70]; push r11; popfq; push rcx; ret` — it restores
  a guest context whose **saved Rip = [rcx+0x70] = 0** and `ret`s to 0. So Wine **NtContinue's a
  context with Rip=0**; the rip=0 fault, the 8 guard-page faults (0x7ff4d130, real PAGE_GUARD
  stack growth — progress), the gs:[-8] fault and the libsystem crash are ALL downstream. The
  24-deep history into it: unix ntdll (0x381a3f7xx/0x381a6c8xx) ↔ libsystem_pthread
  (0x7ff802e6f899, 0x7ff802e733xx) → dispatcher → 0. Restored ctx had rdi=0x381a37ef0 (inside
  the 0x381a37exx thread-recovery fn), rdx=0xb. **Why the context Rip is 0 is the next root**
  (likely a thread-start Windows entry, or a context built wrong). The intertwined sibling wall
  is the WoW64 %gs timing: ~35 insns after the 8th guard-handler swaps `%gs` back to the TEB
  (machdep-3), code at 0x7fed374c does `gs:[-8]` with gs=TEB → TEB-8 (uncommitted) → fault.
  NEXT: watchpoint where the NtContinue context's rip field ([frame+0x70]) is written 0 (or trace
  thread-start), and resolve whether ocerz mis-sequences a %gs swap vs. 0x7fed374c needing
  gs=cthread. (Note: signal_test only validates handler-runs + sigreturn; it does NOT exercise
  these WoW64 / NtContinue paths.)

### 2026-06-11 UPDATE #8 — genuine MAP_SHARED (wineserver shared memory) fixed; rip=0 traced to a zeroed NtContinue context

The rip=0 root (UPDATE #7) was traced further with a per-block rip ring + store-watchpoints:
the dispatcher restores a Wine syscall_frame at guest 0x10fb00 that is all-zero; its rip is
memmove'd (libsystem_platform _platform_memmove) from a source context at 0x10010f850 whose
rip is in turn set by unix-ntdll at 0x381a5a646 (`mov rax,[r15+0x98]; mov [r14+0xf0],rax` =
context_from_server-style copy) — and [r15+0x98]=0. So a new WoW64 worker thread (a real
bsdthread_create thread, pth=0x3900fd000) completes unix init then NtContinue's to a Windows
entry of 0: **the thread's start Rip was never written**.

**FIX LANDED (critical correctness, kept, make-check green): genuine MAP_SHARED.** A 3-agent
workflow (wf_5a1836af-903) found sys_mmap reduced EVERY file-backed mapping — including the
wineserver MAP_SHARED tmpmap blocks (KUSER_SHARED_DATA @0x7ffe0000, the session block
0x381a1c000 len 0x144000, per-thread blocks) — to a ONE-SHOT PRIVATE pread snapshot, so a
client never sees writes the server makes after the snapshot. New `ocerz_map_shared_file`
(src/mem.c) overlays the fd `MAP_SHARED|MAP_FIXED` onto the reserved region (host g2h(gaddr))
and marks the bitmap committed; sys_mmap uses it for `flags & MAP_SHARED` (pread fallback for
MAP_PRIVATE / failure). Verified: all 11 shared blocks now map shared-ok. This is fundamental
to cross-process Wine. (It did NOT change the rip=0 cascade — the thread entry doesn't flow
through these blocks — but it's necessary regardless.)

**ALSO: page-fault err code (UPDATE #7) confirmed kept.** Did not change rip=0 either.

**rip=0 STILL OPEN — two ranked leads from the workflow:** (1) thread-create agent: the
bsdthread_create register/_thread_start/TSD path is CORRECT (args not dropped, r9 bit28 set,
gs_base=pth+0xe0 seeded), but **mach_msg(31)/mach_msg2(47) translate ONLY the top-level buffer
a[0] — NO MIG body/descriptor walk** (complex messages msgh_bits&0x80000000 carrying
port/OOL/descriptor data pass untranslated); if the new-thread context or its entry arrives via
a complex Mach message, it lands wrong/zeroed. (2) wow64-model agent: gs/TEB identity — verify
the new thread reads its entry from the right thread-data and that the two rip=0 threads don't
collide on TEB=0x7ffd8000. NEXT: dump r15 + the source struct at 0x381a5a646 to find what feeds
[r15+0x98]; check whether create_thread's context comes via the unix socket (recvmsg, already
deep-translated) or a complex Mach message (untranslated); implement MIG complex-body
translation if so. Diagnostics added (env-gated): OCERZ_CTXTRAP=<rip> dumps the syscall_frame
at rcx; OCERZ_MEMTRACE now logs FILEMAP/SHAREDMAP; a per-block rip-history ring (g_riphist).

### 2026-06-11 UPDATE #9 — sig_* thread-inheritance bug fixed; MIG-body partly ruled out for rip=0

Workflow wf_5a1836af-903 synthesis ranked 3 causes; acted on them:
- **FIXED (confirmed bug, kept): per-thread signal state was inherited by new threads.**
  sys_bsdthread_create (`w->cpu = *cpu`) and the two workq/workloop worker spawns
  (syscall.c ~792, ~885) copied the creator's OcerzCPU and reset only registers — leaking
  sig_altstack_sp/size, sig_mask, sig_on_stack, sig_last_fault, sig_repeat. A new worker would
  then build signal frames on the PARENT's altstack and gate delivery by the parent's mask
  (corruption + mis-delivery). All three spawn sites now zero the sig_* fields (the HOSTWQ
  bridge already memset'd its template). make-check green. (Didn't change the rip=0 cascade in
  this run — the altstacks happened to be compatible — but it's a real correctness bug.)
- **MIG complex-body translation: real gap, but partly RULED OUT as the rip=0 cause.** Added
  OCERZ_MIGTRACE: 66 complex (msgh_bits&0x80000000) mach_msg2 replies arrive, but they are
  predominantly PORT descriptors (d0 'addr' = a small port name, needs no translation); only a
  few carry real OOL addresses (e.g. reply_id=1021 d0.addr=0x38091e830) and none obviously
  carry a thread context/entry. So mach_msg2 still needs a descriptor-body walk for K3/K4
  faithfulness, but the new-thread entry is NOT arriving via an untranslated Mach descriptor.
- **MAP_SHARED fallback caveat (from synthesis): ocerz_map_shared_file's failure path falls
  back to the private pread snapshot; verified all 11 server-shared blocks report shared-ok, so
  the fallback is not firing here.**

NET this session on the rip=0: the entry is set up LOCALLY at thread creation (not via the
server reply or a Mach descriptor), so the live lead is the gs/TEB-identity / thread-params
question — signal_start_thread reads the new thread's Windows entry from a thread-data /
params structure and writes 0. NEXT: identify signal_start_thread's read of the entry (which
thread-data field / how it's addressed) — likely via a -no-jit scoped trace (OCERZ_TRACE_LO/HI
around the unix-ntdll thread-start fns 0x381a5a6xx) so rips are exact, then watch the producer
of [r15+0x98]. Also worth: confirm the two rip=0 threads don't share TEB=0x7ffd8000.

### 2026-06-11 UPDATE #10 — rip=0 FULLY ROOT-CAUSED: pLdrInitializeThunk=0 (PE-ntdll export dir gate)

The whole rip=0 cascade is now traced to ONE bug. ntdll.so has symbols; the chain (all
disasm-verified + live-watch-confirmed):
- A new worker thread's entry = its syscall_frame.rip, set by `_init_syscall_frame` (ntdll.so
  vmaddr 0x3b430) from the global `_pLdrInitializeThunk`. That global (and its siblings
  pKiUser*Dispatcher / pRtlUserThreadStart, a contiguous block at vmaddr 0xa8830..0xa8868) is
  **0** — store-watchpoints on _pLdrInitializeThunk (0x381aac850) AND _pKiUserExceptionDispatcher
  (0x381aac830) show ZERO writes the whole run. So new threads jump to 0 (downstream: the SEGV
  cascade — guard faults, gs:[-8], libsystem crash).
- The SETTER is `___wine_main` (vmaddr 0x1d4a0). It maps the **PE ntdll.dll builtin** via
  `_virtual_map_builtin_module(machine=0x8664)` (-> r15=[rbp-0x68]), `_virtual_relocate_module`
  (if it fails, jne 0x1f6ac = skip), then reads the mapped PE: e_lfanew@[r15+0x3c], opt-header
  magic@[PE+0x18] (must be 0x20b PE32+ or 0x10b PE32, else jne 0x1f68d = skip), and the
  **EXPORT directory** (DataDirectory[0] at PE+0x88 for PE32+): if RVA==0 (je 0x1f68d) or
  size==0 (je 0x1f68d) it SKIPS the entire kcb-resolution block (vmaddr 0x1e0a4..0x1e84f). The
  block binary-searches that export table for "LdrInitializeThunk" etc. and stores the resolved
  address. Because the watch saw NEITHER the resolved-store NOR the store-0 fallback, control
  never enters the block => the **export directory of the mapped PE ntdll.dll reads as 0 (or its
  magic is wrong) in the wineboot child**.
- The MAIN wine process sets these fine (its threads work); only the wineboot RE-EXEC child
  fails. So **ocerz maps the PE ntdll.dll builtin without a usable export directory in the
  re-exec'd wineboot child.** (context_from_server delivers a VALID rip, so the wineserver/IPC
  is NOT involved — it is purely this local PE-ntdll mapping.)
- NEXT (fix locus): ocerz's handling of `virtual_map_builtin_module`'s mapping of the PE
  ntdll.dll in the wineboot child — verify the export-directory section is mapped/readable
  (likely a file-mapping / section-protection / re-exec-state gap, possibly related to the
  MAP_SHARED/file-mmap path or the PE section layout). Workflow wf_df838b7d-31f is pinning the
  exact ocerz gap + fix. Diagnostics added (env-gated): OCERZ_NOJIT (force interp, propagates to
  children), OCERZ_CTXTRAP now also dumps rsi/server-context_t flags+ctl.rip. Method to inspect
  ntdll.so: per-function capstone disasm from nm -n symbols (linear disasm desyncs); runtime =
  vmaddr + 0x381a04000.

### Phase-2 entry point (next session, low host load)
1. Rewrite the dlopen/boot initializer ordering: iterative 3-state (inited/beingInited)
   + skip upward edges + don't pre-mark cache images done. Exit: ntdll's CF/CoreServices/
   IOKit closure initializes in dyld order, `__CFInitialize` runs, the malloc-proxy loop
   is gone. Keep make-check green + GUI 6/6 (verify at low load).
2. gs_base/TSD uniqueness for initializer execution (route inits onto the owning guest
   thread). Exit: the SkyLight/CoreGraphics dispatch_once abort clears.
3. Then the server/signal era (Phase 2/3 of section 4): sendmsg/SCM_RIGHTS, posix_spawn
   args-desc, guest signal delivery, wineserver bring-up.
- Diagnostics added this session (all env-gated, in src/dyld.c + src/vm.c):
  OCERZ_INITTRACE (per-image init enter/skip with done/gen), OCERZ_INITEDGE
  (parent→dep edges), OCERZ_CFDUMP=<mh> (dump an image's LC_LOAD* as the loader sees
  them), OCERZ_PEEK=<a,a,…> (crash-time guest qword dump), OCERZ_DLINIT (the gated,
  to-be-replaced force-init experiment). RCA workflow: wf_53d7e202-ce4.

### UPDATE #11 (2026-06-12): the "hang" was selpool build slowness, NOT rip=0/threads; real wall = gs_base=0 worker crash

The multi-turn "wineboot/notepad hangs" was mis-diagnosed. Running `bin/wine notepad` with
WINEDEBUG UNSET (prior runs used WINEDEBUG=-all, which suppressed the one informative line) and
sampling the host process showed the reliable behavior is NOT a thread deadlock and NOT rip=0:

- **Reliable hang = `selpool_canonical`/`selpool_build` (src/dyldapi.c) burning 95% CPU.** Wine
  uses ObjC (winemac.drv), so every Wine process calls `_dyld_get_objc_selector`, which builds a
  one-shot open-addressing index over the dyld shared-cache selector pool. base=0x7ff8225a3a40,
  pool_end≈+0x3817f79 (58MB), count≈2,563,796, cap=8,388,608 (64MB table). **Each build takes
  ~11.3s**; the wineboot re-exec chain spawns ~4 processes that EACH rebuild it → ~45s of pure
  selpool overhead. Earlier "hangs" were the loop being killed mid-build. satur=0 (no probe
  saturation) — the table is NOT corrupted; it is just a slow 2M-entry build, made worse by 4
  concurrent builds saturating memory bandwidth. After all 4 builds finish the process tree
  EXITS (procs=0) — it never hung indefinitely.
  - Two false leads ruled out by instrumentation: (a) host/guest aliasing (the 64MB calloc lands
    outside the guest arena; moving it into the arena via ocerz_map_anywhere did NOT help);
    (b) a build data-race (adding a lock did NOT change the timing). The cost is inherent to
    indexing ~2M selectors per process.
  - Over-scan confirmed: real selectors run to ≈ +0x2a11f79 ("…PXModelDeliveryProgr…"); by
    +0x3114f70 the bytes are binary garbage ("…^… eL…"). The 256-consecutive-zeros pool-end
    heuristic over-runs the real pool by ~10MB, but even a correctly-bounded pool is ~2M
    selectors, so bounding alone won't fix the 11s.

- **FIX LANDED (src/dyldapi.c selpool_build): correctness, not perf.** Added a `g_selidx_lock`
  pthread mutex + double-checked init, build into a local `idx` and publish `g_selidx` LAST
  (so a thread that sees it non-NULL sees a complete table), and a probe bound
  (`++probes < cap`) so a full/torn table can never infinite-loop. Reverted to host calloc.
  make check green (14/14 guest ×2, 15/15 diff, 2/2 dyn). NOTE: the ~11s build is still slow;
  the real fix is to query the shared cache's precomputed objc_selopt perfect hash (zero build),
  or correctly bound the pool — deferred as a perf task.

- **REAL Phase-4 wall (after selpool): worker threads crash with gs_base=0.** Deterministic,
  two worker threads (Wine TIDs 002c, 0024) in separate re-exec processes:
  `guest crash: SIGSEGV host_addr=0xfffffffffffffff8 guest_rip=0x7ff802e6f4d6
   guest_addr=0xfffffffffffffff8 icount≈0xb69a3 / 0x13dbeb`, preceded by Wine's
  `err:virtual:alloc_pages_vprot ... Cannot allocate memory for vprot table, size 00100000`,
  ending in `nested fault inside crash handler` (the SECOND worker faulting while the first holds
  the process-global `depth`; the dump itself is complete — not a dump bug).
  - New crash-dump diagnostics (src/vm.c, kept): `gs_base=`/`fs_base=` + `insn@rip=` bytes.
    Output: **gs_base=0x0 fs_base=0x0**, insn@rip = `8b 4b 0c 41 89 c9 41 83 e1 0c 74 36 4c 8b 17`
    = `mov ecx,[rbx+0xc]; mov r9d,ecx; and r9d,0xc; je …; mov r10,[rdi]` (rbx/rdi point into
    ntdll.so data 0x381aac2c0/d8; guest_rip/regs are a JIT block-boundary sync point, not the
    exact faulting insn). The fault host_addr=-8 with gs_base=0 ⇒ a gs-relative (gs:[-8]) or
    base-0 access with the segment base never established.
  - **This SUPERSEDES the "rip=0 / pLdrInitializeThunk=0" theory (UPDATE #10):** the workers DO
    get a valid rip and run real PE code; they fault on gs because cpu->gs_base==0.
  - How Wine sets the x86_64 gs base on macOS (Wine src dlls/ntdll/unix/signal_x86_64.c):
    `_thread_set_tsd_base(teb)` = machdep syscall `eax=0x3000003` (trap 3), arg in RDI. ocerz
    DOES intercept this (src/syscall.c:~2249 sets cpu->gs_base = RDI), and new threads already
    get cpu->gs_base = pth+0xe0 (bsdthread_create/workq). So gs_base==0 means the crashing
    worker's OcerzCPU went through NONE of these (nor the main-thread init at dyld.c:2173) —
    a thread-creation path that never establishes gs_base, OR a g_cur_cpu/per-thread-CPU
    mismatch so the running host thread is using a zeroed CPU. THAT is the next thing to trace.
  - NEXT: log thread creation (which path makes the 002c/0024 worker), confirm whether its
    OcerzCPU.gs_base is set at creation and whether _thread_set_tsd_base(eax=0x3000003) runs on
    it before the first gs access; verify g_cur_cpu identity on worker entry. Repro:
    `OCERZ_INITPHASE=1 OCERZ_HOSTWQ=1 WINEARCH=wow64 WINEPREFIX=$HOME/.wine ./ocerz \
     "<Wine Devel.app>/Contents/Resources/wine/bin/wine" notepad` (WINEDEBUG unset; wait ~45s
    through the selpool builds to reach the crash). Crash diagnostics already in src/vm.c.

### UPDATE #12 (2026-06-12): JIT BUG FOUND + FIXED — IRETQ was not a block terminator (the gs_base=0 wall is GONE)

The deterministic worker crash with gs_base=0 (UPDATE #11) was a JIT correctness bug, not a thread/Mach
semantics gap. Root-cause chain (all live-verified):
- gs_base went 0x3900fd0e0 (worker entry) -> swapped TEB(0x7ffd8000)/cthread(0x3900fd0e0) ~17x via machdep
  trap 3 -> then 0 at the crash. NO code path writes gs_base=0 (only creation=pth+0xe0 and machdep=RDI,
  always logged non-zero); the host cpu struct address is unreachable by any guest g2h address. So gs=0 was
  memory corruption of the running OcerzCPU.
- A per-instruction gs tracer armed after each machdep (OCERZ_GSTRACE, since removed) showed: under the
  INTERPRETER gs NEVER goes 0 and the run reaches a DIFFERENT (earlier) wall. So the corruption is JIT-ONLY.
- The corrupting code is Wine's __wine_syscall_dispatcher context-restore epilogue at ntdll.so 0x381a33b2b:
  `fxrstor64 [rcx+0xc0]` -> a run of GPR restores `mov r15,[rcx+0x68]`... -> `pushfq` ->
  `and qword [rsp],0xffffbfff` (clear NT) -> `popfq` -> **`48 cf` = IRETQ** (returns to the WoW64 code at
  0x7ff08460). The interpreter handles it correctly (re-fetches at the iret target).
- THE BUG (src/jit.c is_terminator): **OCERZ_OP_IRET was NOT in the terminator list.** So translate() decoded
  PAST the iretq (treating the bytes after it as instructions) and the emit loop ran them: the interp slow-call
  for iretq set cpu->rip to the real target, but the JIT block then kept executing the bogus post-iretq
  emitted instructions, corrupting the CPU (gs_base read 0) and diverging control flow (JIT jumped into the
  shared cache 0x7ff8..., interp stayed in WoW64 0x7ff0...).
- **FIX: add `case OCERZ_OP_IRET:` to is_terminator() (src/jit.c ~165).** One line. The JIT block now ends at
  iretq, emits it as a slow-call (interp restores the iret frame), and returns to the run loop which dispatches
  at the new rip -- exactly like RET/SYSCALL. make check GREEN (14/14 guest x2, 15/15 diff, 2/2 dyn). The
  notepad run no longer produces ANY gs_base=0 crash; the JIT now matches the interpreter.
- KEPT (clean, useful): crash dump now prints `gs_base=`/`fs_base=` + `insn@rip=` bytes (src/vm.c).
- NEW WALL (was masked by the IRET bug; pre-existing, the UPDATE #7 "8 guard-page faults" region):
  `SIGBUS guest_addr=0x7ff4d130 guest_rip=0x7ff0ddff gs_base=0x7ffd8000 (TEB)`, insn@rip =
  `48 c1 e0 04 80 bc 01 01 20 00 00 00 74 26` = `shl rax,4; cmp byte [rcx+rax+0x2001],0; je ...`. This is WoW64
  32-bit code at 0x7ff0ddff touching 0x7ff4d130 (an uncommitted/guard page in the low-shadow window -> host
  SIGBUS). gs is now CORRECT (TEB), so this is a genuine Win32 PAGE_GUARD / stack-growth or 32-bit-address
  commit issue, NOT a segment-base problem. NEXT: trace what 0x7ff4d130 is (a WoW64 32-bit stack guard page
  that needs commit-on-demand growth, or a missing reservation) and whether ocerz must deliver this as a guest
  fault Wine grows, or commit it. Repro unchanged (WINEDEBUG unset, ~45s through selpool builds).

### UPDATE #13 (2026-06-12): two more JIT/memory bugs fixed — Wine now initializes Windows DLLs (5x deeper)

After the IRET fix (#12), three further walls were root-caused and fixed; make check stays green
(14/14 guest x2, 15/15 diff, 2/2 dyn) at every step. Wine progressed from icount ~0xb7000 to ~0x39e000
and now reaches `wineboot.exe` loading + initializing system DLLs (msvcrt.dll etc.).

1. **vprot-table ENOMEM (bump-pool waste).** Wine's page-guard handling kept looping at 0x7ff4d130
   because `alloc_pages_vprot` (a guest mmap(NULL,0x100000)) returned ENOMEM. Root cause: a guest
   `mach_vm_allocate(FIXED)` at 0x3fff40000 (Wine reserving a high address that falls in ocerz's
   identity arena) made `ocerz_map_claim_fixed` JUMP bump_next to near arena_hi, stranding ~1.7GB of
   the 2GB pool. FIX (src/mem.c): record such above-the-waterline FIXED claims as "reserved islands"
   instead of jumping bump_next; `ocerz_map_anywhere` fills the gap below contiguously and steps over
   islands (bump_skip_islands); islands are dropped on unmap. The pool is no longer stranded; vprot
   ENOMEM gone.

2. **16KB-vs-4KB protection-rounding bug (THE wall after vprot).** Wine maps ntdll.dll's read-only
   sections then protects them PAGE_READONLY. A RO section start (e.g. 0x3fffde000) is 4KB-aligned but
   not 16KB-aligned; `ocerz_protect` rounded restrictive protections OUTWARD to 16KB, dragging the
   adjacent writable BSS (0x3fffdd130, RVA 0x9d130) read-only -> the guest's own write to its BSS
   SIGBUS'd in a loop (host_prot=0x701: cur=READ). FIX (src/mem.c ocerz_protect): round a PERMISSIVE
   (writable) change OUTWARD but a RESTRICTIVE (RO/RX/NONE) change INWARD, so a 16KB host page shared
   by a RW 4KB guest page and a RO 4KB guest page keeps the union (RW wins) and the guest's writable
   data keeps working. (PROT_NONE already rounded inward; this generalizes it.) Diagnostic kept:
   `ocerz_host_region_prot` + the crash dump's `host_prot=`/`region=` fields (src/vm.c, src/mem.c).

NEW WALL (icount ~0x39ea2e): `wineboot.exe` initializes DLLs and `msvcrt.dll failed to initialize,
aborting` (status c0000005 = STATUS_ACCESS_VIOLATION); `run_wineboot boot event wait timed out`.
The crash is a guest 32-bit load (`ldr w0,[x1]`) of a WILD pointer 0xa3f1c2b745e9ef24 at
guest_rip=0x3a0110d54 (arena PE region). NEXT: disassemble 0x3a0110d54, find which register holds the
garbage pointer and why it's uninitialized (a relocation/global/earlier-init that didn't land), i.e.
why msvcrt's DLL init dereferences garbage. Repro: `OCERZ_INITPHASE=1 OCERZ_HOSTWQ=1 WINEARCH=wow64
WINEPREFIX=$HOME/.wine ./ocerz "<Wine Devel.app>/.../bin/wine" notepad` (WINEDEBUG unset).

### UPDATE #14 (2026-06-12): msvcrt wall root-caused — runtime CoreFoundation dylib's pthread per-thread-data getter returns garbage

The "msvcrt.dll failed to initialize (c0000005)" crash at guest_rip=0x3a0110d54 was traced fully:
- The block is `push rbp; mov rbp,rsp; sub rsp,0x40; call 0x3a0188c76; cmp dword [rax+0x180c],0`.
  rax is the RETURN of the call; the call is an import thunk (`jmp [rip+0x4d44c]`) -> a small fn
  `mov rdi,[global 0x3a031c930]; call pthread_getspecific-thunk; test rax,rax`. So it's a per-thread
  -data getter: `pthread_getspecific([global])`. The libsystem fn is libsystem_pthread (base
  0x7ff802e6e000) +0x1899 = `mov rax, gs:[rdi*8 + disp32]` (= pthread_getspecific).
- The global key at 0x3a031c930 reads 0 (UNINITIALIZED). getspecific then returns a cookie-class
  garbage (0xa3f1c2b7...; cf cthread+0x38 = 0xa3f1c2b4..., a pthread guard cookie) instead of NULL,
  so the caller's NULL-check passes and `cmp [garbage+0x180c]` faults. gs is correct (cthread); this
  is NOT a gs/segment bug (gs:[0x30] under cthread reads 0 cleanly).
- The faulting code lives in a Mach-O DYLIB at host region [0x3a0004000,0x3a0270000) (~2.5MB,
  RO-exec, prot=0x703) loaded INTO THE ARENA. Its LC_LOAD_DYLIBs: /usr/lib/libSystem.B.dylib,
  .../CoreFoundation, and `@loader_path/../../...`. It is NOT a PE (those are at 0x3fe-0x3ff), NOT
  ntdll.so (0x381a04000, 571KB), and crucially **does not appear in ANY ocerz load log** (not the
  closure `loadphase` list, not the `dlopen loaded` line) even under -v. So ocerz's dlopen/closure
  paths did not load it the tracked way, and `OCERZ_DLINIT=1` (g_init_force, which runs all
  sub-closure inits) does NOT fix it.
- CONCLUSION: this dylib's INITIALIZER (which would `pthread_key_create` and store the key in the
  global) never ran. Its key stays 0 -> getspecific reads a wrong/cookie slot -> garbage -> msvcrt
  init aborts -> `run_wineboot boot event wait timed out`. This is the project's central dyld
  init-ordering / eager-set problem (run_init_phase visits the closure + g_dimgs, gated by the eager
  set unless g_init_force; this dylib is reached by neither because its load is untracked).
  NEXT: find HOW this CF-linked dylib enters the arena (it is a runtime use at icount ~0x39ea00; trace
  the mmap/dlopen that creates [0x3a0004000,...)), then run its initializer at the right time without
  regressing the GUI/SkyLight path. Diagnostics kept (vm.c): insn@rip-24, blockhist, host_prot/region,
  rip-region, OCERZ_STRDUMP, ocerz_host_region_prot. Repro unchanged (WINEDEBUG unset).

### UPDATE #14b (2026-06-12): msvcrt wall narrowed to "run_image_inits runs ZERO inits for the runtime dylib"

Pushed the msvcrt/pthread-key wall much further (no fix yet). Traced the load + init of the
faulting dylib precisely:
- A C backtrace on the large arena alloc (OCERZ_MAPBT, since removed) showed the dylib enters at
  host [0x3a0004000,0x3a0270000) via: ocerz_map_anywhere <- map_segments <- ocerz_dlopen <-
  ocerz_dyldapi_dispatch <- ocerz_vm_run_cpu <- ocerz_worker_entry. So the GUEST dlopen()s it via the
  dyld4 API, ON A WORKER THREAD, and ocerz_dlopen loads it.
- The runtime dlopens (OCERZ_DLOPENLOG) are: ColorSync.framework/ColorSync, QuartzCore.framework/
  QuartzCore, ntdll.so, win32u.so. ColorSync/QuartzCore are CACHE-ONLY (no disk file), so the disk
  dylib at 0x3a0004000 is a non-cache DEP of theirs (links libSystem + CoreFoundation +
  @loader_path/../..; uses __cfstring; ~2.5MB). Two such dylibs load contiguously: 0x3a0004000 and
  0x3a0274000. The faulting getter (pthread_getspecific of the key global 0x3a031c930=0) lives in the
  SECOND (0x3a0274000); the first calls it via an import thunk.
- CRUCIAL: ocerz_dlopen_inner DOES run inits (init_closure -> run_image_inits) and INITCLOSURE TRACE
  shows `INITCLOSURE run mh=0x3a0004000` AND `mh=0x3a0274000` both execute -- but OCERZ_INITLOG shows
  ZERO `INIT mh=0x3a0...` lines, i.e. run_image_inits found NO initializer to run for either dylib.
  Their sections are __text/__stubs/__stub_helper (__TEXT) and __got/__const/__cfstring (__DATA_CONST)
  -- NO __mod_init_func (S_MOD_INIT_FUNC_POINTERS, type 0x09) and NO __init_offsets
  (S_INIT_FUNC_OFFSETS, type 0x16), which are the only two run_image_inits handles.
- So the dylib's pthread_key_create constructor never runs (run_image_inits has nothing to run),
  the key stays 0, pthread_getspecific(0) returns a cookie, msvcrt derefs it -> c0000005.
  OPEN QUESTION for the fix: where is this dylib's key actually created? Either (a) it has a
  constructor in an init format ocerz's run_image_inits doesn't recognize (e.g. LC_ROUTINES, or
  __mod_init_func in a segment/section whose type field reads wrong, or chained-fixup __mod_init_func
  pointers that apply_fixups left 0 so the `if(fn)` skip drops them), or (b) the key is created lazily
  via a CF/pthread_once path that didn't fire, or (c) by a dependency whose own init didn't run. NEXT:
  dump the dylib's FULL section list + every LC_* for 0x3a0274000 (peek the whole header), confirm
  presence/type/contents of any init/mod_init section, and check apply_fixups left its init pointers
  non-zero. The fix is then either in run_image_inits (handle the missing init format) or apply_fixups
  (relocate the init pointers). This is the project's dyld init-ordering area; tread carefully to not
  regress the GUI/SkyLight init path. make-check stays green; all this turn's real fixes intact.

### UPDATE #15 (2026-06-13): ★★★ WINEBOOT RUNS END-TO-END TO THE GUI LAYER — zero ocerz faults. 6 walls destroyed this session.

`bin/wine notepad` (WINEDEBUG unset) now runs wineboot + 4 Wine processes (wineboot 0024,
services 002c, rpcss 003c, explorer 004c) all the way through DLL init, the prefix setup, process
spawning, and into WINDOW CREATION. ZERO ocerz fatals/crashes. make check GREEN throughout
(14/14 guest x2, 15/15 diff, 2/2 dyn). The only remaining errors are Wine driver/environment level,
NOT emulator bugs: `no driver could be loaded`/`graphics driver is missing` (winemac.drv display
driver = Phase 5), `Failed to load libMoltenVK.dylib`, `Wine cannot find the FreeType font library`,
`getaddrinfo Failed to resolve host` (offline). ocerz is now faithfully executing Wine; the GUI
needs the macOS display driver, which is the next subsystem.

Six walls destroyed this session, each root-caused and fixed faithfully:

1. **ntdll.so DOUBLE-LOAD (path canonicalization).** The msvcrt pthread-key garbage (UPDATE #14)
   was ntdll.so loaded TWICE: boot at 0x381a04000 (path .../x86_64-unix/ntdll.so) and a duplicate
   via win32u.so's `@rpath/ntdll.so` dep that expanded to `.../x86_64-unix//ntdll.so` (DOUBLED slash
   from an rpath entry ending in '/'). A raw strcmp in dimg_find_by_path missed the dedup -> 2nd
   ntdll, whose per-thread pthread key (global at +0xa8930) was never created by an initializer ->
   pthread_getspecific(key 0) returned a TSD cookie -> msvcrt deref'd garbage. FIX (src/dyld.c
   load_disk_dylib): canonicalize the resolved dep path via canon_dylib_path (realpath collapses //,
   resolves .., follows symlinks) before dimg_find_by_path, exactly as ocerz_dlopen_inner already
   does for top-level dlopens. dyld dedups loaded images by realpath too. Confirmed: only one ntdll
   now; msvcrt initializes.

2. **MAP_SHARED file-overlay teardown (the heap-over-manifest RO collision).** After #1, msvcrt
   init crashed writing 0x110010: Wine SxS-maps a manifest MAP_SHARED PROT_READ (from a read-only
   fd) at 0x110000, parses it, unmaps it, then reuses the address for a HEAP. ocerz_unmap and
   commit_range manage pages with mprotect, but a MAP_SHARED read-only file overlay can't be
   mprotect'd writable (EACCES) nor restored to anonymous reservation -> the page stays the read-only
   file mapping -> the heap's `heap->ffeeffee=0xffeeffee` write SIGBUSes. FIX (src/mem.c): new
   host_make_writable() falls back to `mmap(MAP_ANON|MAP_PRIVATE|MAP_FIXED)` when mprotect can't make
   a page writable, replacing the file overlay with a fresh zero anonymous page (correct MAP_FIXED-
   remap/unmap semantics; a file overlay always covers a whole host page so no anon neighbor is lost).
   Used in commit_range (writable commit + zero-overlap memset) and ocerz_unmap (free path). Wine now
   reaches `wineboot.exe` loading + initializing msvcrt etc.; icount ~0.75M -> ~3.8M.

3. **necp/guarded network syscalls (libsystem_info DNS/host-info setup).** wineboot's libsystem_info
   does getaddrinfo/host-info via necp, hitting unimplemented BSD syscalls. Added to bsd_table
   (src/syscall.c, all host-forwarded with correct ptr_masks): 501 necp_open (1 arg), 444
   change_fdguard_np (6, ptr_mask 0x2a), 502 necp_client_action (6, ptr_mask 0x14), 442
   guarded_close_np (2, 0x02). Forwarding to the real kernel is faithful (the host decides necp
   availability).

4. **PE-syscall dispatch via SIGSYS (the WoW64 NT-syscall mechanism).** Wine's PE ntdll NtXxx stubs
   do `syscall` with a raw Windows syscall number (no macOS class bits -> class 0). On macOS 14+ the
   kernel raises SIGSYS for the "invalid" syscall, and Wine's registered sigsys_handler reads the
   saved rip/rax and routes into __wine_syscall_dispatcher_prolog_end. ocerz fataled on class-0
   syscalls; FIX (src/syscall.c ocerz_handle_syscall default case): deliver SIGSYS to the guest via
   ocerz_signal_deliver (cpu->rip is already past the syscall = the kernel's saved rip; the handler
   computes frame->rip=rip+0xb, frame->rcx=rip). Faithful: ocerz does exactly what the kernel does and
   Wine's real handler runs. (PE stubs alternately `call [0x7ffe1000]` when user_shared_data flag
   0x7ffe0308&1 is set; that direct path needs no SIGSYS.)

5. **int3 -> SIGTRAP (not fatal).** A guest int3 (DbgBreakPoint guards, KiUserExceptionDispatcher)
   is a breakpoint, not a fatal. macOS raises SIGTRAP (trap TRAP_x86_BPTFLT=3) which Wine's
   trap_handler turns into EXCEPTION_BREAKPOINT. FIX (src/interp.c OCERZ_OP_INT3 + src/syscall.c
   trapno): deliver SIGTRAP with trapno 3 so Wine backs ExceptionAddress over the int3; cpu->rip is
   already past it (matching the kernel's saved rip). Only a genuinely unhandled trap (no SIGTRAP
   handler) stays fatal.

6. **★ gs/fs BASE saved+restored across signal delivery (THE big one — the 0x320 crash).** After
   #4/#5 the spawned children deterministically SIGBUS'd at __wine_syscall_dispatcher's return
   (`mov rdi,[r13+0x320]`, r13=0). Root cause: macOS 14+ SAVES the fs/gs base in the signal state and
   RESTORES it on sigreturn; Wine RELIES on this -- init_handler sets gs to the pthread base (cthread)
   for the unix handler's own TLS, expecting the original TEB base back when the interrupted code
   resumes. ocerz explicitly did NOT carry gs_base through the frame, so after a handler gs stayed
   cthread; a syscall reaching the dispatcher then ran `mov %gs:0x30,%r13` with gs=cthread, read
   cthread:0x30=0, and `[r13+0x320]` (the pthread_teb field at amd64_thread_data+0x320) derefed near
   NULL. FIX: ocerz_signal_deliver stashes cpu->gs_base/fs_base in the ucontext (uc+56/uc+64, past
   the 56-byte Darwin ucontext so it nests per-frame) and sys_sigreturn restores them. make check
   green; the 0x320 crash is GONE and wineboot runs to the GUI layer. This was the deepest fix --
   the macOS-14 gsbase-across-signal contract that the whole WoW64 syscall/exception machinery is
   built on.

Diagnostics kept (env-gated): OCERZ_DLPATH (disk-dylib path+base), OCERZ_INITSCAN (per-image section
dump), ocerz_host_region_prot host_prot/region + rip-region in the crash dump (src/vm.c). NEXT WALL
= Phase 5: the winemac.drv display driver (window creation) + FreeType, so GUI apps can actually
draw. ocerz emulation itself is clean through Wine boot.

### UPDATE #16 (2026-06-13): Phase 5 started — winemac.drv display-driver wall PRECISELY located (the Cocoa-app startup spins in libobjc, exceeding macdrv's 5s deadline)

Goal: load winemac.drv so GUI apps can create windows. Traced the "no driver could be loaded" /
"graphics driver is missing" failure end to end (make check stays green; new diagnostics are env-gated).

- winemac.drv (PE) loads + relocates fine; winemac.so (the Cocoa unix lib, links AppKit/Carbon/Metal/
  QuartzCore/Security/...) ALSO loads fine (DLPATH: load_base 0x3a0a88000). So __wine_init_unix_call
  succeeds. The failure is winemac.drv's DllMain process_attach -> MACDRV_CALL(init) = macdrv_init.
- macdrv_init's first check is `SessionGetInfo(callerSecuritySession, ...)` -> sessionHasGraphicAccess.
  NOT the failure: a native x86_64 SessionGetInfo returns attrs=0x6030 graphic=1 both in the launch
  shell AND through ocerz, AND in a re-exec'd ocerz CHILD (verified with a guest posix_spawn ->
  sesstest harness). So the spawned GUI process keeps graphic access.
- The failure is `macdrv_start_cocoa_app` TIMING OUT (winemac.drv PROCESS_ATTACH->DETACH gap = 6.1s,
  i.e. its hard-coded 5s limit + overhead). It posts a CFRunLoopSource (perform=run_cocoa_app) to
  CFRunLoopGetMain() from the Wine-logic thread and waits <=5s for COCOA_APP_RUNNING. apple_main_thread
  (ntdll loader.c:2086) parks the process main thread in CFRunLoopRun(); the same-thread source
  (apple_create_wine_thread) DOES fire (Wine runs), so the run loop works -- the failing case is the
  CROSS-THREAD wakeup, or run_cocoa_app firing but not completing in 5s.
- run_cocoa_app DOES fire: during the 5s wait the GUI process pegs 100% CPU (RNs), so the main thread
  is actively emulating Cocoa setup, not blocked. A SIGUSR1 guest-rip dump (new OCERZ_RIPDUMP) shows
  the hot thread (gs=cthread) spinning in a tight libobjc loop at **0x7ff802a17117** (repeated 17x in
  the rip history; neighbors 0x7ff802a1713a/3f; outer frames 0x7ff802a43541/562, 0x7ff802a469d4,
  0x7ff802a5bd3b) that repeatedly reaches **0xdda002a0** -- a low-shadow-window address that is
  UNCOMMITTED (a byte read of it faults). Main-thread host sample: ~45% in the SIGSYS PE-syscall path
  (ocerz_handle_syscall) + ~49% in framework dlopen (ocerz_dyldapi_dispatch -> ocerz_dlopen).
- CONCLUSION: winemac.drv fails because the Cocoa app startup (run_cocoa_app) spins in a libobjc loop
  (0x7ff802a17117) touching an uncommitted 0xdda002a0, never reaching COCOA_APP_RUNNING inside macdrv's
  5s window. This is the objc/Cocoa-under-ocerz frontier (cf. the earlier objc class-realization /
  selector / selpool notes). NEXT: disassemble libobjc 0x7ff802a17117 (identify the objc op -- method
  cache scan? class realize? selector loop?), find why it lands on uncommitted 0xdda002a0 (a garbage
  IMP/isa, a fault-retry loop, or an objc structure ocerz set up wrong), and either fix the objc state
  or commit/repair 0xdda002a0. Repro: `WINEDEBUG=+macdrv,+module OCERZ_RIPDUMP=1 OCERZ_INITPHASE=1
  OCERZ_HOSTWQ=1 WINEARCH=wow64 WINEPREFIX=$HOME/.wine ./ocerz "<Wine Devel.app>/.../bin/wine" notepad`,
  then `kill -USR1 <hottest ocerz pid>` during the winemac.drv attach. Kept gated diagnostics:
  OCERZ_RIPDUMP (SIGUSR1 guest-rip+history+bytes), OCERZ_DLPATH now also prints DLERR (dlopen failures).

### UPDATE #17 (2026-06-13): selector lookup made O(1) (kills the 11s selpool build); winemac.drv wall moved to a kevent busy-poll

Two findings while attacking the winemac.drv 5s-Cocoa-timeout wall (UPDATE #16):

1. **FIXED (real win, kept): _dyld_get_objc_selector is now O(1) via the cache's objc_selopt perfect
   hash, not the ~11s build.** dyld4 vtable slot 0x2a0 = _dyld_get_objc_selector = selpool_canonical,
   which on first call built a 2.5M-entry open-addressing index over relativeMethodSelectorBase
   (~11s; the deferred UPDATE #11 perf debt). The shared cache already ships a precomputed selector
   hash: objc_opt_t+0x08 (alongside the class hash at +0x20 ocerz already uses). ocerz now reads it
   (g_selopt) and selpool_canonical does a perfect-hash lookup (selopt_canonical, the same objc_-
   stringhash_t format as the class table's stringhash_find_raw, but the selector table ends at the
   string-offset array and the canonical selector IS table+offsets[h]). The slow build is now only a
   fallback for a cache without selopt. VERIFIED: OCERZ_SELVERIFY cross-checks selopt vs the build per
   lookup -> ZERO mismatches on an objc program; objc2/objctest pass; make check green; selpool_build
   no longer runs at all (OCERZ_SELLOG shows 0 builds). This permanently removes the 11s build.

2. **winemac.drv STILL fails (gap unchanged at ~5.95s), so selpool was NOT the bottleneck.** With the
   selector lookup now instant, macdrv_start_cocoa_app still times out at its 5s limit. SIGUSR1 guest-
   rip dumps (OCERZ_RIPDUMP) of the hottest wine proc now consistently (3/3) land in libsystem_kernel
   at 0x7ff802e347ce -- the instruction right after a `syscall` for BSD 363 = **kevent** (the prior
   stub is 362 = kqueue). Regs: kevent(kq=9, changelist=NULL, nchanges=0, eventlist=..., nevents=128,
   timeout=ptr); caller is a tight loop in arena code at 0x300011xxx (the re-exec'd wine loader / main
   exe, NOT the Cocoa cache code). The thread is at 100% CPU (host kevent returns immediately, so the
   timeout is small/zero), i.e. a BUSY-POLL: a wine thread spins polling kqueue 9 for an event ocerz
   never delivers (a registered fd/Mach-port/timer filter that isn't bridged to the host kqueue, or a
   wineserver reply that's stuck). NOTE the kevent loop is in ARENA code, not the Cocoa setup -- so it
   may be a different thread/process than the GUI main thread doing run_cocoa_app; the leading theory
   is that this busy-poll steals CPU (contention) so the GUI process's Cocoa startup misses its 5s
   deadline. NEXT: identify what filter is registered on kq=9 (instrument the kqueue(362)/kevent(363)
   forward to log the registered filters + returns) and why its event never fires under ocerz -- this
   is the kevent/event-delivery / wineserver-coordination layer. Kept gated diag: OCERZ_RIPDUMP
   (SIGUSR1 rip+hist+regs+bytes), OCERZ_SELVERIFY (selopt vs build), OCERZ_DLPATH->DLERR. Repro as
   UPDATE #16 + `kill -USR1 <hottest ocerz pid>`.

### UPDATE #17b (2026-06-13): winemac kevent spin pinned to libdispatch event-source dispatch (not drained)

Refined the UPDATE #17 kevent busy-poll. The hot kevent(kq=9, timeout=0.016s) returns 1 event ~96% of
the time (11k calls in ~5s). OCERZ_KEVLOG dumping the RETURNED events shows the firing events are
EVFILT_READ (filter=-1) on ~21 DISTINCT fds (0x41/0x3b/0x4a/0x38/...), each consistently data=0x40
(64 bytes readable), flags=0x5 (EV_ADD|EV_ENABLE, NOT EV_CLEAR -> LEVEL-triggered). The registered
filters are 49 EVFILT_READ + 48 EVFILT_WRITE + 6 EVFILT_USER (ident=0x1 = the dispatch manager's
self-wakeup). So this is libdispatch's event manager (dispatch_mgr) on kq=9: it gets a READ source
ready (64 bytes), should dispatch the source handler to a workqueue worker that drains the fd, but the
fd is NEVER drained (strace shows 0 read/recvmsg on these fds), so the level-triggered source re-fires
every 16ms forever -> ~2300 events/s burns 100% CPU and the GUI process's Cocoa startup misses
macdrv's 5s deadline. ROOT: libdispatch event-source dispatch isn't completing under ocerz -- the
kqueue event reaches the dispatch_mgr but the source handler (which would read the 64 bytes) never runs
on a worker. This is the libdispatch-event-source <-> OCERZ_HOSTWQ workqueue coordination layer (the
manager polls + delegates; the delegation/handler-run is what's broken). NEXT: trace how a ready
EVFILT_READ source is supposed to dispatch its handler (dispatch_mgr -> _dispatch_source_invoke ->
workqueue worker) and find where ocerz drops it -- likely the same HOSTWQ bridge that the wineboot
phase relied on, but for event SOURCES (read/write) rather than async blocks. Diagnostics kept (gated):
OCERZ_KEVLOG (kevent calls + registered filters + returned events). make check green.

### UPDATE #18 (2026-06-13): Phase 5 winemac.drv — THREE objc bring-up walls destroyed (selector canon, disk-dylib class registration, objc-registration boundary)

The UPDATE #17/#17b kevent spin was a RED HERRING for the GUI: it is wineboot's ntdll kqueue reactor
(symbol region near server_select/sock_read in lib/wine/x86_64-unix/ntdll.so, loaded low at the per-
process arena base) waiting, not the cause of the winemac timeout. The actual Phase-5 wall is
winemac.drv (lib/wine/x86_64-unix/winemac.so) failing to load its graphics driver. Three distinct objc
bugs, each masking the next, were found and fixed. ALL faithful to real dyld/objc behaviour; make check
green throughout (14/14 guest x2, 15 differential, 2 dynamic, all unit suites incl. test_syscall 96/0).

NOTE on repro: the heap-commit c0000005 reading 0x8ffff8 (an uncommitted page inside a heap view the
guest committed RW, 0x700000-0x8fffff) only fires on a FRESH prefix during wineboot --init and is
flaky; it is a real ocerz commit/16KB-host-page-aliasing bug worth tracking later, but it is NOT the
GUI blocker. Once $HOME/.wine is initialized, notepad reaches winemac.drv directly.

WALL 1 (FIXED) — selector not uniqued. macdrv_start_cocoa_app sends
  +[NSThread detachNewThreadSelector:toTarget:withObject:]
and aborts: "selector (0x3a0acf686) ... does not match selector known to Objective C runtime
(0x7ff8225f6052)". 0x3a0acf686 is winemac.so's OWN __objc_methname string (a __TEXT,__objc_methname
entry); the cache canonical is 0x7ff8225f6052. _dyld_get_objc_selector (dyldapi slot 0x2a0) DID return
the canonical when queried, but the winemac.so selref at __objc_selrefs (winemac+0x53a28) stayed at the
local string pointer -- modern objc, on a shared-cache launch, trusts dyld to have uniqued every selref
and runs NO per-image selref fixup, so a selref left at the image-local string is a distinct pointer
from the cache canonical and fails objc_msgSend's pointer-identity match. FIX: ocerz's mini-dyld now
canonicalizes each arena dylib's __objc_selrefs to the cache canonical at load (dyld.c
canonicalize_objc_selrefs, called after apply_fixups in both load_disk_dylib and dlopen_load_image;
uses new public ocerz_dyldapi_canonical_selector -> selpool_canonical). A name absent from the cache is
left as its local string (a stable unique pointer). This is exactly what dyld's objc selector optimizer
does for disk images.

WALL 2 (FIXED) — disk dylib classes never registered with objc. Past wall 1, objc:
"Attempt to use unknown class 0x3a0add788" (winemac.so __objc_data class-object region). The cache
dlopen path (cache_dlopen_hit) calls ocerz_dyldapi_objc_map_one to drive objc map_images, but the DISK
dylib path (ocerz_dlopen_inner) never did -- so winemac.so's own classes (WineApplication/window
subclasses) were never realized. Early Cocoa steps used cache classes (worked after wall 1); the first
winemac-defined class faulted. FIX: ocerz_dlopen_inner now calls objc_map_one for each newly loaded
disk image (deps-first) BEFORE running its initializers, mirroring dyld's notify-objc-then-init order.

WALL 3 (FIXED) — objc-registration boundary. objc_map_one STILL skipped winemac.so: it returned early
with already=1. objc_image_already_loaded checked g_closure_mh (the dyld image-list closure), but
ocerz_dyldapi_register_image appends EVERY disk image to g_closure_mh for the image/unwind APIs --
which is NOT the objc-registration set. Only the boot closure was actually handed to objc map_images;
post-boot disk images sit in g_closure_mh unregistered. FIX: api_objc_register_callbacks now records
the boot-driven images into g_objc_dlopen_mapped, and objc_image_already_loaded checks ONLY
g_objc_dlopen_mapped (the true objc-registered set), not g_closure_mh. winemac.so now registers
(OBJCMAP ... winemac.so batch=5).

RESULT: winemac.drv loads, canonicalizes its selectors, registers its classes, and SPAWNS the Cocoa
thread (detachNewThreadSelector now works). The crash moved deep into the Cocoa thread startup.

NEXT WALL (open) — Cocoa-thread Foundation. The spawned NSThread crashes in:
  __NSThread__start__ -> -[NSThread name] -> +[NSString stringWithUTF8String:] ->
  -[NSString initWithBytes:length:encoding:]: unrecognized selector sent to instance 0x60000000c000
i.e. basic NSString creation fails ON THE COCOA THREAD (works on the main thread), then libc++abi
terminate -> the abort path null-derefs in Wine's ntdll ([rax+0x320] with rax=0, the recurring SIGBUS
at guest_rip 0x381a37f23 / guest_addr 0x320). Thread-specific -> suspect the new thread's objc/runtime
state (TSD/gs_base/autorelease) or selector identity on a non-main guest thread. winemac.so categories
(WineExtensions, WineShapeMaskExtensions) target graphics classes, not NSString, so they are not the
cause. Diagnostics kept (all env-gated): OCERZ_OBJCLOG (objc_map_one path + batch), OCERZ_SELLOG
(SELMISS), OCERZ_MEMLOG=<addr> (per-op commit trace for a probe host page), commitmap in the SIGTRACE
fault dump, OCERZ_KEVLOG (kevent + caller module via ocerz_dyld_dump_images/name_for_addr +
ocerz_vm_riphist).

### UPDATE #19 (2026-06-13): Phase 5 NEXT WALL precisely localized — Cocoa-thread [NSString alloc] returns a HEAP NSString, not the placeholder (cache method-list lookup miss for +allocWithZone:, Wine-context-only). DIAGNOSED, NOT YET FIXED. make-check GREEN throughout.

After WALL 1-3 (UPDATE #18) winemac.drv spawns its Cocoa thread; that thread aborts in
__NSThread__start__ -> -[NSThread name] -> +[NSString stringWithUTF8String:] ->
-[NSString initWithBytes:length:encoding:]: unrecognized selector sent to instance 0x60000000c000.
Exhaustively root-caused (new env-gated diag OCERZ_SELTRAP=<rip> dumps recv/isa/sel/gs at a guest rip;
pointed at objc __forwarding_prep_0___ = 0x7ff802f0f070):

PROVEN NOT the cause (each ruled out with a live isolated test that PASSES under ocerz):
- selector canonicalization: SELTRAP shows sel=0x7ff8225a3f25 = the UNIQUE cache canonical for
  "initWithBytes:length:encoding:" (selopt perfect-hash confirms; only 1 copy in the pool). NOT WALL-1's
  canonicalize_objc_selrefs.
- class identity: receiver isa=0x7ff8436d08a0 IS ocerz's real NSString ([NSString class] in an isolated
  ocerz run returns exactly 0x7ff8436d08a0). The workflow's "AOSAccounts NSString" was a whichlib
  segment-range artifact (the cache COALESCES objc class data across dylib ranges).
- static method lists / thread / NSApplication-on-thread / dlopen+category / winemac.so-objc-shape mimic:
  ALL reproduced in isolation and ALL PASS (string creation returns the __NSPlaceholderString placeholder).
  So it is NOT static cache data, NOT a spawned-thread issue, NOT objc registration of a category dylib.

THE ACTUAL MECHANISM (objc x86_64): +[NSString stringWithUTF8String:] (0x7ff8040cc9df) allocates via an
objc alloc fast-path (libobjc 0x7ff802a20e8a, same shape as objc_alloc 0x7ff802a1f9fe):
  rax = metaclass = [cls] & 0x7ffffffffff8 ; test byte[rax+0x1f],0x40 (FAST_CACHE_HAS_DEFAULT_AWZ, the
  0x4000 bit of cache_t._flags at metaclass+0x1e) ; jne fast-createInstance ELSE msgSend(cls,allocWithZone:).
WATCHED NSString's metaclass (0x7ff8436d08f0) _flags at 0x7ff8436d090e in the live Wine run: the AWZ bit
0x4000 is NEVER set (values 0x1/0x2001/0x2031/0xa031) -- so the alloc CORRECTLY falls through to
objc_msgSend(NSString, allocWithZone:). The bug is therefore ONE LEVEL DEEPER: in the Wine context the
+[NSString allocWithZone:] dispatch resolves to NSObject's DEFAULT allocWithZone: (class_createInstance)
instead of NSString's cluster override that returns the __NSPlaceholderString singleton -> it returns a
HEAP NSString-isa object (0x60000000c000, a nano-zone alloc, NOT the placeholder at 0x7ff8436d0080 that an
isolated ocerz run returns) -> -[NSString initWithBytes:length:encoding:] is then sent to a plain NSString
instance, which (confirmed on BOTH native and ocerz: instancesRespondToSelector==0) does NOT implement it
-> unrecognized selector -> NSException -> libc++abi terminate -> the recurring null-deref SIGBUS in Wine's
ntdll abort path (guest_rip 0x381a37f23, [rax+0x320] rax=0).

So the root is a CACHE METHOD-LIST LOOKUP MISS for +allocWithZone: on NSString's metaclass that happens
ONLY in the live Wine process (not in any isolated harness). NSString's metaclass IS realized early
(_flags 0x2031 at icount ~314380) and its cache fills during the call, yet the lookup misses the cluster
override -- i.e. NSString's metaclass baseMethodList (a cache RELATIVE-LIST-LIST: header {entsize=8,
count=0x1a0} of relative pointers to sub-lists) is incompletely searched/attached in the Wine context, so
allocWithZone: resolves to the inherited NSObject default. winemac.so registration is the only Wine-unique
trigger but mimicking its objc shape (RR-overriding class + graphics-class categories) does NOT reproduce,
so the trigger is the live Wine load/realize ORDERING, not winemac.so's objc structure per se.

NEXT (fix-critical): instrument objc class realization / relative-list-list attachment for NSString's
metaclass in the Wine context -- find why +allocWithZone: is not in the searched method table there (a
missed relative-list-list sub-list, a re-realization that drops methods, or a realize-ordering where
NSString is consulted before the cluster methods attach). Trap +[NSString allocWithZone:]'s IMP in the
Wine run vs isolation to confirm which IMP wins. Kept env-gated diag: OCERZ_SELTRAP, OCERZ_WATCH/WATCHBT
(store watch + bt), OCERZ_OBJCLOG (now also logs objc_map_one batch contents). make-check green
(14/14 guest x2, 15 diff, 2 dyn, all unit incl test_syscall 96/0).

### UPDATE #20 (2026-06-13): Phase-5 alloc wall pushed deeper — loaded-bit hypothesis RULED OUT; frontier = which +allocWithZone: IMP wins. Still unfixed; make-check GREEN.

Continued from UPDATE #19. New env-gated diag OCERZ_METHDUMP (in dyldapi.c ocerz_dyldapi_dump_method, fired from vm.c sel_trap_report): for the SELTRAP receiver's class, walks the metaclass's static baseMethods relative-list-list and reports each sub-list's image index + objc loaded bit + whether it defines a selector + the method IMP.

FINDINGS (live Wine run, the failing -[NSString initWithBytes:length:encoding:] forward):
1. The crash receiver 0x60000000c000 is a HEAP NSString instance (nano-zone), NOT the __NSPlaceholderString
   placeholder (which an isolated ocerz run returns at 0x7ff8436d0080). So +[NSString stringWithUTF8String:]'s
   internal alloc returned a plain NSString instead of the placeholder.
2. The objc alloc fast-path it uses (libobjc 0x7ff802a20e8a) tests FAST_CACHE_HAS_DEFAULT_AWZ (bit 0x4000 of
   the NSString metaclass cache_t._flags @ metaclass(0x7ff8436d08f0)+0x1e). OCERZ_WATCH on that word in the
   live Wine run: the AWZ bit is NEVER set (flags only ever 0x1/0x2001/0x2031/0xa031), so the fast-path
   CORRECTLY falls through to objc_msgSend(NSString, allocWithZone:). So the bug is in the allocWithZone:
   resolution/execution, not the AWZ flag.
3. ★ LOADED-BIT HYPOTHESIS RULED OUT: NSString's metaclass baseMethods IS a cache relative-list-list
   ({entsize=8,count=0x1a0} of per-image sub-lists), and one sub-list (imgidx=21, **loaded=1**) DOES define
   allocWithZone: (nameptr 0x7ff8225a3acc = the canonical selector; IMP 0x7ff8040b450a = Foundation+0xa50a).
   So allocWithZone: is present AND its image is loaded -> objc realization should include it. The earlier
   "a sub-list is dropped because its image is unloaded" theory is dead.
4. That IMP (Foundation+0xa50a) disassembles to: `cmp rdi(self), [rip->global 0x7ff843adad48]; je ->return
   placeholder(lea); else -> xor esi,esi; jmp default-alloc`. BUT in an ISOLATED ocerz run where
   [NSString alloc] CORRECTLY returns the placeholder, that global 0x7ff843adad48 reads 0 (== in the static
   cache too) and self=NSString!=0 -> the cmp would FAIL -> else branch -> heap. Yet isolation returns the
   placeholder. CONCLUSION: 0x7ff8040b450a is NOT the operative +allocWithZone:. There must be MULTIPLE
   allocWithZone: definitions across the relative-list-list sub-lists and a DIFFERENT one wins; the Wine
   context likely resolves to a different (wrong) one than isolation.

FRONTIER / NEXT: dump EVERY allocWithZone: definition across ALL sub-lists (with IMP + imgidx + loaded) in a
reproducing Wine run, and trap each candidate IMP to see which objc actually invokes in Wine vs isolation
(and what it returns). The reproduction is FLAKY (needs to reach winemac.drv within the run window; ~1/3-1/8
runs). DIAGNOSTIC CAVEAT: the helper meth_list_has() in dyldapi.c gives FALSE NEGATIVES (reported
has(allocWithZone:)=0 for the sub-list that demonstrably contains it via the strstr printer) -- trust
meth_list_print_substr, not meth_list_has, until that discrepancy is understood.

Kept env-gated diag (all inert when unset; make-check GREEN 14/14 guest x2, 15 diff, 2 dyn, unit incl
test_syscall 96/0): OCERZ_SELTRAP, OCERZ_METHDUMP, OCERZ_WATCH/WATCHBT, OCERZ_OBJCLOG (with batch contents).
Cache tools: /tmp/{clsname,methwalk,resolv,cdump,whichlib}.

================================================================================
UPDATE #21 (2026-06-13): PHASE-5 "unrecognized selector"/NSString CASCADE WALL DESTROYED
  (supersedes the #19/#20 diagnosis — the alloc miss was a missing +LOAD phase)
================================================================================
ROOT CAUSE (definitive, via WATCH/WATCHBT + capstone disasm of the cache):
- +[NSString allocWithZone:] returns the __NSPlaceholderString singleton only when
  (self == *<global at cache 0x7ff8436dad48>). That global is set by Foundation's
  __NSInitializeProcess, which is a Foundation +LOAD method (reads NSZombieEnabled
  etc. via getenv, then calls a helper at Foundation+0x...3fc that caches several
  class pointers via objc_getClass -- NSString -> the global). Foundation has 0
  mod_init_funcs (INITLOG count 0); the setup runs ONLY via +load.
- ocerz runs the objc +LOAD phase (run_load_phase -> ocerz_dyldapi_run_image_loads,
  which walks __objc_nlclslist/__objc_nlcatlist and calls each +load IMP) ONLY at
  BOOT, for the main executable's dependency closure (dyld.c ~line 2455).
- win.m links Foundation -> Foundation is in the boot closure -> its +load runs ->
  global set -> alloc returns the placeholder -> works.
- The Wine loader does NOT link Foundation; winemac.drv dlopens Cocoa/Foundation
  POST-boot. cache_dlopen_hit ran map_images (objc register) + restricted mod_init,
  but NEVER the +load phase -> __NSInitializeProcess never ran -> global stayed 0 ->
  +[NSString alloc] returns a plain NSString -> -[NSString initWithBytes:length:
  encoding:] unrecognized -> Cocoa-thread NSException abort -> macdrv "no driver".
  (Proven: OCERZ_WATCH on the global fires in win.m, NEVER in Wine; the writer's
  function is never entered in Wine even under NOJIT.)

FIX (src/dyld.c):
1. cache_dlopen_hit now runs dyld's full dlopen sequence for the newly-loaded cache
   sub-closure: map_images (objc_map_one) -> +LOAD phase (run_load_phase) -> C/C++
   initializers (restricted init_closure). The +load phase is the missing piece.
2. run_load_phase gained a PERMANENT per-image done marker g_load_done[] (it
   previously deduped only per-generation), so each image's +load runs exactly once
   -- boot images are skipped on the later dlopen, only the genuinely new images
   (Foundation/AppKit/...) run. g_init_force was added to its run gate (mirroring
   run_init_phase) so the dlopen call bypasses the boot eager-init filter.
3. WALL-2 win.m crash regression (introduced earlier this session by registering
   disk-dylib objc): the loop now SKIPS images whose path contains
   "/System/Library/Extensions/" (GPU/hardware driver bundles, e.g.
   AGXMetalG14X.bundle) -- registering their classes/categories routed Metal/
   CoreAnimation through a driver ocerz can't run and faulted libdispatch. Wine's
   own dylibs are never under /Extensions/, so they still register.

VERIFIED: Wine objcerr=0 across 4/4 runs (cascade gone); make-check GREEN (sse 246/0,
syscall 96/0, guest 14/14 x2, diff 15, dyn 2); win.m 0 crashes, ~3-4/5 PROBE-OK
(baseline). Cleaned up all the throwaway debug toggles (OCERZ_FULLINIT, OCERZ_LOADIMG,
OCERZ_CBDUMP, WINEMAC-LOAD/global-read traces, WATCHHEX, RIPLOG caller-chain dump).

NEW WALL (older, now revealed past the cascade): macdrv_start_cocoa_app TIMES OUT
("err:macdrv:macdrv_init Failed to start Cocoa app main loop") = the UPDATE #16/#17
libdispatch event-source <-> OCERZ_HOSTWQ workqueue wall (Cocoa main-loop startup
spins, never signals COCOA_APP_RUNNING in 5s). Plus the gs:0x320 SIGBUS in the WoW64
dispatcher (guest_rip=0x381a37f23, guest_addr=0x320) in ~half the runs -- a separate
known thread-gs issue, now reachable because the cascade no longer aborts first.
NOTE #14's "0x7ff843adad48 reads 0 in isolation yet placeholder returned" puzzle is
resolved by #18: in isolation the +load DID run (boot closure) and set the REAL
global 0x7ff8436dad48 (not the 0x...adad48 typo'd in #14); the placeholder path keys
on 0x7ff8436dad48.

================================================================================
UPDATE #22 (2026-06-13): NEXT WALL (post-cascade) PRECISELY LOCALIZED — gs:0x320 crash
  = libdispatch/HOSTWQ worker running the WoW64 dispatcher with no Wine thread setup
================================================================================
With the #21 +load fix, Wine advances past the NSString cascade into winemac.drv's
Cocoa bring-up. notepad x6: gs:0x320 SIGBUS 4/6, macdrv "Failed to start Cocoa app
main loop" 2/6, window-created 0/6. The two are facets of ONE wall (the Cocoa app's
libdispatch machinery under ocerz+Wine = UPDATE #11 thread-gs ∩ #16/#17 libdispatch).

ROOT (gs:0x320, traced via new OCERZ_GSTRACE + capstone):
- Crash: SIGBUS guest_addr=0x320, guest_rip=0x381a37f23 (WoW64 __wine_syscall_dispatcher),
  reached via the SIGSYS path (PE ntdll class-0 syscall). The faulting insn is the
  dispatcher's gs-restore: it computes r14 = rsp & ~0xffff (the thread's 64KB-aligned
  stack/signal-stack base), rax = [r14] (Wine's amd64_thread_data ptr stored at the
  base), then rdi = [rax + 0x320] (amd64_thread_data+0x320 = the saved pthread/cthread
  base) and calls libsystem _thread_set_tsd_base(rdi) (mov eax,0x3000003; syscall) to
  restore gs. For the crashing thread [rax+0x320] reads a CONSTANT garbage 0x16 (same
  every run; the thread's pthread struct address varies, so 0x16 is NOT derived from it
  -- it's an uninitialized slot). So the dispatcher sets gs_base=0x16 -> the next
  gs-relative access faults. (In #11 the same slot read 0; now 0x16 -- same wall.)
- The crashing thread's stack (r14=0x380900000) matches NO bsdthread_create (7 logged,
  none near it) -> it is an ocerz HOSTWQ/libdispatch worker (spawned for the Cocoa app's
  dispatch machinery), NOT a Wine-created thread. Wine's per-thread WoW64 setup
  (storing amd64_thread_data at the stack base and the real pthread base at +0x320,
  done by signal_init_thread on Wine threads) never ran for it, so when guest code on
  that worker reaches the WoW64 dispatcher (a Cocoa->Wine callback making a Win32
  syscall), the gs-restore reads the uninitialized 0x16.

So the wall is: the Cocoa app's libdispatch workers (bridged by OCERZ_HOSTWQ) end up
running Wine PE code that hits __wine_syscall_dispatcher, which relies on per-thread
Wine state that only Wine-created threads have. macdrv-timeout is the same machinery
spinning instead of crashing (the #17 event-source/workqueue drain issue).

NEXT (fix-critical, deep): determine the exact Cocoa->Wine call path that runs Wine PE
code on a HOSTWQ worker; either (a) ensure such workers get Wine's per-thread WoW64
init (amd64_thread_data + the +0x320 pthread base) before they can reach the
dispatcher, or (b) route those Win32 calls so they don't execute the dispatcher's
stack-relative thread lookup on a non-Wine stack. Tied to the #16/#17 libdispatch
event-source<->workqueue drain. New gated diag (inert unset, make-check GREEN): 
OCERZ_GSTRACE (logs machdep/sigreturn/deliver gs writes; flags gs_base < 0x100000 and
dumps the dispatcher caller on a bad write). Repro: WINEDEBUG=+macdrv OCERZ_INITPHASE=1
OCERZ_HOSTWQ=1 WINEARCH=wow64 WINEPREFIX=$HOME/.wine ./ocerz "<Wine>/bin/wine" notepad.

--------------------------------------------------------------------------------
UPDATE #23 (2026-06-13): #22 gs:0x320 ROOT CAUSE COMPLETED — foreign (Cocoa/libdispatch)
  thread runs Wine PE code; the WoW64 dispatcher's rsp&~0xffff lookup has no cpu-area
--------------------------------------------------------------------------------
Refines #22 (which said "HOSTWQ worker"; the truth is more precise). Watching the
exact +0x320 slot (OCERZ_WATCH=0x380900340, the slot the dispatcher reads) shows it is
NOT a stable Wine amd64_thread_data field at all -- it is written repeatedly by
LIBSYSTEM code (rip 0x7ff802efbadd / 0x7ff802c8db49, rdi=0x6000xxxx nano-zone heap)
with heap values 0x10, 0x16, 0x6c23649f. So the 64KB region the dispatcher lands on
(rsp & ~0xffff = 0x380900000) is a LIBSYSTEM/libdispatch HEAP/scratch region, and
[base]=td=0x380900020, [td+0x320]=0x16 are just coincidental heap bytes -- NOT a Wine
thread structure. (td "looked valid"/committed because it's live heap; [td+8] was a
heap-stored code ptr, not a thread field.)

COMPLETE ROOT: a FOREIGN thread -- one running libsystem/Cocoa/libdispatch code on a
libsystem-allocated stack with NO Wine TEB/cpu-area -- executes Wine PE code (a
Cocoa->Wine callback), does a Win32 syscall (class-0 -> SIGSYS), and enters
__wine_syscall_dispatcher. The dispatcher's gs-restore finds the thread's cpu-area via
rsp&~0xffff, but on a foreign stack that points into libsystem heap, so it reads garbage
0x16 as the saved pthread base and sets gs=0x16 -> the next gs access faults
(guest_addr=0x320). The macdrv-timeout runs are the same machinery spinning instead.

So the wall is the FOREIGN-THREAD -> WINE-PE entry: Wine PE code must only run on a
thread with a Wine TEB/cpu-area (so the dispatcher's stack lookup resolves). On real
Wine+macOS this holds because macdrv marshals Cocoa events to the Win32 side rather than
running PE code inline on Cocoa/dispatch threads (or Wine sets up a TEB at the unixlib
boundary). Under ocerz, a dispatch/Cocoa thread reaches PE code without that setup.

FIX DIRECTIONS (deep, design-level): (a) detect the foreign-thread->PE entry and give
the thread a Wine TEB/cpu-area (or borrow the owning Wine thread's) before it can reach
the dispatcher; (b) ensure the Cocoa->Wine path marshals to a real Wine thread instead
of running PE inline; tied to the #16/#17 libdispatch event-source<->workqueue bridge,
since OCERZ_HOSTWQ is what brings these foreign workers into the guest. NEXT concrete
step: trace the exact call edge where a thread with gs!=TEB first executes PE code
(0x381xxxxxx range) and what spawned it, to choose (a) vs (b). make-check GREEN
(syscall 96/0 after `make clean`), win.m 0 crashes. Gated diag OCERZ_GSTRACE retained.

  #23 ADDENDUM (direction-b probe): the foreign thread DOES have a sigaltstack, but it
  is a LIBSYSTEM region (not a Wine signal stack with thread_data at its base). And at
  the Win32-syscall instant the +0x320 slot is NOT yet 0x16 -- the 0x16 is written
  shortly after, by CONCURRENT libsystem code mutating that shared heap (WIN32SYS
  syscall-time prediction never fires). Confirms the dispatcher lands on live shared
  libsystem heap, not thread_data. UNIFYING INSIGHT for the fix: this is the #16/#17
  libdispatch wall -- a block/event that should run on the MAIN thread (a Wine thread)
  is instead run on a libdispatch WORKER (foreign thread); winemac.so's handler there
  calls a Wine PE callback -> Win32 syscall on a non-Wine thread -> dispatcher crash.
  So the macdrv-timeout (block never runs -> COCOA_APP_RUNNING never signals) and the
  gs:0x320 crash (block runs on the wrong thread) are the SAME root: libdispatch
  main-queue/event-source ownership under ocerz. THE fix target = make main-queue work
  run on the guest main thread (the #16/#17 OCERZ_HOSTWQ event-source<->workqueue
  bridge), which resolves both. Removed the dead WIN32SYS probe; kept OCERZ_GSTRACE.

================================================================================
UPDATE #24 (2026-06-13): REAL FIX landed — HOSTWQ async-worker bridge (queue_cb), the
  macdrv Cocoa-main-loop TIMEOUT is GONE; sole GUI blocker is now the gs:0x320 crash
================================================================================
THE GAP: with HOSTWQ, ocerz_hostwq_queue_cb (the kernel's ASYNC / non-event workqueue
callback) was a NO-OP, and sys_workq_kernreturn always spawned SYNTHETIC workers even
under HOSTWQ -- a split worker model. So dispatch SOURCE handlers whose target is a
GLOBAL/root queue (Cocoa brings up several EVFILT_READ sources) never got a draining
worker -> the fd stayed readable -> the dispatch event-manager re-fired every 16ms at
100% CPU -> macdrv_start_cocoa_app missed its 5s deadline ("Failed to start Cocoa app
main loop"). [The #17 dispatch_mgr spin.]
THE FIX (src/syscall.c): (1) ocerz_hostwq_queue_cb now BRIDGES like the kevent path but
with NO events -- one guest worker region per host wq thread, enter start_wqthread as an
async (NEWSPI, QoS-tagged, non-workloop) worker so _dispatch_worker_thread2 drains the
matching QoS root-queue bucket. (2) Under OCERZ_HOSTWQ_ASYNC, sys_workq_kernreturn
REQTHREADS (op 0x20) forwards to the REAL kernel __workq_kernreturn(368) instead of
spawning synthetic, so workloop(kevent_id) + event(kevent) + async(here) all go through
ONE kernel-owned pool with correct QoS/lifecycle, like a native process.
RESULT (Wine notepad x6, OCERZ_HOSTWQ_ASYNC=1): macdrv timeout 2/6 -> 0/6 (GONE). The
runs now progress (faster, no dispatch spin) to the SAME gs:0x320 foreign-PE crash,
which is the SOLE remaining GUI blocker. VERIFIED SAFE: make-check GREEN (96/0 etc.);
win.m 0 crashes; wineboot crash count IDENTICAL with/without async (2 per run, completes
either way) -- so no regression, the fix is gated and clean.
NEW FINDING: the gs:0x320 crash is GENERAL, not GUI-specific -- wineboot (no Cocoa/
winemac) ALSO hits it 2x/run (guest_rip 0x381a37f23, guest_addr 0x320) but SURVIVES
because the crashed thread/process is not its critical path. For the GUI the crashed
process IS the one that creates the window, so it blocks. So the foreign-thread->PE /
WoW64-dispatcher-on-a-non-Wine-stack crash (UPDATE #22/#23) is the project-wide next
wall, surfaced everywhere a libdispatch/workqueue worker reaches PE code.
NEXT: the gs:0x320 crash (foreign worker runs the WoW64 dispatcher with no Wine cpu-area
on its stack). Gate OCERZ_HOSTWQ_ASYNC stays experimental until the crash is fixed and
the full GUI path can be validated. Gated diag OCERZ_GSTRACE retained.

================================================================================
UPDATE #25 (2026-06-14): gs:0x320 crash — Wine dispatcher CONTRACT decoded + TWO
  variants established by ground truth (4-agent workflow + first-hand Wine-source read)
================================================================================
WINE CONTRACT (wine-11.6 source = the active 11.8 binary's layout; dlls/ntdll/unix/
signal_x86_64.c): __wine_syscall_dispatcher requires gs = the thread's Wine TEB at its
gs-state lookup. get_current_teb() = (TEB*)(rsp & ~signal_stack_mask) -- the TEB sits at
the base of a 64KB block and the per-thread signal stack lives INSIDE that block, so the
SIGSYS handler (registered SA_ONSTACK, signal_init_process:2787) resolves the TEB from
rsp. TEB+0x320 = amd64_thread_data.pthread_teb (C_ASSERT:515) = the saved macOS gs base,
set once at thread init from mac_thread_gsbase() (2848). sigsys_handler (2566): runs
init_handler (sets gs=pthread_teb for the handler's own TLS), stamps the syscall_frame,
then redirects RIP_sig to __wine_syscall_dispatcher_prolog_end and RELIES on the macOS-14
kernel restoring gs=TEB on sigreturn (the gs the PE code held at the class-0 syscall).
So: the dispatcher only works when entered with gs=TEB, on a thread whose 64KB stack base
holds the TEB. A foreign thread (no Wine TEB, libsystem sigaltstack) has neither.

TWO VARIANTS (same root "dispatcher entered with gs != the thread's Wine TEB", different
thread/edge), proven via a per-thread gs-history ring + stack-scan diag (added then
REVERTED this turn -- see below):
 - VARIANT 1 (DOMINANT in this repo; notepad 4-5/6, wineboot 2/run): a foreign WORKER
   thread (synthetic workq / start_wqthread; altsp=0 = NOT a Wine thread, no
   signal_init_thread) runs a PE callback via a libsystem_pthread->PE edge (stack scan:
   cache 0x7ff802xxxxx frames calling PE 0x381909xxx -> __wine_syscall_dispatcher) with
   gs = the worker's cthread (pth+0xe0), NOT via SIGSYS (gshist shows NO sigdeliv -- a
   DIRECT __wine_unix_call). The dispatcher's gs-restore reads [rsp&~0xffff]->[+0x320] =
   garbage (libsystem heap) = 0x16 -> gs=0x16 -> SIGBUS guest_addr=0x320 rip=0x381a37f23.
 - VARIANT 2 (the workflow's slow-worktree observation; rip=0x3fff66748, the 64-bit unix
   dispatcher): a genuine bsdthread (Wine thread WITH a TEB) takes a class-0 SIGSYS while
   gs is the cthread (Wine's init_handler swapped it); ocerz delivers SIGSYS without
   restoring gs=TEB -> the dispatcher reads gs:0x30 = cthread -> bad. gshist for this
   shows the normal TEB(0x7ffd8000)<->cthread(0x3900fd0e0) swap. THIS repo never hit it
   fatally (the SIGSYS path always had gs=TEB here -- fix C never fired in 5 runs).

FIX DESIGN (from the workflow, faithfulness-gated; owner rejects pokes):
 - Fix C (variant 2): at the class-0 SIGSYS edge, if gs is the cthread but the thread has
   a known last-TEB (tracked per-cpu when machdep/sigreturn install a TEB-band gs,
   predicate 0x10000<=gs<0x380000000), restore gs=TEB so the dispatcher enters as the
   macOS-14 kernel would. IMPLEMENTED this turn, then REVERTED: it is INERT in this repo
   (variant 2 doesn't reproduce; it never fired) and unverifiable here. Faithful and
   ready to re-land when variant 2 can be reproduced.
 - Fix B (variant 1, the real blocker here): the foreign worker must not run Wine PE code
   (real Wine marshals Cocoa->Win32 to a Wine thread and never runs PE on a libsystem
   workqueue worker). This is the open #16/#17 libdispatch-ownership / foreign-thread-PE
   work and is the dominant gs:0x320 crash. Identifying the exact PE callback + routing
   it to a Wine thread (or not bringing up the worker) is the next concrete step.

REVERTED diagnostics: a per-cpu gs-history ring (cpu.h + pushes + crash dump) and fix C
were added to nail the above, then fully reverted -- the crash dump's gshist line and the
GSBAD stack-scan did their job (establishing variant 1 vs 2) and the tree is back to the
verified #21-#24 state (make-check GREEN). KEPT: OCERZ_GSTRACE (machdep/sigreturn/deliver
gs-write trace + GSBAD).
ENVIRONMENT NOTE: win.m (the native Cocoa GUI regression test) is currently flaky --
crashes ~3-4/5 at guest_rip=0x7ff802e803af (libsystem_pthread accessing an uncommitted
arena addr) -- AND IT CRASHES IDENTICALLY ON THE COMMITTED HEAD 36935c6 (3/4). So this is
a machine/environment state issue (it passed earlier in the session), NOT the session's
uncommitted work. A fresh machine state / reboot is the likely remedy; re-verify win.m
before treating any win.m crash as a code regression.

================================================================================
UPDATE #26 (2026-06-14): gs:0x320 variant-1 crash localized to the EXACT instruction
  (ntdll.so unix→PE dispatcher return on a worker with no Wine TEB). Fix still open.
================================================================================
The user chose to pursue the Wine GUI crash (gs:0x320 variant 1). Localized it to the
instruction level with a non-perturbing per-thread probe (OCERZ_PEENTRY: first time a
WORKER thread runs a 0x381xxxxxx rip with a non-TEB gs; one cheap compare/step so the
timing-race still fires -- unlike the single-stepping OCERZ_TRACE window, which masks it).

FINDINGS:
- The crashing thread is a workqueue/libdispatch WORKER: gs=0x3900fd0e0 (cthread =
  pth+0xe0), rsp=pth-0x48, altsp=0 (NO Wine sigaltstack -> signal_init_thread never ran
  -> NOT a fully set-up Wine thread). It runs SUBSTANTIAL Wine ntdll.so UNIX-side code
  (PEENTRY at icount ~0xde000, crash at ~0x98d9xx -- ~9M instructions later) CORRECTLY
  with gs=cthread (unix code uses the cthread base), then crashes at the unix->PE return.
- Pinned the module: byte-searched ntdll.so for the exact crash insn
  `mov rax,[r14]; mov rdi,[rax+0x320]` = 49 8b 06 48 8b b8 20 03 00 00 -> file/RVA 0x3336d;
  the crash rip 0x381a37f23 is the `call _thread_set_tsd_base` 10 bytes later, so
  ntdll.so LOAD BASE = 0x381a04bac. The dispatcher gs-restore is ntdll.so RVA ~0x33f23;
  the worker's first non-TEB PE rip (0x381a23ea0) is RVA 0x1f2f4 -- an internal bsearch
  loop (get_load_order/config-lookup style) between ___wine_main and
  _set_load_order_app_name, i.e. Wine init/loader code.
- So __wine_syscall_dispatcher's gs-RESTORE epilogue (the unix->PE return:
  rax=[rsp&~0xffff] = the Wine TEB stored at the signal-stack base; rdi=[TEB+0x320] =
  pthread_teb; _thread_set_tsd_base(rdi)) reads GARBAGE because this worker has NO Wine
  TEB block at rsp&~0xffff -> rdi=0x16 -> gs=0x16 -> SIGBUS guest_addr=0x320.

ROOT (restated, now instruction-precise): an ocerz workqueue/libdispatch worker runs
Wine ntdll.so code that returns to the PE side via the syscall dispatcher, but the worker
is not a Wine thread (no TEB, no Wine signal stack), so the dispatcher's gs-restore -- the
macOS-14 unix->PE return that recovers the TEB from the signal-stack base -- reads junk.
On real macOS a libdispatch worker never runs Wine PE/ntdll-return code (Wine marshals to
a Wine thread); ocerz lets it.

FIX (B, still OPEN, deep): the worker must not run Wine code that returns through the
dispatcher, OR must be given a Wine TEB/signal-stack before it can. This is the
#16/#17/#24 libdispatch-ownership / foreign-thread-setup work. Reverted this turn's
probes (OCERZ_PEENTRY, the WT-trace rsp addition); tree is back to the verified #21-#24
state, make-check GREEN. NOTE: win.m is currently flaky with a SEPARATE memmove-over-read
crash (a guest memmove reads past a page-aligned source into an unmapped arena gap; on
HEAD too) -- not this; see prior note.

================================================================================
UPDATE #27 (2026-06-14): gs:0x320 crash MOSTLY FIXED via a bounded gs-recovery; Wine
  now advances past it to the macdrv Cocoa-main-loop wall. make-check GREEN.
================================================================================
Built on #26's instruction-precise root cause (an ocerz workqueue/libdispatch worker
runs Wine ntdll.so code with no Wine TEB; __wine_syscall_dispatcher's gs-restore reads
pthread_teb from a non-existent TEB -> junk -> SIGBUS guest_addr=0x320). The fully
faithful fix is upstream (don't run Wine code on a non-Wine worker / marshal) -- deep,
still open. As a BOUNDED, safe bridge:

FIX (src/syscall.c dispatch_machdep, DEFAULT-on): _thread_set_tsd_base with an
obviously-invalid base (newgs < 0x100000) WHILE the thread already has a valid gs
(>= 0x100000) is, by construction, only the dispatcher's gs-restore installing the junk
it computed from a missing TEB -- a legit thread NEVER sets a sub-0x100000 gs base. So
keep the thread's current (valid) pthread/cthread base instead of installing junk and
faulting. This lets the mis-routed worker finish its Wine work with its real pthread
base (it already ran ~1.65M PE instructions fine with that gs).

RESULT (notepad x6): gs:0x320 crashes ~4-5/6 -> 2/6; the runs now ADVANCE past gs:0x320
to the macdrv Cocoa-main-loop wall ("Failed to start Cocoa app main loop" 5x = the
#16/#17/#24 libdispatch-ownership wall) instead of crashing. make-check GREEN (sse 246/0,
syscall 96/0, 54/0, guest 14/14 x2, diff 15, dyn 2); win.m not worsened (2/3 PROBE-OK,
0 crashes -- its memmove-over-read crash is separate/flaky). The trigger NEVER fires for
a legit thread (gs bases are always large), so it is safe process-wide.

RESIDUAL (2/6): the rarer sub-case where the dispatcher's `mov rax,[r14]` yields rax=0
(stack base has no TEB ptr) so the *read* `mov rdi,[rax+0x320]` faults BEFORE the machdep
-- gs-keep (which acts at the machdep) cannot catch it. Eliminating it needs either the
upstream marshaling fix or a fault-handler recovery (a bigger intervention).
NEXT WALL (now reliably reached): macdrv_start_cocoa_app's Cocoa main-loop timeout =
the #16/#17/#24 libdispatch event-source/main-queue ownership work.
NOTE: gs-keep is a bounded RECOVERY for an ocerz divergence (a real macOS process never
runs Wine code on a libdispatch worker), not faithful to the macOS kernel (which would
install the junk and fault). Kept default because it is safe + unblocks GUI progress;
gate/revert if strict faithfulness on the default path is required.

================================================================================
UPDATE #28 (2026-06-14): ★★ gs:0x320 CRASH FIXED (0/6 default, was ~4-5/6). make-check
  GREEN, win.m 4/4 PROBE-OK. Two complementary fixes; Wine now advances past it.
================================================================================
The crash (an ocerz workqueue/libdispatch worker, OR a Wine thread whose gs was swapped
to the unix cthread by its own signal handler, running __wine_syscall_dispatcher whose
gs-restore resolves the TEB/pthread_teb from a gs that is NOT the thread's Wine TEB) is
eliminated by TWO fixes (both DEFAULT-on, src/syscall.c + include/ocerz/cpu.h):

1. FIX C (the faithful one, dispatch via the class-0 SIGSYS edge): track per-cpu
   `wine_teb_base` = the last TEB-band gs the thread installed (predicate
   0x10000 <= gs < 0x380000000, i.e. a low WoW64 TEB ~0x7ffd8000, NOT the arena cthread
   ~0x3900fd0e0). At the class-0 SIGSYS edge, if gs is currently the cthread/arena base
   but wine_teb_base is set+committed, restore gs=wine_teb_base BEFORE delivering SIGSYS,
   so __wine_syscall_dispatcher runs with gs=TEB exactly as the macOS-14 kernel would
   (it restores the segment base across a signal). This is the workflow's recommended
   faithful fix and prevents the crash AT THE SOURCE (gs=TEB before the dispatcher).
2. GS-KEEP (bounded recovery, machdep edge): if _thread_set_tsd_base is handed an
   obviously-invalid base (< 0x100000) WHILE the thread already has a valid gs, keep the
   valid base instead of installing junk. Covers the DIRECT-call dispatcher path (no
   SIGSYS) where a non-Wine worker has no TEB at all -- it keeps its real pthread base.

RESULT: gs:0x320 crashes default 0/6 (gs-keep alone was 2/6 residual; fix C closes it by
keeping gs=TEB so the dispatcher never reads junk). make-check GREEN (sse 246/0, syscall
96/0, 54/0, guest 14/14 x2, diff 15, dyn 2). win.m 4/4 PROBE-OK 0 crashes. The fix-C
predicate NEVER fires for a healthy thread (legit syscalls already have gs=TEB) and the
restore only swaps cthread->the-thread's-own-TEB, so it is safe process-wide.

NEXT WALLS (now reliably reached, past gs:0x320):
- Default (no async): macdrv_start_cocoa_app Cocoa-main-loop timeout (#16/#17/#24).
- With OCERZ_HOSTWQ_ASYNC=1 (advances past the timeout): a flaky WoW64 32-bit fault --
  the 64-bit unix dispatcher at 0x3fff66748 reads [0x8ffff8] uncommitted (gs=0x7ffd8000,
  the 32-bit/WoW64 TEB, vs the 64-bit TEB it needs) = the #12 Win32-32bit-commit family;
  AND a no-window murky state (procs at 13-24% CPU, no NSWindow per Quartz) where
  wineboot's "boot event wait timed out" (a service stalls). These are the Wine-GUI /
  WoW64 next layer, NOT the gs:0x320 crash, which is fixed.

  #28 ADDENDUM (the next wall, characterized): with gs:0x320 fixed, the GUI's remaining
  blockers are: (a) DEFAULT (no async) -> macdrv Cocoa-main-loop timeout (#16/#17/#24);
  (b) OCERZ_HOSTWQ_ASYNC=1 clears that timeout but exposes a flaky WoW64 fault: the
  64-bit unix dispatcher (0x3fff66748) reads [0x8ffff8] which is uncommitted. OCERZ_MEMLOG
  (PID-tagged) shows WHY: in ONE process, thread b2f0 COMMITS a 32-bit stack
  [0x800000,0x944000) (probe 0x8ffff8 committed=1), but thread d75e then UNMAPS the whole
  low reservation [0x1000,0x200000000) (Wine's preloader-reservation release),
  uncommitting it (probe=0) -> the later read faults. So it is a RACE: an ASYNC-spawned
  worker commits its stack BEFORE the main thread finishes releasing the preloader
  reservation; on real macOS workers do not run until after that release, so no race. The
  uncommit-on-release is faithful (real munmap unmaps everything) -- the divergence is the
  ASYNC worker running too early. So OCERZ_HOSTWQ_ASYNC must stay GATED (it advances past
  the macdrv timeout but trades it for this preloader race + a no-window murky state where
  wineboot "boot event wait timed out" because the crashed process fails). NET: gs:0x320
  is FIXED on the default path; the GUI window needs the libdispatch main-queue/source
  ownership done WITHOUT spawning workers before the preloader release -- the unified
  #16/#17/#24 + worker-ordering problem.

========================================================================
UPDATE #18 2026-06-14 -- THE INIT GATE (thread-start sequencing), make-check GREEN.
========================================================================
Implemented the unified-fix part A the #28 workflow recommended: a thread-start gate
so an ASYNC-spawned worker never runs guest code before the main thread finishes
loader-init. Faithful: it keys ONLY on the guest's own preloader-reservation munmap,
no timer/icount/usleep.

  src/mem.c   g_initgate_{m,cv}, g_init_released, ocerz_init_gate_{arm,release,wait}().
              - arm() in ocerz_mem_init / ocerz_mem_init_identity: parks workers iff
                getenv("WINEARCH") (a Wine process); native test progs (win.m, make
                check) never set it, so g_init_released stays 1 and they never wait
                -> structurally inert, make-check stays green.
              - release() at the END of ocerz_unmap when the unmap is the preloader
                release (gaddr<=0x10000 && gaddr+len>=0x100000000) = the guest's own
                [0x1000,0x200000000) munmap. NOT a timer.
  src/syscall.c  wait() at the top of ocerz_worker_entry + both HOSTWQ run points
                (ocerz_hostwq_bridge, ocerz_hostwq_queue_cb), before ocerz_vm_run_cpu.
                ocerz_fork_child calls release() (a forked child inherits the parent's
                set-up space; a re-exec'd child re-runs mem_init -> re-arms after).
  include/ocerz/mem.h  the 3 prototypes.
  Diagnostics kept (gated behind OCERZ_MEMLOG, off by default): MEMLOG now prints the
  guest rip + image (src/vm.c ocerz_current_guest_rip, src/mem.c via
  ocerz_dyld_name_for_addr; shared-cache rips show <shared-cache> since cache dylibs
  are not in the DynImage list).

VERIFIED: OCERZ_THRLOG shows "INITGATE released pid=N" fires once per process, right
after that PID's [0x1000,8GB] preloader unmap. make check fully green (clean build;
the 2 test_syscall fails were the known stale-build pattern, gone after make clean).

RESULT: the gate ELIMINATES the preloader-vs-worker variant of the 0x8ffff8 crash
(a worker committing its stack before [0x1000,8GB] decommits it). But a RESIDUAL
0x8ffff8 remains, and it is a DIFFERENT race -- root-caused this session:

  The faulting unmap is [0x700000,0x900000) (a 2MB 32-bit stack), guest rip a
  DETERMINISTIC 0x7ff802e306a2 -- and the SAME rip also issues the [0x800000,0x944000)
  stack COMMIT. Same rip for both munmap and mmap => it is a generic libsystem
  syscall() return point (shared cache, NOT Wine ntdll.so @0x381a..., NOT the
  preloader). So two threads' 32-bit stacks OVERLAP ([0x800000,0x900000)): one thread
  munmaps [0x700000,0x900000) while another has [0x800000,0x944000) live -> uncommit ->
  read 0x8ffff8 faults. This is a 32-bit-stack REUSE-after-free serialization race
  (thread A frees its low stack while thread B has reused the overlapping range),
  AFTER the gate release, so the start-gate does not cover it. The faithful fix needs
  the Wine-side CALLER (one frame up from the libsystem syscall wrapper, on the guest
  stack, in ntdll.so) to confirm whether ocerz mis-tracks the 32-bit reserve or breaks
  the alloc/free ordering Wine relies on. NEXT: dump guest rsp + walk the stack at the
  [0x700000] unmap for the ntdll.so return addr, byte-search it like #15.

ROOT CAUSE of the residual, PINNED this session via the guest-stack walk at the unmap
(OCERZ_MEMLOG now dumps Wine-.so return addrs; ntdll.so base ~0x381a04000): the
deterministic ntdll.so frame is virtual_alloc_teb(+0x7bb), the THREAD-CREATION TEB
allocator (dlls/ntdll/unix/virtual.c:4076), alongside virtual_set_large_address_space
+ server_call_unlocked (all thread-init/virtual-memory). virtual_alloc_teb reserves a
TEB block of `32 * block_size`, block_size = signal_stack_mask+1 = 0x10000 (64KB) =>
32*0x10000 = 0x200000 = EXACTLY the [0x700000,0x900000) 2MB region, at the low WoW64
`user_space_wow_limit`. So [0x700000,0x900000) IS a Wine TEB block, and the other
thread's [0x800000,0x944000) OVERLAPS it on [0x800000,0x900000). Tearing the TEB block
down (munmap) decommits the overlap -> 0x8ffff8 read faults. virtual_alloc_teb does it
all under `virtual_mutex` (server_enter/leave_uninterrupted_section), so on real macOS
the per-process TEB-block bookkeeping (teb_block/teb_block_pos) is serialized and a TEB
block never overlaps a live stack. ROOT (OPEN): ocerz's low-WoW64 placement/tracking
lets a TEB block and another low allocation overlap -- likely a wrong
`user_space_wow_limit`, or the TEB-block MEM_RESERVE not being honored by the next low
allocation, or the guest virtual_mutex not serializing the two virtual_alloc_teb calls.
NEXT: instrument the low MEM_RESERVE/commit placement + user_space_wow_limit -- a
distinct WoW64-allocator investigation, NOT the libdispatch/start-gate work. (Caveat:
the stack walk scans all committed qwords so it includes stale frames; virtual_alloc_teb
is corroborated by the exact 32*64KB=2MB match, not frame order alone.)

A VISIBLE WINDOW is still gated behind THREE distinct walls (the gate cleared a
fourth):
  1. the residual 32-bit-stack reuse race above (flaky-to-frequent; crashes explorer
     -> "no driver could be loaded" / "explorer process failed to start").
  2. macdrv "no driver could be loaded" PERSISTS even with OCERZ_HOSTWQ_ASYNC=1 -- the
     #16/#17 Cocoa/libdispatch event-source-draining wall is NOT cleared by ASYNC
     alone (and the residual crash may be masking it; fix wall 1 first to retest).
  3. FreeType: "Wine cannot find the FreeType font library" persists even with
     DYLD_FALLBACK_LIBRARY_PATH=/usr/local/lib (x86_64 libfreetype.6.dylib EXISTS
     there). ocerz's GUEST dlopen loads x86_64 dylibs itself and does NOT honor the
     host DYLD_FALLBACK_LIBRARY_PATH -- the guest dlopen search path needs to include
     /usr/local/lib (or Wine's freetype loader needs the full path).
  The gate is the keeper (default-on for WINEARCH, green). ASYNC stays an env opt-in.

========================================================================
UPDATE #19 2026-06-14 -- ★★★ WALL 1 (the 0x8FFFF8 crash) DESTROYED. One-line faithful
fix in ocerz_unmap. make-check GREEN, 0x8FFFF8 0/10 (was ~every run), no regressions.
========================================================================
ROOT CAUSE (found by disasm + OCERZ_SIGTRACE/MEMLOG/WATCH + two ultracode workflows whose
first answers were wrong and got corrected by empirical evidence):
- The fault is a GENUINE Wine heap out-of-bounds read that ocerz faithfully forwards: the
  64-bit PE ntdll.dll insert_free_block (heap.c:869) + inlined next_block (heap.c:436)
  walks a subheap's free block and reads the next block at the subheap RESERVE end
  (0x8FFFF8 = 0x900000-8), which is uncommitted. It only reaches that read because
  next_block's bound = subheap+data_size+0x38 (commit_end) is defeated by a CORRUPT
  subheap->data_size at [subheap+0x18]=[0x700018]: dumped 0x1dcfc0d01dcfc0d (garbage,
  VARIES per run). With the correct data_size=0xffc0, commit_end=0x710000, next+8=0x900000
  > commit_end => next_block returns NULL => NO fault. So the bug is a CORRUPT data_size,
  not a missed MEM_COMMIT and not a gs/#28 bug (gs=0x7ffd8000 is the correct committed TEB).
- WHO corrupts data_size: NOT a guest store (OCERZ_WATCH=0x700018 caught only Wine's two
  correct 0xffc0 writes, rip 0x3fff6b6c0; the faulting PID's last store before the fault
  was 0xffc0, then garbage with NO store between). The corruptor is a STALE MAP_SHARED
  page: OCERZ_MEMLOG shows [0x700000,0x701000) was a `shared` (MAP_SHARED wineserver/file
  overlay) before the heap reused the low address. ocerz_unmap decommitted it via
  mprotect(RW)+memset(0)+mprotect(NONE) -- which on a READ-WRITE shared page SUCCEEDS
  without converting it to private, so the page STAYS MAP_SHARED. When the heap re-commits
  [0x700000] and writes its SUBHEAP header there, data_size at [0x700018] is silently
  shared with another process whose wineserver writes stomp it (explaining: stale/varying
  garbage, no guest store, and that the later-written [0x700030] user_value + [0x700038]
  free block -- past the contended region -- survive correct). The low-shadow is PRIVATE
  per process (mach_vm_allocate), so the corruptor is another process writing the same
  wineserver SHARED file, not a shared arena.
THE FIX (src/mem.c ocerz_unmap, faithful, ~one statement): replace the per-page
mprotect(RW)+memset+mprotect(NONE) (and its RO-overlay fallback) with an UNCONDITIONAL
`mmap(hp, OCERZ_HOST_PAGE, PROT_NONE, MAP_ANON|MAP_PRIVATE|MAP_FIXED, -1, 0)` for every
committed host page. This is exactly what the macOS kernel does for munmap-then-anon-remap:
the old physical page (incl. a MAP_SHARED overlay) is DISCARDED and replaced by a fresh
PRIVATE zero-fill-on-demand reservation, so a later heap commit of the reused address is
PRIVATE (no cross-process stomp) and reads zero (the zero Wine's create_subheap/
block_init_free assume). It SUBSUMES the old #15 RO-file-overlay fallback. No timer/poke.
VERIFIED: 0x8FFFF8 0/10 (ASYNC) and 0/3 (default path), gs:0x320 0/4, ocerz-fatal 0/4,
winedbg 0/3, make-check FULLY GREEN. Wine now runs PAST wall 1 (no crash, process stays
alive) into wineboot SERVICE STARTUP (services.exe/PlugPlay), where the remaining walls are
the SEPARATE, pre-existing ones: FreeType-not-found (guest dlopen doesn't honor host
DYLD_FALLBACK_LIBRARY_PATH) + the winemac.drv/macdrv Cocoa display driver (#16/#17). KEPT
gated diag (off by default): OCERZ_SIGTRACE now also dumps GPRs + mem[rsi/r12/r11]; MEMLOG
prints guest rip+image + a Wine-.so stack walk + OCERZ_LOWLOG range mode; OCERZ_WATCH logs
pid (src/vm.c, src/mem.c). notes/wine_bringup.md UPDATE #19.

========================================================================
UPDATE #20 2026-06-14 -- NEXT WALL root-caused (not yet fixed): wineboot service
startup deadlocks on a LOST EVFILT_USER kqueue ALERT. make-check GREEN; wall 1 (#19)
stays fixed.
========================================================================
With wall 1 fixed, Wine no longer crashes in boot and runs FURTHER -- to wineboot
service startup -- then HANGS (idle 0% CPU, no crash, no ocerz fatal; "run_wineboot boot
event wait timed out"; service PlugPlay "failed to start"). Reproduces on BOTH the default
and OCERZ_HOSTWQ_ASYNC paths -> not async-specific, not the #18 gate (all PIDs released the
gate), not gs/#28, not the #19 unmap fix.
EXACT STALL: winedevice.exe (hosting the MountMgr driver) calls
wine_enumerate_root_devices(MountMgr) (ntoskrnl pnp.c:1501) -> SetupDiGetClassDevsW(ROOT,
DIGCF_ALLCLASSES) -> loads setupapi.dll -> blocks. macOS `sample`: the stuck thread sits
in Wine's NtWaitForAlertByThreadId (ntdll sync.c); other threads in mach_msg; idle
workqueue thread.
ROOT CAUSE (decisive, via OCERZ_KEVLOG which logs every kevent + its EVFILT changes/events,
plus a new KEVWAIT-ENTER pre-syscall log for blocking waits): Wine's per-thread thread-id
alert on macOS is a kqueue with an EVFILT_USER filter (sync.c get_tid_alert_entry: kqueue()
+ kevent EV_ADD|EV_CLEAR ident=1; NtAlertThreadByThreadId = kevent NOTE_TRIGGER;
NtWaitForAlertByThreadId = kevent wait). In the stuck process the alert kq=12 gets the
EV_ADD registration + FIVE NOTE_TRIGGER wakeups (fflags=0x1000000, ret=0 each) and the
waiter blocks in kevent(kq=12,...) -- BUT **ZERO EVFILT_USER events are ever delivered**
(0/5). So NtWaitForAlertByThreadId never wakes on the alert; it only ever returns via its
TIMEOUT and re-checks, which makes ALL Wine thread-alert sync (SRW/condvars/loader/the PnP
start handshake) fall back to slow timeout polling -> the boot can't finish within the 30s
service-start timeout -> deadlock-by-slowness.
ocerz forwards kevent(363)/kqueue(362) DIRECTLY to the host kernel (NULL handler, the
changelist/eventlist/timeout pointers g2h-translated); ocerz_host_syscall does NOT retry on
EINTR (single svc), so the simple EINTR+EV_CLEAR theory is OUT. Yet a directly-forwarded,
process-shared host kqueue still loses the cross-thread NOTE_TRIGGER -> EVFILT_USER
delivery. NEXT (the fix): find why the host kqueue's EVFILT_USER trigger is not seen by the
concurrent kevent waiter under ocerz's execution model -- candidates: the kqueue fd is not
truly shared across the guest's threads (per-thread fd remap), the blocking kevent runs in
a context (Rosetta/ocerz thread) where the kernel doesn't deliver, an EV_CLEAR/ordering
race in how ocerz issues the two kevents, or fd-reuse (two kqueue() both returned fd 12).
KEPT gated diag (off by default): OCERZ_KEVLOG now also prints KEVWAIT-ENTER (kq + num +
timeout for blocking infinite/nev==1 kevent waits, so a wait that never returns still shows
its kq). notes/wine_bringup.md UPDATE #20.

========================================================================
UPDATE #21 2026-06-14 -- ★★★ WALL 2 DESTROYED: the wineboot service-startup deadlock.
ocerz sys_pthread_kill ignored the TARGET thread; the wineserver's cross-thread
SIGUSR1 kick was dropped. Fixed faithfully. make-check GREEN, no regressions; the
boot now runs PAST service startup to explorer/window-creation.
========================================================================
ROOT CAUSE (workflow w4pmwev0s, 4 understand + synth + 3 adversarial verify; CONFIRMED
empirically by a new OCERZ_PTKILL probe): ocerz's sys_pthread_kill (src/syscall.c)
implemented __pthread_kill(mach_port_t target_thread, int signo) by IGNORING a[0]=target
and synthesising the signal on the CALLING thread (ran guest_sigact[signo].handler via
ocerz_vm_call, then *cpu=saved). The real macOS __pthread_kill delivers an async signal to
the NAMED thread (interrupting its current syscall). Wine's wineserver relies on exactly
this: send_thread_signal (server/mach.c:364-391) does mach_port_extract_right(client_task,
unix_tid) + __pthread_kill(client_thread_port, SIGUSR1) to kick a CLIENT thread into
re-selecting for a SYSTEM APC / context capture / suspend (queue_apc thread.c:1463-1466,
stop_thread/get_thread_context thread.c:2227). Under ocerz that SIGUSR1 ran on the wrong
thread and VANISHED. During MountMgr root-device enumeration (winedevice ZwLoadDriver ->
wine_enumerate_root_devices -> SetupDiGetClassDevsW), the wineserver needed to deliver such
a kick to the winedevice service-control dispatcher thread; it was lost; that thread sat in
read(wait_fd)/kevent forever; the lock it held stalled the enumeration thread (seen in
NtWaitForAlertByThreadId); the whole boot hard-deadlocked (0% CPU). PROBE PROOF: with
OCERZ_PTKILL, during the hang the wineserver calls __pthread_kill(target!=self, signo=30
[SIGUSR1]) and signo=3 [SIGQUIT] -- the exact cross-thread kicks -- which ocerz dropped.
(The standalone kqueue EVFILT_USER alert works in isolation; the wineserver processes
requests fine; so it was NOT a lost kqueue wakeup, NOT a SIGSYS/EINTR/EV_CLEAR race, NOT a
Wine lock-ordering bug -- it was the dropped cross-thread signal.)
THE FIX (faithful, mirrors the macOS __pthread_kill contract; no timer/poke):
  - src/syscall.c sys_pthread_kill: FORWARD the real __pthread_kill(a[0], a[1]) to the host
    kernel (ocerz_host_syscall(328,a)), so the kernel delivers signo to the thread named by
    the port -- incl. cross-process (the extracted client thread port). Replaces the
    same-thread synthesis.
  - src/vm.c: a per-thread g_pending_async_mask + async_sig_handler installed for SIGUSR1
    (when OCERZ_RIPDUMP is off) and SIGQUIT, with NO SA_RESTART so the target's blocked
    forwarded syscall returns EINTR; the handler only records the signo (async-safe).
    ocerz_take_pending_async_sig() takes+clears the mask.
  - src/syscall.c ocerz_handle_syscall: after a class-1/2/3 dispatch returns OCERZ_STEP_OK,
    deliver any pending async signal via ocerz_signal_deliver -- the guest handler (Wine's
    usr1_handler) runs at the syscall-return boundary, re-enters select, picks up the system
    APC, and the boot proceeds. This is the macOS "async signal delivered at the next user
    boundary" contract.
RESULT: 3/3 runs the boot RELIABLY progresses past PlugPlay/MountMgr to explorer/window-
creation (was: PlugPlay "failed to start", never reached explorer, 0% hard-freeze). PlugPlay
no longer fails. make-check GREEN; 0x8FFFF8 (#19) 0, gs:0x320 (#28) 0, ocerz-fatal 0.
NEXT WALLS (separate, pre-existing): (1) explorer "no driver could be loaded" = the
winemac.drv/macdrv Cocoa display-driver wall (#16/#17) + FreeType; (2) a ~15-30s busy phase
(workflow fix #2: commit_range holds the single global map_lock across an O(pages) mprotect
loop, serialising all threads' loader/enumeration commits) trips wineboot's 30s boot-event
timeout -- throughput, not a deadlock. KEPT gated diag: OCERZ_PTKILL, OCERZ_MACHMSG (+the
KEVWAIT-ENTER in OCERZ_KEVLOG). notes/wine_bringup.md UPDATE #21.

========================================================================
UPDATE #22 2026-06-14 -- FreeType "no driver" wall FIXED (bare-soname dlopen search);
make-check GREEN. The boot now loads FreeType + winemac.drv and reaches the GUI, which
EXPOSES the next layer: the gs:0x320 dispatcher residual + a vprot-table OOM.
========================================================================
FREETYPE FIX (src/dyld.c): ocerz's guest dlopen passed a BARE soname (no slash, no @)
through VERBATIM, so Wine's runtime dlopen("libfreetype.6.dylib")/dlopen("libMoltenVK.dylib")
never searched the macOS dylib fallback paths and failed ("Wine cannot find the FreeType
font library", x5). Added resolve_bare_soname() + try_soname_in_pathlist(): for a bare
soname, search DYLD_LIBRARY_PATH then DYLD_FALLBACK_LIBRARY_PATH (or its macOS default
$HOME/lib:/usr/local/lib:/usr/lib), wired into ocerz_dlopen_inner before dlopen_load_image
(re-dedups against the resolved path). The x86_64 homebrew libfreetype.6.dylib is at
/usr/local/lib, now found WITHOUT even setting DYLD_FALLBACK (the default covers it).
RESULT: "Wine cannot find the FreeType font library" 0 (was 5+); "no driver could be loaded"
0 (was present) -- FreeType was the cascade that failed win32u's font init -> the graphics
driver load. winemac.drv (PE) maps; FreeType resolves. make-check GREEN.
NEXT LAYER (exposed by the new progress; the actual blockers to a window now):
 (1) gs:0x320 DISPATCHER RESIDUAL (the #26/#27/#28 foreign-worker family): a thread enters
     __wine_syscall_dispatcher via the guest signal trampoline (0x7ff802e7d3a0) and its
     gs-restore epilogue (guest_rip=0x381a37f23 = ntdll.so `mov rax,[r14]; mov rdi,[rax+
     0x320]`) reads [r14]=0 -> rax=0 -> deref [0x320] faults; gs=cthread (0x3a1aa40e0, NOT a
     TEB). It LOOPS (blockhist: dispatcher<->trampoline x4) until ocerz's consecutive-fault
     guard makes it fatal. This is the rarer rax=0 sub-case #27 flagged (faults BEFORE the
     machdep/SIGSYS gs-fix edge, so #28's fix-C/gs-keep can't catch it). NOTE the #21 async
     signal delivery now reaches more threads -- if a thread that lacks a saved Wine TEB at
     [r14] is signalled and runs Wine's handler -> the dispatcher, it hits this. NEXT:
     determine whether [r14]=0 is a foreign worker (no TEB at all) or a real Wine thread
     whose TEB slot the async-delivered signal frame didn't populate; the faithful fix is
     the #16/#17 marshaling (don't run the dispatcher on a no-TEB thread) OR ensure the
     signal-delivery frame carries the thread's TEB at [r14].
 (2) vprot-table OOM: "alloc_pages_vprot anon mmap error Cannot allocate memory for vprot
     table, size 00100000" -- Wine fails to mmap the 1MB per-process 32-bit vprot table.
     Either arena exhaustion from the homebrew FreeType dep chain, or a consequence of the
     gs:0x320-crashed thread leaving the arena inconsistent. Characterize which.
ALSO STILL OPEN (UPDATE #21 next-walls): the ~15-30s busy phase (a guest libsystem loop
iterating the 8GB low reservation -- rdx=0x200000000 -- doing many mmap/mprotect; trips the
30s boot-event timeout). notes/wine_bringup.md UPDATE #22.

========================================================================
UPDATE #23 2026-06-14 -- ★★★ gs:0x320 ROOT CAUSE FOUND (empirically, overturns #22(1) and
the entire #26/#27/#28 "dispatcher gs-restore" theory). gs:0x320 was always the AMPLIFIER,
not the disease. Real disease: the HOSTWQ EVFILT_MACHPORT event bridge. Fix in progress.
========================================================================
Method: chose option (a) (tackle the gs:0x320 core). Empirics-first (we mis-diagnosed twice
before). Added cheap env-gated diag (OCERZ_WSIG): logs every guest-signal delivery to a
NO-TEB worker (sig_altstack_sp==0 && gs not TEB-band), with source tag (g_ocerz_deliver_src:
0=fault,1=#21-async,2=class0-SIGSYS), faddr, code, handler; plus OCERZ_WSIG dumps the events
ocerz_hostwq_bridge hands a worker. Symbolicated cache addrs via the cache .map file
(/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64.map;
unslid base 0x7ff800000000) and disassembled libdispatch via a tiny x86_64 dladdr helper
(/tmp/dispdis.c -> dli_fbase+RVA).

THE FULL CAUSAL CHAIN (every link confirmed from a live notepad run):
 1. ocerz brings up a HOSTWQ workqueue WORKER (gs=cthread pth+0xe0, NO Wine TEB, altsp=0).
 2. The worker drains an EVFILT_MACHPORT event (filt=-8) the host kernel handed it:
      HOSTWQ-EV[0] filt=-8 flags=0x185 ident=0x250b udata=0x600001705d40
                   data=0 ext0=0x17019ab00 ext1=0x78
    ext0 = the Mach receive-message buffer ptr, ext1=0x78 = its size.
 3. Guest libdispatch runs `_dispatch_kevent_mach_msg_recv` (cache rip=0x7ff802cdacc4 =
    libdispatch __TEXT+0x21cc4; faulting insn `mov r12d,[rdx+4]` = read msgh_size at hdr+4,
    rdx = ext0 = 0x17019ab00) and faults: WSIG sig=11 src=0 faddr=0x17019ab04 (=ext0+4)
    code=1 (SEGV_MAPERR, UNMAPPED IN GUEST). i.e. the Mach msg buffer ext0 is NOT a guest-
    readable address -> deref garbage.
 4. ocerz delivers SIGSEGV to Wine's segv_handler (guest sa->handler=0x381a37ef0). The
    handler's init_handler()->get_current_teb() does `mov r14,rsp; and r14,~0xffff; mov
    rax,[r14]; mov rdi,[rax+0x320]` -- but the worker's stack base is not a 64KB Wine TEB
    block, so [r14]=0 -> rax=0 -> deref [0x320] FAULTS AGAIN. ocerz re-delivers SIGSEGV ->
    re-faults, rsp dropping ~0xd80 per iter (eating stack) until the consecutive-fault guard
    fires -> the "guest crash SIGBUS guest_addr=0x320" we'd been chasing.

SO: the gs:0x320 crash is NOT the WoW64 syscall dispatcher, NOT the #21 async wineserver
kick (src=0 = a genuine fault, NOT src=1), NOT a missing TEB at a dispatcher [r14] slot. It
is Wine's segv_handler being UNABLE TO RUN on a no-Wine-TEB libdispatch worker, recursively
re-faulting on a PRIOR fault. The prior fault is the real bug:

ROOT (Bug A, the disease): under OCERZ_HOSTWQ, sys_kevent_id forwards the guest's kevent_id
to the host kernel (syscall 375) translating only the TOP-LEVEL pointer args (changelist,
eventlist, data_out, data_available) -- it NEVER translates the per-kevent ext[] fields. For
an EVFILT_MACHPORT MACH_RCV source, ext[0]=receive-buffer ptr / ext[1]=size; the kernel does
the Mach receive and returns ext[0] = a buffer address that is NOT mapped in the guest.
ocerz_hostwq_bridge memcpy's the kevent verbatim (OCERZ_KEVENT_QOS_S=0x40) into the guest
worker, so guest libdispatch reads the unmapped ext0 -> fault. (NOTE udata=0x600001705d40 is
a 0x6000.. nano-malloc addr -- ambiguous host/guest since the guest libmalloc nano-zone maps
at the same fixed base; the DECISIVE fact is code=1: ext0 is unmapped in the guest regardless
of origin.) This is the #16/#17/#24 "libdispatch ownership / HOSTWQ event bridge" wall,
localized to EVFILT_MACHPORT message-buffer handling.

MECHANISM REFINED (more diag): the EVFILT_MACHPORT source is registered with ext0=0/ext1=0
(KEVREG filt=-8 flags=0x385 fflags=0x7000a0e ext0=0 ext1=0) -- the guest does NOT provide a
receive buffer; fflags requests a KERNEL-ALLOCATED receive. On delivery the kernel returns
ext0=0x170aaab00 ext1=0x78 = a buffer the host kernel allocated IN OCERZ'S HOST ADDRESS SPACE
(the workqueue worker is a host thread). The guest reads ext0 as a GUEST addr -> unmapped.
Also confirmed: ocerz + host SHARE ONE MACH PORT NAMESPACE (dispatch_mach case 47 passes
msgh_remote/local port NAMES through untranslated, translating only the buffer ptr a[0]=g2h
and specific vm-reply OOL relocations via mig_vm_reply_relocate). So port names inside the
received message need NO translation.
FIX DIRECTION (faithful, owner rejects bandaids):
 - Bug A: it is NOT a registration ext[] translation (ext0=0 at reg). It is DELIVERY-side
   Mach-message bridging in ocerz_hostwq_bridge (and ocerz_spawn_workloop_worker, which also
   copies events): for each EVFILT_MACHPORT event with ext0!=0, the kernel-received message
   lives at host VA ext0 (size from msgh_size/ext1, host-readable). Copy it into GUEST memory
   and rewrite the guest kevent ext0 -> the guest copy, so guest libdispatch's
   _dispatch_kevent_mach_msg_recv reads the message in the guest AS. Open lifecycle/correctness
   points to resolve BEFORE coding (corruption risk): (i) WHO frees the buffer -- does guest
   libdispatch vm_deallocate(ext0,ext1) after processing? If yes the guest copy must be a real
   guest vm_allocate'd region it can free (NOT carved from the reused worker region, or its
   free unmaps part of the region); and ocerz must free the HOST kernel buffer (vm_deallocate
   the memory only, NOT mach_msg_destroy -- shared namespace, the rights must persist). (ii)
   COMPLEX messages (MACH_MSGH_BITS_COMPLEX bit in msgh_bits) carry OOL/descriptor host
   addresses needing relocation like mig_vm_reply_relocate. (iii) a per-worker recv-buffer
   pool to avoid arena bump-exhaustion. Being designed + adversarially verified before coding.

   RESOLUTION (same day) -- ★★★ FIXED + VERIFIED, make-check GREEN, notepad now builds its GUI.
   Implemented ocerz_bridge_mach_msg() (src/syscall.c) called from ocerz_hostwq_bridge
   (DEFAULT-ON; OCERZ_NO_MACHBRIDGE to A/B): copy the kernel's host-allocated message (ext0,
   sz=ext1) into a fresh guest mapping and rewrite ext0; for COMPLEX messages walk descriptors
   and relocate OOL/OOL_PORTS/OOL_VOLATILE (types 1/2/3) into guest memory (PORT type 0 = name
   only, shared namespace, no reloc). A/B (clean slate): NO bridge -> machport_ev=3 gs320=3
   wfault=3; WITH bridge -> machport_ev=10 gs320=0 wfault=0. Observed COMPLEX msgs carry only
   PORT descriptors (decoded type+11=0x00, names 0x2a03/0x2213) so verbatim is faithful; OOL
   reloc is completeness (NOT exercised here -> code-review-only). Lifecycle VERIFIED: gbuf
   addresses RECYCLE -> guest vm_deallocate()s the copy (kernel allocs, app frees, like macOS).
   Two next-walls the bridge exposed also fixed: BSD 142 gethostuuid (host-forward ptr_flags
   0x03) and Mach trap 11 mach_vm_purgable_control (ocerz never purges -> SUCCESS + NONVOLATILE).
   ★ RESULT: `bin/wine notepad` runs PAST gs:0x320 to notepad's OWN GUI -- creates the Edit
   control (1028x727 text area: hwnd 0x20052 visible (0,0)-(1028,727)), status bar, IME windows.
   ZERO ocerz fatals (only the pre-existing non-fatal init-skip UD2). make-check GREEN.
   NEW WALLS reached (separate): (1) err:ole:start_rpcss "Failed to start RpcSs service" then
   tree exits; (2) winemac.drv on-screen NSWindow stage (no macdrv trace yet -- the NSWindow for
   main window 0x1004a is the next frontier; the bridge should now let macdrv_start_cocoa_app's
   main loop run, undoing the #16/#17 timeout). OPEN ROBUSTNESS (code-review follow-ups, not
   blocking): OOL-reloc path untested; the host kernel buffer (+ relocated host OOL) is not
   vm_deallocate'd after copy -> small leak; the synthetic ocerz_spawn_workloop_worker path
   (OCERZ_KEVENT_WORKER) has the same latent un-bridged-Mach bug. KEPT diag: OCERZ_WSIG.

   ADVERSARIAL VERIFICATION (workflow, 5 agents / 4 lenses) + ROBUSTNESS HARDENING applied:
   the panel CONFIRMED the core fix correct (ABI/stride, nev>1 ordering, gethostuuid ptr_mask,
   purgable_control NONVOLATILE all verified). REAL gaps found -> FIXED (make-check GREEN; deep
   re-run: machport_ev=10 bridge=10 COMPLEX=5 gs320=0 wfault=0, notepad Edit 1028x727 still
   created): (1) ★ the SYNCHRONOUS kevent_id(#375) drain path (a worker re-draining its workloop
   gets MACHPORT events back with host ext0) was UN-bridged = same crash in another reachable
   path -> now bridges the returned eventlist in sys_kevent_id; (2) GUARDED_PORT (desc type 4)
   aborted the descriptor walk (could leave a later OOL un-relocated) -> now advances 16 and
   continues; (3) fail-unsafe: on map_anywhere failure the call site now nulls ext0 (never leaves
   a host pointer), and an OOL reloc failure delivers an EMPTY OOL. OVERRODE the panel's #1
   ("monotonic arena leak -> use a pooled slab"): its premise is refuted by live evidence (gbuf
   addresses RECYCLE = the guest vm_deallocate's the copy), and its proposed slab-in-worker-region
   would CORRUPT that region (the guest frees the buffer) -- the current per-message map_anywhere
   is correct. DEFERRED (verifier-classified follow-up): freeing the host kernel buffer + OOL
   backings (slow host-AS leak) -- held until validatable at the deep phase without destabilizing;
   and bridging plain kevent(#363) + the synthetic ocerz_spawn_workloop_worker path.
 - Bug B (latent amplifier): Wine's segv_handler cannot run on a no-Wine-TEB worker (any
   fault on such a worker becomes a fatal gs:0x320 loop, not just this one). Real macOS Wine
   doesn't hit this (no PROT_NONE arena -> no commit faults; no mistranslated buffers). Decide
   whether to also faithfully harden ocerz's fault path for no-TEB workers.
DIAGNOSTICS KEPT (env-gated, cheap): OCERZ_WSIG (worker-signal + HOSTWQ-EV dump),
g_ocerz_deliver_src source tags. Tree builds; make-check to be re-verified after the fix.
KEY ANCHORS: ntdll.so load base 0x381a04bac; segv_handler/init_handler @ ntdll.so vmaddr
~0x33344-0x33377 (nm mislabels it _signal_init_process due to missing asm syms);
libdispatch _dispatch_kevent_mach_msg_recv @ __TEXT+0x21caf (fault +0x21cc1); guest _sigtramp
= libsystem_platform __TEXT+0x33a0 = 0x7ff802e7d3a0. notes/wine_bringup.md UPDATE #23.

========================================================================
UPDATE #24 2026-06-14 -- BUSY-PHASE / BOOT-SLOWNESS WALL CHARACTERIZED (the ~100s-per-process
boot that starves service startup -> RpcSs fails -> notepad exits after creating its windows).
It is NOT a compute loop (the old "8GB mmap/mprotect loop" guess is WRONG); it is WAITING.
========================================================================
Method: after the gs:0x320 fix, notepad creates its GUI then the tree exits. WINEDEBUG=
+timestamp,+service,+process showed the smoking gun: NtCreateUserProcess -> child actually
created = ~104 SECONDS (292983.4->293087.6), and services.exe's process_send_command TIMES OUT
waiting on these glacial spawns -> RpcSs never comes up -> notepad's COM init fails -> exit.
So the root wall is ~100s PER PROCESS CREATION, which serially starves the boot.
WHERE the time goes (new OCERZ_SCCOUNT profiler: per-class-2-syscall histogram + wall-clock
timing of each dispatch_bsd call, logs any >20ms call as SCSLOW; cached getenv, ~0 overhead off):
the busy phase is 100% in BLOCKING WAITS, not CPU:
  num=3  read         33 calls  ~86s total  (single reads block up to 14.7s, dicount=0 = pure
                                              block) -- caller ntdll.so 0x381a30e2a = Wine's
                                              read(wait_fd) wineserver server-reply wait.
  num=544 ulock_wait2  5 calls  ~15s total  (EXACTLY 5000ms each = a 5s TIMEOUT that repeats =
                                              a LOST WAKEUP) -- caller libsystem 0x7ff802e7fe69
                                              = a libdispatch/pthread futex wait on lock addr
                                              0x3a001c04c.
  num=27 recvmsg 3.7s; num=363 kevent 0.5s; num=197 mmap 0.37s (all minor).
Both read(3) and ulock(515/516/544) are host-forwarded with CONSISTENT g2h(addr) key translation
(ptr_mask 0x02), so these are GENUINE waits, not an ocerz key-mismatch -- the things being
waited ON are slow. So the boot is a CASCADE: wineserver round-trips block for seconds, and a
fresh process's init does many round-trips, and the boot creates many processes -> ~100s each.
TWO concrete sub-walls to attack:
 (1) ulock_wait2 5s-timeout LOST WAKEUP: keys match, yet the wake never arrives in 5s. Likely
     the UL_UNFAIR_LOCK / adaptive-ulock OWNER semantics -- the wait value encodes the owner's
     thread port and the host kernel owner-boosts / requires the wake from the owner; ocerz's
     guest->host thread-port identity in the ulock value may mismatch (cf. the existing
     "Owner in ulock is unknown" note at syscall.c:81-83). Tractable + code-readable.
 (2) wineserver round-trip LATENCY (the dominant 86s): is the server slow to PROCESS (its
     poll/select/kevent main loop over client fds, per-syscall overhead x frequent polling) or
     slow to SIGNAL (wait_fd write delayed)? Needs LIVE profiling of the wineserver process
     (a separate ocerz guest process) -- the next concrete experiment.
This is a deep wineserver-IPC + ulock SYNC-PERFORMANCE wall, a different class from the memory
bugs (#19/#21/#23). KEPT diag: OCERZ_SCCOUNT (SCSLOW per-call timing + SCCOUNT histogram).
make-check GREEN. An analysis workflow on (1)+(2) was launched. notes UPDATE #24.

UPDATE #24b -- ROOT REFRAMED (analysis workflow + CPU profiling). The 86s read(wait_fd) is NOT
a server bug: the workflow confirmed the wineserver processes each request in one read+handler+
write (server/request.c:327) and signals via a single non-blocking write(wait_fd) (server/
thread.c:1215) = instantaneous; ocerz forwards read/kevent/ulock faithfully (no key mismatch,
no EINTR-swallowing). The latency is in PRODUCING the wakeup CONDITION: each spawned guest
process is genuinely SLOW TO REACH "ready", and the boot starts ~7 processes SERIALLY, and Wine
has a HARDCODED 10s deadline (programs/services/services.c:44 service_pipe_timeout=10000ms, used
in rpc.c:1204/1222 WaitForSingleObject) -> a child that needs >10s makes the SCM declare the
service failed -> RpcSs fails -> notepad COM init fails -> exit. (Also: server/fd.c:355 caps the
server's kevent timeout at 16ms when KUSER_SHARED_DATA is mapped -> ~62Hz spin per process, a
steady emulation tax, not the seconds-latency.)
WHAT makes per-process startup slow = CONFIRMED COMPUTE-BOUND by direct measurement: ps showed
cold-start processes burning 9.65s CPU/13s (74%) and 100% CPU; idle processes just wait on them.
CPU profile (sample) of a hot process under ocerz_dyld_run: ~all CPU is EXECUTING GUEST STARTUP
CODE through the engine -- JIT buffer (??? @0x109..) ~489, ocerz_jit_exec_one ~620, ocerz_interp_
exec ~300 (~25% -- run-once startup blocks that never get hot enough to JIT -> interpreted slow),
ocerz_read_op (decode) ~61. NO dyld-closure/selpool/memcpy hotspot (the #17 selopt perfect-hash
held). So each process pays ~5-10s CPU re-translating + running its own ntdll+DLL init. This IS
the project's core PERFORMANCE gap, now localized: serial boot x per-process JIT/interp cold-start
compute (+cascade waits) ~= 100s > the 10s service deadline.
PRIORITIZED PATH FORWARD (deep, perf-class -- next focused effort, NOT a quick patch):
 (A) cut per-process cold-start CPU -- the dominant lever. Candidates: a JIT translation cache
     SHARED across processes (ntdll/Wine code is identical in every process -> translate once);
     better tiering / JIT cold startup code sooner (the ~25% interp tax); faster decode.
 (B) ulock owner-port lost-wakeup (workflow Agent A, ~15s, medium-confidence): a HOSTWQ worker
     mach_port_deallocate's its thread port while it is still the ulock OWNER word -> kernel
     can't resolve the dead owner -> 5s ETIMEDOUT. Fix = keep the worker port alive until pthread
     reap. CONFIRM FIRST with owner-resolution instrumentation (don't perturb the Mach-bridge).
 (C) reduce boot serialization (start independent services concurrently) -- mitigation.
make-check GREEN. SCCOUNT diag kept (gated). notes UPDATE #24b.

========================================================================
UPDATE #25 2026-06-14 -- ★ RENDERING BLOCKER FOUND + FIXED: winemac.drv "no driver could be
loaded" was caused by ARENA EXHAUSTION (the deferred #22 vprot-OOM). Enlarging the arena
4GB->8GB cleared it. make-check GREEN. (Goal: render a full Wine app window.)
========================================================================
GOAL set: make ocerz RENDER a full Wine application window (visible notepad). After the gs:0x320
fix notepad builds its Win32 windows but NOTHING RENDERS. WINEDEBUG=+macdrv,+win showed why:
  err:virtual:alloc_pages_vprot anon mmap error Cannot allocate memory for vprot table, size
    00100000
  err:winediag:nodrv_CreateWindow Application tried to create a window, but no driver could be
    loaded.  / L"The graphics driver is missing. Check your build!"
So winemac.drv FAILS TO LOAD (a 1MB anon mmap for Wine's 32-bit vprot table fails) -> "no driver"
-> no Cocoa NSWindow -> no render. (notepad's Win32 main window 0x1004a 2041x1539 IS created;
it just has no display driver.) ROOT (OCERZ_OOMLOG diag added to ocerz_map_anywhere): the arena
= [0x300000000,0x400000000) = 4GB, but ocerz_map_anywhere (mem.c:578) only bumps the UPPER HALF
(bump_next starts at lo+size/2), so map_anywhere has only ~2GB; bump_next hit 0x3ffffc000 (16KB
below arena_hi), islands only 18MB -> NOT island-stranding, NOT the Mach-bridge (~10 msgs/~160KB),
just genuine ~2GB of Wine address-space reservations (DLLs+heaps+mmaps) filling the 2GB bump pool.
FIX (src/dyld.c:226): DYN_ARENA_SIZE 4GB -> 8GB (bump pool 2GB -> 4GB). Faithful, not a band-aid:
a real x86_64 macOS process has a vast AS; ocerz's 2GB bump arena was simply under-provisioned.
RESULT: vprot-OOM 0 (was 14), MAPOOM 0, "no driver could be loaded" 0 (all cleared). make-check
GREEN (test_syscall 96/0, guest 14/14 x2, diff 15, dyn 2). winemac.drv can now LOAD. KEPT diag:
OCERZ_OOMLOG (arena state at map_anywhere exhaustion). NEXT toward render: confirm winemac.drv's
Cocoa side creates the on-screen NSWindow (the Mach-bridge fix should have unblocked the old
#16/#17 Cocoa-main-loop wall) AND that the process survives the boot (#24 perf/RpcSs). notes #25.

========================================================================
UPDATE #26 2026-06-14 -- gs:0x320 BOUNDED RECOVERY (Bug B) so it stops KILLING the process; the
8GB arena lets the boot run ~30x deeper (icount ~6M -> ~200M) and exposes the NEXT walls. Still
no on-screen NSWindow. make-check GREEN. (Goal: render a Wine window.)
========================================================================
After #25 (8GB arena -> winemac.drv loads), the boot reaches deep (~200-280M instructions) and
re-hits gs:0x320 at the NEW arena addresses (e.g. ntdll.so now ~0x401a04bac), confirming the
Mach-bridge (#23) fixed only ONE trigger -- gs:0x320 is the AMPLIFIER for ANY fault on a no-Wine-
TEB worker (Wine's segv_handler can't run -> get_current_teb reads no TEB -> repeated ~0x320
near-null deref -> the loop guard makes it FATAL, killing the whole process).
FIX (src/vm.c crash_handler, Bug B BOUNDED RECOVERY): when the loop guard trips AND the thread is
a no-Wine-TEB worker (sig_altstack_sp==0) AND the looping fault is near-null (gaddr<0x10000 = the
get_current_teb garbage), set cpu->terminated=1 + siglongjmp(*g_sig_recover) -> the worker's main
VM loop (vm.c:1016 `while(!exited && !terminated)`) exits, ocerz_vm_run_cpu returns, the HOSTWQ
host thread parks. So just THAT worker's dispatch callback is abandoned and the PROCESS SURVIVES
(instead of dying). Never fires for a real Wine thread (it has an altstack; legit faults aren't
near-null-looping). On real macOS such a worker never faults -- this is an ocerz-artifact bridge
(owner has accepted similar #27/#28 bounded bridges). make-check GREEN.
NEXT WALLS the 8GB depth EXPOSES (toward render, under investigation):
 - "nested fault inside crash handler": a REAL Wine thread (TEB gs=0x7ffd8000 + altstack) faults
   at ntdll.so ~0x401a338e0 with comm(addr)=-1 (addr not in a registered region -> looks like a
   use-after-unmap of code, OR an 8GB-arena region-registration gap), and the signal-frame build
   (on altsp~0x100000858) faults again -> fatal. Being captured cleanly (a prior run had
   OCERZ_SIGTRACE accidentally on, perturbing timing).
 - macdrv Cocoa NSWindow STILL not reached (0 macdrv trace) -- the boot doesn't get far enough /
   a thread crashes first. winemac.drv LOADS now (no "no driver"), so once a process survives to
   create its top-level window, macdrv_create_cocoa_window should run. The #24 boot-perf wall
   (serial ~100s boot tripping Wine's 10s service deadline) still gates full completion.
KEPT diag: OCERZ_OOMLOG, OCERZ_SCCOUNT, OCERZ_WSIG (all gated). notes UPDATE #26.

UPDATE #26b -- ARENA SIZING REVISITED (the #25 8GB enlargement had a bad side effect). DYN_ARENA
_SIZE=8GB makes mmap land the arena TENS OF GB HIGH (~0xb24..-0xbf5.. = ~48GB), even with an
mmap hint of OCERZ_LOW_LIMIT (kernel ignores the advisory hint for an 8GB anon reservation). At
that high base a thread DETERMINISTICALLY diverges to an UNMAPPED arena address (guest_rip==
guest_addr==arena_base+CONST, CONST low bits 0xa338e0, comm(addr)=-1 = not in any region;
host_insn=0x38696806 = the JIT's byte-load reading the unmapped target during translate()), then
the signal-frame build ALSO faults -> "nested fault inside crash handler" -> that process dies
(~10-200M icount, intermittent). The known-good low arena (lands at 0x300000000) reached the Edit
window without this. So: REVERTED to DYN_ARENA_SIZE=4GB (low landing) and instead WIDENED the bump
split -- bump_next = lo + size/4 (was size/2) -> ~3GB for ocerz_map_anywhere (was 2GB), which still
clears the vprot-OOM (~2GB pressure) WITHOUT the high landing. (src/dyld.c:226 back to 4GB;
src/mem.c:423 bump_next=lo+size/4; the mmap-hint at mem.c:409 is kept, harmless.) make-check GREEN.
Testing whether the low arena AVOIDS the fetch-fault (if it reappears at LOW 0x3xx addresses it's a
deep control-flow bug, not the landing). 
RENDER-PATH BLOCKER MAP (goal = render a Wine window; current state with the fixes above):
  [DONE]  arena -> winemac.drv LOADS (no vprot OOM / "no driver").
  [DONE]  gs:0x320 worker crash -> Bug B bounded recovery (process survives).
  [OPEN]  fetch-fault: a thread diverges to an unmapped arena addr (+the nested-fault that makes it
          fatal). Likely root to attack next; a jump-to-unmapped is unrecoverable so the nested-
          fault fix alone won't save it -- must stop the divergence.
  [OPEN]  macdrv on-screen NSWindow STILL never traced (0 lines) -- processes crash before the
          desktop/window Cocoa path runs; winemac.drv loads, so it should run once a process
          survives to create a top-level window.
  [OPEN]  perf wall (#24): serial ~100s boot (per-process JIT/interp cold-start; ocerz_jit_step
          re-translates every block per process) trips Wine's 10s service deadline -> RpcSs fails.
          The real fix is a JIT translation cache SHARED across processes (big; blocked by arena
          base non-determinism across processes -> would need a fixed arena base). notes #26b.

========================================================================
UPDATE #27 2026-06-14 -- RENDER-PATH CONSOLIDATION (goal: render a Wine window). Several real
fixes landed this session; the boot now survives deeper but does NOT yet reach an on-screen
NSWindow. The gating blocker is the #24 perf wall. make-check GREEN (clean rebuild).
========================================================================
FIXES LANDED THIS SESSION (all make-check GREEN, all UNCOMMITTED):
 1. ARENA (src/dyld.c DYN_ARENA_SIZE stays 4GB; src/mem.c bump_next=lo+size/4 was lo+size/2;
    mmap hint of OCERZ_LOW_LIMIT at mem.c:409). The 2GB bump pool exhausted -> 1MB vprot-table
    mmap failed -> "no driver could be loaded" (NO RENDER). Widening to a 3GB bump pool (low
    landing) clears it: winemac.drv now LOADS. (8GB lands the arena ~48GB high -> a deterministic
    jump-to-unmapped crash; rejected. The low 4GB+3/4 split keeps the known-good 0x300000000
    base AND clears the OOM.) -> vprot-OOM 0, "no driver" 0, fetch-fault 0.
 2. gs:0x320 Bug B BOUNDED RECOVERY (src/vm.c): a no-Wine-TEB worker looping on the get_current_
    teb near-null fault -> terminate just that worker (process survives) instead of fatal.
 3. NESTED-FAULT signal-safe COMMIT (src/mem.c ocerz_commit_fault_page + src/vm.c crash_handler):
    a no-altstack thread builds its signal frame on its rsp stack; the top page is reserved-but-
    uncommitted -> ocerz's frame-build write faults INSIDE the host fault handler (can't take
    map_lock) -> "nested fault inside crash handler" _exit(139). Fix: commit that page lock-free
    (mprotect+bit_set, async-signal-safe) and re-run the write. Plus eager-commit the sigaltstack
    at registration (src/syscall.c sys_sigaltstack).
 4. gethostuuid (BSD 142) + mach_vm_purgable_control (trap 11) from earlier.
CURRENT RENDER STATE (low 4GB arena, all fixes): the boot runs deep (icount 100M+), winemac.drv
LOADS (no "no driver"), the ROOT process stays ALIVE ~190s, explorer starts -- BUT win=0 (notepad
never reaches CreateWindowEx) and macdrv=0 (the Cocoa NSWindow path never runs). So the boot does
NOT complete to notepad's window. ROOT = the #24 PERF WALL: serial ~100s boot (per-process JIT/
interp cold-start; ocerz_jit_step re-translates every block per process) trips Wine's 10s service
deadline (programs/services/services.c:44) -> services/RpcSs fail -> notepad never launches to
create its window. The arena/gs:0x320/nested-fault fixes removed the CRASHES, exposing that the
remaining wall is pure PERFORMANCE, not a crash.
THE PERF FIX (next, big): a JIT translation cache SHARED across processes -- ntdll/Wine code is
byte-identical in every process; translate once instead of 7x. NOW FEASIBLE: the mmap hint makes
the arena base DETERMINISTIC (0x300000000) across processes, so guest code addresses match and
JIT'd code (with embedded guest addrs) is shareable. Needs: a shared (memfd/MAP_SHARED) JIT code
buffer + block-cache index, position consistent across processes. This is THE path to a window.
KEPT diag (all gated): OCERZ_OOMLOG, OCERZ_SCCOUNT, OCERZ_WSIG; nested-fault now prints host_pc/
gaddr/comm. notes UPDATE #27.

========================================================================
UPDATE #28 2026-06-14 -- ★★★ THE RENDERING PIPELINE WORKS. A Wine GUI window RENDERED ON SCREEN
under ocerz (winemac.drv -> Cocoa NSWindow -> visible). The render goal is now reframed: it is
NOT a rendering/macdrv problem and NOT primarily a perf problem -- it is process STABILITY.
========================================================================
A fast run (WINEDEBUG=-all) was checked with CGWindowList (Quartz): it found 2 on-screen windows
owned by "wine", layer=0, real bounds (500x500 + a small one) -- these are `winedbg --auto` CRASH
DIALOGS (3 Wine processes crashed -> Wine auto-launched winedbg, whose dialog winemac.drv RENDERED
ON SCREEN). So: (a) winemac.drv successfully creates on-screen Cocoa NSWindows under ocerz (the
old #16/#17 Cocoa-main-loop wall is effectively cleared -- the Mach-bridge #23 + the arena/crash
fixes did it); (b) a Wine GUI process CAN reach window creation FAST under ocerz (winedbg launched
on a crash and immediately showed its dialog) -- so PERF is NOT the hard blocker for reaching a
window; (c) the reason NOTEPAD specifically doesn't render is that Wine processes CRASH (-> winedbg)
before/instead of reaching their own windows. The crashes are the #26/#28 dispatcher family
(garbage gs-restore: guest_rip=ntdll.so+0x33f23, [r14]=poison e.g. 0x1000007feedfde0) on REAL Wine
threads (have an altstack) -- NOT caught by Bug B's no-altstack worker recovery. So the PATH TO
RENDERING NOTEPAD is now: STABILIZE the #26/#28 real-thread dispatcher crash (e.g. #28 Fix C:
restore gs=wine_teb_base at the dispatcher edge, or recover the bad TEB lookup), so the boot
processes (and notepad) stop crashing into winedbg and reach their own windows -- which winemac.drv
will then render (proven). This is a MUCH more tractable wall than the perf/shared-JIT-cache work.
HOW TO CHECK A WINDOW: `python3 -c "import Quartz; wl=Quartz.CGWindowListCopyWindowInfo(Quartz.
kCGWindowListOptionAll,Quartz.kCGNullWindowID); ..."` filter owner~='wine'; a notepad window is
WIDE (>700px, ~1028x727) vs winedbg's ~500x500. notes UPDATE #28.

UPDATE #28b -- the boot is UNSTABLE TWO WAYS before notepad reaches its window, both deep:
 (A) CRASH: the #16/#17 foreign-worker dispatcher crash (~167M icount) -- a worker with an
     altstack but no TEB runs the WoW64 dispatcher -> kills a CHILD process (root survives) ->
     winedbg. Proper fix = #16/#17 marshaling.
 (B) HANG: in another run (render15, broadened Bug B, WINEDEBUG=-all) the boot did NOT crash
     (0 crashes, 0 winedbg) but HUNG at ~152M icount for the full 280s with no progress and no
     window -- the #24 wineserver-IPC / ulock sync + perf waits compounding to a stall.
So notepad never reaches CreateWindowEx because each boot run either crashes a needed process or
hangs, before notepad's own window-creation. REVERTED the Bug B "LOOPING-alone" broadening back
to `looping && sig_altstack_sp==0` (the broadened form is risky -- terminating a lock-holding
real thread HANGS instead of crashing -- and was unexercised; the conservative form is tested and
lets the root survive child crashes). make-check GREEN.
NET FOR THE RENDER GOAL: the rendering PIPELINE works (winemac.drv->NSWindow->on-screen, proven);
every crash blocker that ocerz can remove without deep architecture is removed; what's left are
TWO architectural, multi-session features -- (1) #16/#17 foreign-worker MARSHALING (don't run Wine
PE code on libdispatch workers) and (2) the #24 PERF/SYNC wall (shared JIT cache + wineserver-sync
stabilization so the boot completes fast and without hanging). notepad rendering is gated on those
two, both precisely localized + documented here. notes UPDATE #28b.

UPDATE #28c -- attacking the #16/#17 crash (A) directly with a TARGETED recovery (the crash is
the gate: when it kills a process into winedbg, that process never reaches its own window). The
crash is the WoW64 dispatcher reading a POISON TEB ptr from the 64KB stack base r14 (rax=poison
e.g. 0x1000007feedfacf) and dereferencing [rax + small TEB-field offset] -> a WILD fault OUTSIDE
guest space, so the in-guest-space recovery (vm.c:412) never sees it. Observed at MULTIPLE
dispatcher instructions/offsets: [rax+0x320] @ ntdll.so+0x33f23, [rax+0x2f0] @ +0x41d0d. The
crashing thread runs a 32-bit (WoW64) stack (~0x200000000), r14=0x200000000, [r14]=poison -- a
foreign worker (or a thread whose TEB setup at its stack base never ran). FIX (src/vm.c, NEW
out-of-guest-space recovery block after the in-guest-space one): if a fault is OUTSIDE guest
space AND it is a deref of a wild/poison rax (rax also outside guest space, fault == rax + small
offset <0x2000) AND the thread has no valid Wine TEB (no altstack, OR the ptr at r14 is not
committed), terminate just that thread (cpu->terminated=1; siglongjmp) so the process survives.
Targeted to the dispatcher-poison signature -> won't fire for a real Wine thread (valid TEB at
r14). make-check GREEN (test_syscall 96/0, sse 246/0, decode 190/0). Testing whether recovering
this crash lets a (notepad) process survive to CreateWindowEx -> winemac.drv render. CAVEAT: the
proper fix is still #16/#17 marshaling; this is a bounded recovery. And even with it, the #24
perf/sync wall (slow boot / ~152M hang in fast runs) may still gate notepad's window. notes #28c.

## UPDATE #28d (2026-06-23): ROBUST thread-state wild-worker recovery -> boot reaches macdrv for the FIRST TIME (major advance); #17 Cocoa-main-loop is now the wall
render18 proved the fault-ADDRESS signature is hopeless: the poison TEB (read from the 64KB
stack base by the WoW64 dispatcher's gs-restore) propagates into rax and is dereferenced at
UNPREDICTABLE addresses by different dispatcher insns -- observed [rax+0x320], [rax+0x2f0], and
[rax + scaled-index ~0x200000000] at 3 distinct guest_rips. No fault-offset match covers them.

FIX (src/vm.c, crash_handler, kept, make-check green: test_syscall 96/0, sse 246/0): match the
reliable THREAD STATE instead of the fault address. A real Wine thread ALWAYS stores a valid
(committed) TEB pointer at its 64KB stack base *(rsp&~0xffff); a foreign HOSTWQ worker has poison
there. So on ANY out-of-guest-space fault, read teb = *(rsp&~0xffff) (guarded by committed()) and
if teb is not a valid committed pointer, this is a no-TEB worker running the dispatcher ->
terminate just it (g_cur_cpu->terminated=1; siglongjmp), process survives. Real threads (valid
TEB at the stack base) are NEVER touched. This replaces the brittle wild_rax/wild_fa offset match.

RESULT (render19): the robust recovery caught ALL poison crashes (crashes=0, vs render18's
crashes=1 at 152M) and the boot progressed PAST the 167M crash point to ~171.5M -- and macdrv RAN
FOR THE FIRST TIME (winemac.drv loaded + macdrv_init executed). This is the deepest the Wine boot
has ever reached: past every gs:0x320 / poison-TEB crash, into the Cocoa display driver.

NEW WALL (the genuine remaining blocker for an on-screen window):
  0088:err:macdrv:macdrv_init Failed to start Cocoa app main loop
This is the #16/#17 wall: macdrv_start_cocoa_app posts run_cocoa_app to CFRunLoopGetMain() and
waits <=5s for COCOA_APP_RUNNING; it times out. Per UPDATE #16/#17 the Cocoa setup spins because
a libdispatch event-manager (dispatch_mgr) thread's kevent returns EVFILT_READ events on ~21 fds
that are never drained (the HOSTWQ bridge handles async blocks but not read/write event SOURCES),
so the source handlers never run on a worker and COCOA_APP_RUNNING is never signalled in 5s.

KEY TENSION discovered: the robust recovery TERMINATES the foreign workers -- but those same
libdispatch workers are what the Cocoa main loop needs to DRAIN its event sources. So terminating
them avoids the crash but can directly starve the Cocoa-main-loop bring-up. This sharpens the
true #16/#17 fix: the workers must successfully RUN their (Wine/Cocoa) work with a valid TEB, NOT
be terminated. The recovery is a genuine advance (first boot to macdrv) but remains a bounded
bandaid; the durable fix is foreign-worker TEB setup / event-source marshaling so the dispatch
source handlers run. winedbg's macdrv DOES pass this wall (its crash dialog renders) -- a simpler
process with fewer event sources -- so the wall is state/event-count dependent, not impossible.
NEXT: trace ocerz's HOSTWQ event-SOURCE dispatch (dispatch_mgr kevent -> _dispatch_source_invoke
-> worker source-handler) and find where the read/write-source handler dispatch is dropped.

## UPDATE #28e (2026-06-23): #17 Cocoa-main-loop wall isolated to the MAIN thread (run_cocoa_app), NOT the workqueue workers
After #28d got the boot to macdrv, a series of runs nailed down WHY macdrv_start_cocoa_app times
out (5s) -- it is NOT the worker/event-source path:

- render19 (HOSTWQ sync) and render20 (HOSTWQ_ASYNC, kernel-driven workers): IDENTICAL result --
  crashes=0, macdrv runs, "Failed to start Cocoa app main loop". So the worker-path choice (sync
  synthetic vs kernel-driven async) does NOT matter. Both reach macdrv and both time out.

- kevent_qos (374) was stubbed (returns 0). Forwarding it to the real kernel:
  * render21 (forward ALL): early CORRUPTION crash at icount ~0x2d3c4 during dylib init --
    host PC driven into guest memory (si_code=ADRALN on an 8-but-not-16-aligned addr), i.e. a
    WORKQ-flagged kevent_qos re-entrantly ran a guest worker with a half-built context. Reverted.
  * render22 (forward only NON-WORKQ, flags & KEVENT_FLAG_WORKQ(0x20)==0): clean (crashes=0),
    make-check green, reaches macdrv -- but STILL "Failed to start Cocoa". So kevent_qos was not
    the blocker. KEPT this anyway: forwarding a real kqueue syscall is more faithful than the
    0-return stub, mirrors the existing kevent_id forward, and does not regress.

- render23 (GSTRACE) + render24 (WSIG) measured the workers DURING the 5s macdrv window:
  * Workers DO run (HOSTWQ_ASYNC kernel-driven path works) but only ~2/s -- NOT a 100% spin.
  * Only 2 worker regions recycle; events are 20x EVFILT_WORKLOOP(-17) + 10x EVFILT_MACHPORT(-8).
  * The Mach-message bridge SUCCEEDS (gbuf valid, COMPLEX OOL relocated) -- not the #23 fault.
  => The workqueue side is healthy. The workloop/machport re-fires are normal background, ~2/s.

CONCLUSION: the 100%-CPU spinner during the macdrv wait is run_cocoa_app on the MAIN thread (the
GUI proc pegs 100%, per #16), independent of the workers. #16 first located a libobjc spin on the
uncommitted selpool slot 0xdda002a0; #17 FIXED that (selopt perfect hash). So this is the NEXT
main-thread spin past the selpool fix. render25 pulses SIGUSR1 (OCERZ_RIPDUMP) at the high-CPU
ocerz proc during the macdrv window to name the current spinning guest rip -> the targeted fix.

## UPDATE #28f (2026-06-23): found+fixed a REAL fatal (__pthread_canceled, syscall 333) killing the Cocoa worker -- but it is not the sole window blocker
RIPDUMP (OCERZ_RIPDUMP=SIGUSR1) of the macdrv-phase spinner caught a libdispatch worker (gs=pth+0xe0)
doing read(fd=4,buf,64) (rax=0x2000003, draining an EVFILT_READ source) then hitting:
  ocerz: fatal: unknown BSD syscall: class=2 num=333 (no table entry) rip=0x7ff802e397e2
Syscall 333 = __pthread_canceled(action) -- the pthread cancellation-point EPILOGUE libpthread runs
after a cancelable syscall (the read). It had no bsd_table entry, so ocerz fatally aborted the worker
mid-drain. Real bug, real fix.

FIX (src/syscall.c bsd_table):
- [333] __pthread_canceled -> FORWARD (NULL handler). It only READS cancel state; with no guest thread
  ever marked, the kernel returns "not canceled" and the worker continues. Stubbing to 0 would falsely
  mean "canceled" -> the worker pthread_exit()s. Forwarding is the faithful + correct choice.
- [332] __pthread_markcancel -> STUB (sys_workq_stub). Forwarding it (render26) cancels the real HOST
  thread backing the guest thread, which then exits mid-ocerz_vm_run_cpu and corrupts the host stack
  (deterministic SIGBUS/ADRALN at icount ~0x2d3c4 during ICU dylib init, host PC driven into guest
  memory -- the SAME signature render21 hit). ocerz never pthread_cancel's guest threads, so leaving
  them unmarked is the correct observable state. Proper guest-level cancellation is a TODO.

RESULT (render27): build clean (test_syscall flaky mmap aside), NO more syscall fatal, NO early crash,
boot reaches macdrv -- but macdrv STILL "Failed to start Cocoa app main loop". So the 333 fatal was a
genuine bug (workers no longer die on a cancellation point) but NOT the sole cause of the Cocoa-main-
loop timeout. The worker now SURVIVES the read+canceled; the question is what it (or run_cocoa_app on
the main thread) does next that still misses the 5s deadline. render28 = denser RIPDUMP (broadcast
SIGUSR1 to all ocerz procs across the macdrv window) to locate the post-fix spin/block. Kept fixes:
robust recovery (#28d), non-WORKQ kevent_qos forward (#28e), 332/333 (#28f).

## UPDATE #28g (2026-06-23): the macdrv-phase 100%-CPU blocker is a foreign worker flooding REQ_enum_key_value (registry enumeration)
After the #28f cancellation fixes, macdrv still times out. RIPDUMP/RDLOG/WRLOG pinned the spinner:
- One libdispatch WORKER (gs=pth+0xe0, NO Wine TEB) pegs 100% CPU running Wine PE code at 0x341a2fe70
  in a write-request/read-reply loop on the wineserver fd (libsystem read@0x7ff802e305d2 /
  write@0x7ff802e32982; Wine's own +server trace is SUPPRESSED on it because TRACE_ON reads debug
  state from the missing TEB -- itself proof the worker has no Wine thread context).
- WRLOG decoded the request opcode: REQ=93 = REQ_enum_key_value (sampled 1-in-200/500, so THOUSANDS
  of actual calls). Reply data (RDLOG) holds UTF-16 "Default"/"WinSta0" + a Windows SID.
- So a foreign worker enumerates a registry key's values thousands of times, flooding the wineserver;
  since the server processes requests serially, run_cocoa_app's own Cocoa-setup server calls queue
  behind the flood -> COCOA_APP_RUNNING misses the 5s deadline -> "Failed to start Cocoa app main loop".

This is the #16/#17 foreign-worker problem in LIVELOCK form (not the crash form the robust recovery
already handles): a libdispatch worker runs Wine PE registry code with no Wine TEB. In real Wine this
enumeration runs on a real Wine thread, fast/cached. NEXT (render33): log the enum index to tell a
finite-but-slow pass (likely the Fonts-key build; fix = perf/cache/timeout) from an infinite loop
(fix = the enum_key_value termination bug the no-TEB worker induces). Either way the durable fix is
the #16/#17 marshaling/TEB work so Wine PE code does not run on bare libdispatch workers. Diagnostics
added (env-gated OCERZ_RDLOG): read fd 3/4/5 returns+content, write request opcode/hkey/index.

## UPDATE #28h (2026-06-23): final diagnosis of the macdrv wall = PERF (finite enum, not a loop); tree clean+green
render33 settled loop-vs-finite: within each key the enum index increases MONOTONICALLY (hkey=0x48:
96->292->492; hkey=0x3c: 33->233->429), so each of ~4-5 keys (~500-700 values) is enumerated ONCE per
process across 4 processes. NOT an infinite loop -> a FINITE registry enumeration (Wine's font/registry
init) that is simply too slow under ocerz to finish within macdrv_start_cocoa_app's hard 5s deadline
(confirmed in Wine source dlls/winemac.drv/cocoa_main.m:136 `dateWithTimeIntervalSinceNow:5`). The
process is pinned to ONE core during the wait; the syscall + committed-memory hot paths are lock-free
(map_lock only guards commit/protect/bump), so the main thread is idle/blocked (not lock-serialized)
while the foreign worker monopolizes wineserver bandwidth. So this is the project's #24 PERF wall
surfacing at the macdrv gate, compounded by the #16/#17 foreign-worker issue (the enum runs on a bare
libdispatch worker with no Wine TEB; in real Wine it runs fast on a real thread).

FIX DIRECTIONS (next session): (a) PERF -- cut per-server-call/IPC-round-trip overhead so thousands of
enum_key_value calls finish in <5s (shared JIT cache across processes so the wineserver+client font
loops are not cold-JIT'd per process; faster client<->wineserver context switches). (b) #16/#17 -- give
foreign libdispatch workers a real Wine TEB or marshal Wine PE work off them so the enum runs on a Wine
thread in parallel with run_cocoa_app. Either unblocks the macdrv 5s gate -> the Cocoa app starts ->
notepad's window renders. SESSION STATE: 3 faithful fixes landed (robust thread-state recovery #28d,
kevent_qos non-WORKQ forward #28e, __pthread_canceled fwd + markcancel stub #28f), all make-check green
(test_syscall 96/0, sse 246/0, decode 190/0); exploratory RDLOG/WRLOG diagnostics removed; tree clean.
The Wine boot reaching macdrv at all (zero ocerz crashes) is this session's milestone -- the deepest yet.

## UPDATE #28i (2026-06-23): PROOF the macdrv wall is per-process PERF, not contention — notepad fails ALONE
render40 (wineboot -> wait-for-quiesce -> notepad): phase 1 booted the prefix with wineboot and it
QUIESCED at 60s (total ocerz CPU = 3%, 6 idle procs). Phase 2 then launched notepad alone against the
quiescent, warm prefix. notepad STILL hit "Failed to start Cocoa app main loop" (macdrv=1,
cocoa_fail=1), running at exactly ONE core (~100%) on its own font enumeration. So the 5s macdrv
timeout is NOT a boot-contention artifact -- it is notepad's OWN startup cost. winedbg (a simpler
dialog, far less font work) renders fine through the same macdrv path; notepad does not, because
notepad's edit-control font init drives the ~thousands-of-REQ_enum_key_value font/registry pass.

This pins the blocker conclusively to the #24 per-process PERFORMANCE wall: the font pass is finite
and JIT'd, but at ocerz's throughput it lands ~5-6s -- just over Wine's hard 5s deadline. It is
BORDERLINE: a ~20-30% per-process speedup would tip it under 5s and the window would render. The
durable faithful fix is a SHARED/persistent JIT cache so each new Wine process does not cold-translate
the WoW64 dispatcher + ntdll + the enum path from scratch (today every process re-JITs everything --
notes #24). winedbg only passes because it is launched later/simpler, not because the path is fast.
No faithful quick fix exists that does not either (a) do the #24 shared-JIT-cache work, or (b) cross
into a workaround the owner rejects (shrink the prefix font set / extend Wine's 5s timeout / scale
guest time). Tree remains clean + green (3 faithful fixes from this session); boot-to-macdrv stands.

## UPDATE #28j (2026-06-23): 64-bit app ALSO fails -> blocker is per-syscall guest-code JIT throughput, not WoW64/arch/warmth
render41 ran the 64-bit notepad (drive_c/windows/system32/notepad.exe, PE32+ x86-64) to remove the
WoW64 32->64 dispatcher cost from every font-enum syscall. It STILL hit "Failed to start Cocoa app
main loop" (macdrv=1, cocoa_fail=1), and wineboot also logged "run_wineboot boot event wait timed
out" (its own 30s deadline). So the WoW64 dispatcher was NOT the tipping cost; the dominant per-
REQ_enum_key_value cost is the Nt* implementation + server-protocol guest code, identical for 32- and
64-bit. Combined with render40 (notepad fails ALONE on a quiesced prefix) and the warm-prefix tests
(warming the wineserver's font handler doesn't help -- the client's own JIT'd guest code per enum is
the bottleneck, not the server side or the IPC), this triangulates the blocker conclusively to ocerz
JIT-CODEGEN THROUGHPUT on the WoW64/Nt syscall path. It cascades through MULTIPLE Wine timeouts
(wineboot 30s + macdrv 5s), so the prefix never cleanly boots and every GUI app's macdrv times out.
winedbg only renders because it launches post-crash in a narrower/warmer window, not because the path
is fast.

Levers tried and ruled out this session (all faithful, none a workaround): robust recovery (boot ->
macdrv), kevent_qos forward, __pthread_canceled fix, getenv-hotpath already memoized, 64-bit arch,
warm/quiesced prefix, simpler-app reasoning. The SIGSYS-frame and WoW64-dispatcher per-syscall costs
were measured/argued marginal (~tens-hundreds of ms), not the seconds. The remaining faithful fix is
genuine JIT-throughput optimization (better arm64 codegen for the hot x86 patterns on the Nt path,
and/or a persistent cross-process translation cache), which needs a guest block-level profiler to
target and is too large/risky to land safely without endangering the boot-to-macdrv milestone. The
window is gated behind that work. Tree remains clean + green; milestone (boot to macdrv, zero ocerz
crashes) preserved.

## UPDATE #28k (2026-06-23): lldb confirms the main thread is BUSY (not idle) -> PERF, Mach-wakeup bug ruled out; map_lock contention is the next concrete lever
render42 attached lldb the instant macdrv logged "Failed to start Cocoa app main loop" and caught
thread #1 (com.apple.main-thread) STOPPED in ocerz`ocerz_ld (mem.h:188) on EXC_BAD_ACCESS at guest
addr 0x8195f90000 -- i.e. the main thread was EXECUTING guest code and taking a memory fault, NOT
parked in mach_msg. This DEFINITIVELY rules out the tractable "CFRunLoop source wakeup never
delivered" hypothesis: run_cocoa_app's AppKit init is genuinely running, just too slowly. So the
blocker is PERF (confirmed from 4 independent angles now: render40 notepad-alone, render41 64-bit,
the font-enum opcode trace, and this lldb).

Mechanism detail: the macdrv-phase faults are Windows LAZY-COMMIT/guard-page faults -- the guest
touches PROT_NONE reserved pages, ocerz delivers a guest SIGSEGV (depth==0 path, vm.c:412), Wine's
page-fault handler VirtualAllocs the page -> ocerz_protect -> commit_range -> the single global
map_lock (mem.c:97). Both the main thread and the font-enum worker fault frequently, so they likely
SERIALIZE on map_lock -- which fits the observed "process pinned to ONE core" during the wait. Each
fault also pays a full guest signal-frame build (ocerz_signal_deliver) + the JIT'd Wine handler + a
wineserver round-trip. All of it is the #24 per-operation overhead, cascading through Wine's 5s
(macdrv) and 30s (wineboot) timeouts so no GUI process ever signals COCOA_APP_RUNNING in time.

CONCRETE NEXT-STEP LEVERS (faithful, but each needs measurement + careful testing, NOT to be rushed
at session end against the boot-to-macdrv milestone):
  1. Measure map_lock contention during the macdrv phase (trylock-fail counter). If hot, move to
     per-region/finer-grained commit locking so the main thread + workers commit in PARALLEL
     (would directly address the one-core serialization).
  2. Cheaper guest page-fault path: the per-fault ocerz_signal_deliver builds a full AVX mcontext;
     a lighter frame for the commit/guard-page case (validated against what Wine's handler reads).
  3. Shared/persistent cross-process JIT cache (#24 core) so the WoW64 dispatcher + ntdll + AppKit
     translation is not redone per process.
Any one of these that lands a ~20-40% per-process speedup would tip the borderline font/AppKit pass
under 5s and the window would render. Tree clean + green; milestone (boot to macdrv, 0 ocerz crashes,
3 faithful fixes) preserved. This session: 42 render runs + lldb, full multi-angle proof of the wall.

## UPDATE #28l (2026-06-23): ROOT CAUSE of the perf wall pinned -- JIT is ~100% (not interp), but every low-shadow/top memory access is a per-access SLOW CALL. THE high-impact next fix.
OCERZ_JITPROF (added to ocerz_jit_step, then removed) measured the boot: ratio_interp = 0% the whole
run (xlat_fail=0, dyld-region interp=3085 total), i.e. the code is ~100% JIT-COMPILED. The macdrv proc
ran ~580M JIT blocks in ~150s. So the earlier lldb catch of the main thread "in ocerz_ld" was NOT the
interpreter -- it was the JIT'd code CALLING the ocerz_ld helper for a guest memory access.

WHY that matters (the actual bottleneck): the JIT (src/jit.c emit_mov_mem / emit_arith_mem via
emit_commpage_guard) inlines a guest memory access ONLY for the flat main arena (host = gaddr +
ocerz_guest_base, ~3 arm64 insns). For the COMMPAGE, the LOW-SHADOW window (g2h = gaddr + low_base,
all addresses < OCERZ_LOW_LIMIT 0x300000000), and the TOP region (g2h = gaddr - TOP_LO + top_base),
it emits a SLOW CALL instead (emit_slowcall -> the full ocerz_ld/ocerz_st path + call overhead,
~20-50 insns). For a 32-bit WoW64 process (plain `wine notepad`), ALL 32-bit code/data lives below
0x100000000 -> ALL in the low-shadow -> EVERY memory access is a slow call. Memory ops are ~30-40% of
x86 instructions, so this throttles WoW64 GUI startup ~2-3x -- enough to blow the macdrv 5s and
wineboot 30s deadlines. (The 64-bit notepad helped only partially because its PE image at 0x140000000
is also < LOW_LIMIT -> also low-shadow/slow.)

THE FIX (high-impact, faithful, the clear next step -- deferred only to avoid rushing a core JIT
change at session end against the boot-to-macdrv milestone): inline the low-shadow and top regions in
the JIT memory emitters using their CONSTANT offsets (ocerz_low_base / ocerz_top_base), keeping only
the genuine commpage on the slow call. i.e. replace emit_commpage_guard's "low/top -> slowcall" with
an inline host-address computation (a couple of compare+branch+add per access selecting +low_base /
-TOP_LO+top_base / +guest_base). This removes a function call from the hot path of EVERY WoW64 memory
access -> ~2-3x WoW64 throughput -> the font/AppKit cold-start should finish under Wine's 5s and the
window renders. Validate with make-check (test_decode/test_interp/test_syscall exercise memory ops)
then a boot run. This is the single most promising lever found this session.

Session tally: 43 render runs + lldb + JITPROF; proven the wall from interp-ratio, thread-state,
alone/64-bit/warm-prefix angles; 3 faithful fixes landed (boot->macdrv, kevent_qos, pthread_canceled);
tree clean+green; milestone preserved. notes #28d-#28l.

## UPDATE #28m (2026-06-23): LANDED the low-shadow JIT inline (real faithful WoW64 speedup, make-check green, boot->macdrv, 0 crashes). macdrv decider is now isolated to the 64-bit AppKit cold-start.
Implemented the #28l fix in src/jit.c emit_commpage_guard: the low-shadow region [0, LOW_LIMIT) is no
longer a per-access slow call -- it shifts addr_reg by the constant (ocerz_low_base - ocerz_guest_base)
so the caller's existing "+guest_base" native body lands at gaddr + low_base = ocerz_g2h(gaddr). TOP
region + commpage stay on the slow call (rare). This removes a function call from EVERY 32-bit WoW64
memory access (all guest data < 0x100000000 < LOW_LIMIT). VALIDATED: make-check green (test_syscall
96/0, sse 246/0, decode 190, interp, corpus), boot runs end-to-end to macdrv with ZERO ocerz crashes
(render44) -- i.e. the inline is correct (no memory corruption), and it is a genuine faithful perf gain
for all WoW64 code. KEPT.

BUT it did NOT render the window. render45 (notepad ALONE on a quiesced prefix, phase2 wall=18s) still
cocoa-fails. So the low-shadow inline is NOT the macdrv decider: the decider is run_cocoa_app's
[WineApplication sharedApplication] / NSApplication-AppKit cold-start, which is 64-bit winemac.so +
AppKit code in the MAIN ARENA (already inlined). Its sheer instruction count at ocerz's JIT throughput
exceeds Wine's 5s deadline -- a raw 64-bit-throughput problem the WoW64 memory fix cannot touch. lldb
(render42) had already caught that main thread BUSY executing (ocerz_ld helper), confirming it is
compute-bound, not blocked. So the wall has moved one layer in: from "WoW64 memory-access overhead"
(now fixed) to "64-bit AppKit cold-start throughput".

NEXT LEVERS for the AppKit cold-start (each deeper, faithful): (a) better arm64 codegen density on the
hot 64-bit blocks (fewer arm64 insns per x86 insn); (b) reduce guest page-fault (lazy-commit) signal
overhead AppKit init incurs (cheaper commit path / batch commit); (c) the persistent cross-process JIT
cache so AppKit isn't re-translated per process. Session result: 45 render runs + lldb + JITPROF;
4 faithful fixes LANDED (boot->macdrv robust recovery, kevent_qos, pthread_canceled, low-shadow JIT
inline); root cause fully traced; tree clean+green; milestone preserved. notes #28d-#28m.

## UPDATE #28n (2026-06-23): CPU measurement confirms the AppKit wall is SINGLE-THREADED compute-bound (no tractable parallelism fix)
render46 sampled the busiest ocerz process's %CPU every 1s through notepad's (quiesced, alone) startup
+ macdrv wait: it held ~90-100% = ONE CORE the whole time (top PID rotates as wineboot/services/
explorer/notepad take turns; each is single-threaded-busy, with occasional dips to ~65-78% = brief
IPC/fault waits). So the macdrv-deciding work is NOT multi-thread lock contention (ruling out the
map_lock/parallelism lever) -- it is one thread, compute-bound. Combined with lldb (main thread busy
executing) this is conclusive: run_cocoa_app's 64-bit AppKit cold-start is ~6-13s of single-threaded
JIT'd execution + per-block translation, exceeding Wine's 5s. The ONLY remaining faithful levers are
raw-throughput (deep): denser arm64 codegen on hot 64-bit blocks, faster/cached block translation
(persistent cross-process JIT cache so AppKit isn't re-translated each process), cheaper lazy-commit
fault path. winedbg renders only because it launches later in a warmer/lighter state, not because the
path is fast. Session totals: 46 render runs + lldb + JITPROF + CPU profiling; root cause fully traced
to single-threaded 64-bit AppKit throughput; 4 faithful fixes LANDED; tree clean+green; milestone kept.

## UPDATE #28o (2026-06-23): main-arena JIT fast-path LANDED (2nd real speedup); + HONEST CORRECTION: the "rendered window" was a FALSE POSITIVE (Steam + stale-window contamination)
LANDED (src/jit.c emit_commpage_guard): a single fast-path range check -- if gaddr in [LOW_LIMIT,
TOP_LO) (the main arena, where all 64-bit winemac.so+AppKit code lives) branch straight to the native
+guest_base access, skipping the ~10-instruction commpage/top/low guard. make-check green (syscall
96/0, sse 246/0, decode, interp, corpus), boot->macdrv with ZERO crashes. Correct + a real 64-bit
throughput gain. KEPT (with the #28m low-shadow inline -- two genuine JIT memory speedups this session).

HONEST CORRECTION: I briefly believed notepad/a Wine app had rendered ("APP-WINDOW" at ~152s in
render47/48). It was a FALSE POSITIVE. Two contaminants fooled my CGWindowList check: (1) the user's
Steam keeps respawning (steam.exe / steamwebhelper.exe own 500x500 windows) -- my winedbg filter used
`ps comm` which shows "ocerz" for everything under ocerz, so it never excluded these; (2) stale
NSWindows from earlier runs lingered after pkill (the proc dies but the WindowServer keeps the window
briefly), so a fresh run detected them at ~2s. The AIRTIGHT run (render53: killed all my window-owning
procs, slept 10s, VERIFIED 0 non-Steam wine windows before launch, then only accepted a window after
60s of fresh boot) found NO Wine window on screen -- only the internal #32769 desktop CLASS created
(Wine always makes it; not a rendered NSWindow), and cocoa_fail=1. So ocerz does NOT yet render a Wine
window; the macdrv 5s wall still wins. (Screenshots are blocked by macOS Screen-Recording permission
-- "could not create image from display" -- so I cannot visually inspect either.)

TRUE STATE after this session (53 render runs): 5 faithful fixes LANDED (boot->macdrv robust recovery,
kevent_qos forward, pthread_canceled, low-shadow JIT inline, main-arena JIT fast-path), root cause of
the wall fully traced (single-threaded 64-bit AppKit cold-start, ~100% JIT, compute-bound, >5s), tree
clean+green, milestone (boot->macdrv, 0 ocerz crashes) preserved. The window is STILL gated on ~2x more
64-bit throughput (the two JIT speedups landed ~20-30%, not the 2x needed). Next: denser hot-block
codegen, persistent cross-process JIT cache, cheaper lazy-commit fault path. NO rendered window yet.

## UPDATE #28p (2026-06-23): persistent JIT cache — VERIFY-FIRST measurement = NO-GO (translation is 61–83ms, the cold-start is execution-bound). V-A proven, V-B KILLs the cache as a window fix.

Owner picked "persistent cross-process JIT cache" as the next direction. Before writing any cache code I
ran the mandated VERIFY-FIRST gate (V-A zero-reloc invariant, V-B break-even). Added one log-only,
env-gated instrument: `OCERZ_JITMEASURE` in `translate()` (src/jit.c) times every successful
translation with `clock_gettime_nsec_np(CLOCK_UPTIME_RAW)` under jit_lock (so plain statics are
race-free), splitting out shared-cache code (`rip >= 0x7ff800000000`). make-check unchanged
(test_syscall 96/0, test_decode 190) — the measure path is a single cached `getenv` branch when off.

V-A (zero-relocation invariant across processes) — PROVEN BY INSPECTION, no run needed:
  - cache.c:218 `map_subcache` mmaps every shared-cache mapping MAP_FIXED at its file-preferred address
    (`rd64(m)` = 0x7ff800000000 for the x86_64 cache), slide 0, or OCERZ_FATAL. So the cache base and
    every baked guest rip >= 0x7ff8… in a shared-cache block is BIT-IDENTICAL in every process.
  - mem.c:525 `setenv("OCERZ_LOWBASE", …)` + syscall.c:629-635 propagate the low-shadow base across the
    guest exec env; mem.c:499-510 reserves the inherited base MAP_FIXED. So within ONE boot, all
    processes share identical guest_base(=0)/low_base/top_base — every baked window constant matches too.
  - Net relocation surface for a within-boot cross-process block = exactly TWO per-process bakes:
    R1 `&ocerz_jit_exec_one` (ASLR helper) and R2 `&insns[i]` (heap insn ptr) — both PIC-able. Cross-RUN
    (disk) additionally needs low_base in the env key (advisory mmap differs per boot). So V-A is real:
    the cache is *technically* near-trivial to relocate.

V-B (break-even — does eliminating translation actually move the 5s Cocoa deadline?) — **KILL**.
  Ran notepad to the macdrv phase under OCERZ_JITMEASURE. The GUI process (the one whose macdrv
  `macdrv_start_cocoa_app` misses the 5s deadline) translated its ENTIRE working set:
      blocks=65536  xlat_total=83ms   |  shared-cache: blocks=49378  xlat=61ms
  Every boot process is in the same ballpark (16k blocks ≈ 18ms; ~1.1µs/block; ~900k blocks/s). So the
  COMPLETE translation cost of the deadline-missing process is ~83ms (61ms is the cache's exact target).
  The deadline is 5000ms and is missed by seconds. A cache that eliminates ALL translation saves ≤83ms.
  **It cannot move a multi-second-missed 5s deadline.** The cold-start is execution-bound, not
  translation-bound — consistent with #28l (≈100% JIT execution, not interp; the cost is *running* the
  translated AppKit cold-start code, not *translating* it).

DECISION: NO-GO on the persistent JIT cache as a fix for the window. (It would shave ~50-80ms × boot
redundancy off total boot latency — a real but tiny general win, not worth the relocation/keying
machinery now, and irrelevant to the 5s wall.) The OCERZ_JITMEASURE instrument is kept (gated off) as a
cheap standing measurement. The faithful lever remains the execution-bound Cocoa cold-start: either a
remaining libdispatch event-source spin (#17) or broad per-instruction execution cost (native coverage).
Profiling the CPU-pegged GUI proc during the 5s wait (host `sample`) is the next diagnostic to pick
between them. Also noted: a flaky-deterministic guest crash at guest_rip=0x7ff802e35bbf / icount=0x2d7e1
(SIGBUS, host_pc inside the JIT cache, host_insn=0x35000020 cbnz; host_addr varies only by JIT-cache
ASLR) sometimes aborts the boot before macdrv — recoverable (attempt 1 reached macdrv at ~34s), but a
reliability item to pin down since reaching macdrv reliably is needed to work the window.

## UPDATE #28q (2026-06-24): ROOT-CAUSED + FIXED the real window blocker — ocerz never translated in-message guest pointers on Mach SEND, so libxpc os_crashed "Malformed Mach message" during framework init. Flat-OOL + vector send translation landed; the framework-init UD2 crash is GONE (0/8 vs 8/8). make-check green.

THE REAL BLOCKER (supersedes the "macdrv 5s deadline" framing of #28h–#28p): with extended Cocoa budget
(#28p) the window STILL didn't render because the GUI process was ABORTING during framework init,
BEFORE macdrv ever ran. The abort is a guest UD2 (0f 0b) at libxpc+0x3a3ea =
os_crash("Data corruption: Malformed Mach message or kernel bug."), arg edi=0x10000002 =
MACH_SEND_INVALID_DATA. It was FLAKY only because it is allocation-placement dependent, NOT a race.

ROOT CAUSE: ocerz's mach_msg SEND paths (case 31 mach_msg_trap, case 47 mach_msg2_trap, src/syscall.c)
translated ONLY the message-buffer pointer a[0]=ocerz_g2h(a[0]) and forwarded the rest verbatim to the
real kernel. They NEVER translated the GUEST pointers EMBEDDED in the message from guest->host. Under
Wine the low-shadow window is active (ocerz_low_base!=0), so any guest addr < OCERZ_LOW_LIMIT
(0x300000000) is NON-identity: host = gaddr + ocerz_low_base. libxpc's outgoing message / OOL / vector
segment buffers frequently land sub-4GB in low-shadow. The kernel's copyin reads those embedded
addresses as host pointers, hits the wrong/unmapped host page, and rejects the send. When the same
buffer happened to land in the identity arena (host==guest) the send coincidentally worked -> the
flakiness. ocerz already did the EXACT INVERSE (host->guest) for INBOUND messages in
ocerz_bridge_mach_msg; the send-side guest->host walk was the missing symmetric half.

TWO embedded-pointer families (both captured via OCERZ_MACHMSG send-error logging + register decode):
  * FLAT complex message (opt-hi MACH64_SEND_MQ_CALL, no vector): an OOL/OOL_PORTS/OOL_VOLATILE
    descriptor's .address sat in low-shadow (e.g. d0 addr=0x10010ea10, type=1, committed). kr=0x1000000c
    = MACH_SEND_INVALID_MEMORY.
  * MACH64_MSG_VECTOR send (opt bit 32 set; a[0] is an array of `a[2]>>32` 24-byte mach_msg_vector_t
    {msgv_data@0, msgv_rcv_addr@8, msgv_send_size@0x10, msgv_rcv_size@0x14}): a segment's msgv_data sat
    in low-shadow (e.g. V1 data=0x10010ae20). kr=0x10000002 = MACH_SEND_INVALID_DATA = THE FATAL one
    libxpc crashed on. (MACH64_MSG_VECTOR/mach_msg_vector_t are XNU-private, not in public SDK headers;
    recovered the layout by decoding the register-packed header a[2..5] + the buffer.)

THE FIX (faithful — mirrors exactly what an in-process macOS send presents to the kernel; src/syscall.c):
  - ocerz_send_xlate_descriptors(): for a flat COMPLEX message, rewrite each OOL/OOL_PORTS/OOL_VOLATILE
    descriptor's 8-byte .address guest->host (ocerz_g2h). PORT (type 0) / GUARDED_PORT (type 4) untouched
    (shared port namespace). Strides mirror the inbound bridge (port +12, others +16; addr@+0; type@+11).
  - ocerz_send_xlate_vector(): for a MACH64_MSG_VECTOR send, translate each vector entry's msgv_data +
    msgv_rcv_addr guest->host, plus walk the control segment's OOL descriptors (guarded on the segment
    being committed so a non-pointer never faults the walk).
  - IN-PLACE-then-RESTORE: translate before the trap, restore originals after, but only restore a field
    still holding OUR host value (a mach_msg2 send+receive overwrites the buffer with the reply, which
    must be left intact). Identity-arena addresses (host==guest) are no-ops. Wired into case 31 + case 47.
  - Both gated on ocerz_low_base!=0; saved-descriptor arrays capped at 64.

RESULT (notepad, 8 fresh boots each): BEFORE fix = 8/8 UD2-crash at framework init; flat-only fix =
family-1 (0x1000000c) eliminated but 8/8 still crash on the vector 0x10000002; flat+vector fix = 0/8
UD2-crashes, no 0x10000002 / no 0x1000000c, only benign kr=0x10000003 (MACH_SEND_INVALID_DEST, dead
port). make-check fully green (syscall 96/0, decode 190, sse 246/0, interp, ext 165/0, loader 54/0,
cache 14). The framework-init crash that aborted the GUI process — the true reason no window rendered —
is DESTROYED. Next: re-test the full window path (the earlier #28p "extended budget -> no window" was
contaminated by THIS crash). Diagnostics kept gated: OCERZ_MACHMSG (BRIDGE-MSG / MACH-SEND-ERR / VEC).

## UPDATE #28r (2026-06-24): the Mach send fix advanced the boot PAST the framework-init crash to a NEW wall — LaunchServices _LSApplicationCheckIn deadlocks because the real coreservicesd never replies (daemon-policy gap, CONFIRMED). The .app bundle did NOT fix it. Past macdrv now lies a CHAIN of macOS daemon-compat gaps.

With the send-side Mach translation (#28q) the boot no longer crashes and progresses into AppKit/
NSApplication init, where a GUI process DEADLOCKS in a synchronous mach_msg2 send+receive that never
returns. Diagnosed in full:
  * Identity: the blocking sequence is msgh_id 10019 / 10050 / 10054 (LaunchServices MIG subsystem base
    10000 = the _LSApplicationCheckIn / app-registration family — confirmed vs shared-cache exports
    __LSApplicationCheckIn / __LSASNReturnASNForPIDFromWithinCoreServicesd), sent COMPLEX SEND|RCV with
    a MAKE_SEND_ONCE reply port and NO MACH_RCV_TIMEOUT, to the real coreservicesd port (obtained from
    the immediately-preceding com.apple.CoreServices.coreservicesd bootstrap lookup). Captured via
    OCERZ_MSGDUMP (inline service names) + OCERZ_MACHMSG.
  * ocerz forwards it FAITHFULLY: the send SUCCEEDS (no MACH_SEND_* error), the message is NOT touched by
    the send translation (xlated=0 — no low-shadow OOL), and the RCV is a normal synchronous in-trap
    receive. So it is NOT an ocerz translation/forwarding bug.
  * Root cause = DAEMON-POLICY GAP: every Mach trap is issued on the real host thread (svc #0x80), so the
    kernel stamps the message audit-token with the genuine ocerz/Wine host pid+identity. coreservicesd
    correlates that against an LS-registered app/ASN; the Wine guest was never LS-spawned and has no
    resolvable bundle/ASN, so _LSApplicationCheckIn cannot be satisfied and coreservicesd returns (or
    stalls on its own sub-RPC to launchd/secinitd) WITHOUT replying -> AppKit deadlocks before macdrv.
  * PROOF (OCERZ_LSTIMEOUT diag): forcing a 1.5s MACH_RCV_TIMEOUT on the 10000-range check-in sends makes
    them return MACH_RCV_TIMED_OUT with NO reply and NO crash -> confirms "send delivered, daemon chose
    not to reply" (policy gap), and rules out the alternative (a reply that arrives then faults on
    un-relocated inbound OOL — that latent case-47 reply-OOL gap remains real but is NOT this wall).
  * FAITHFUL fixes tried that did NOT work: (a) running ocerz from a real .app bundle (Contents/MacOS/ocerz
    + Info.plist org.ocerz.wine) AND `lsregister -f`-ing it — coreservicesd still does not reply (so the
    process-path/.app identity alone is not what real Wine-under-Rosetta relies on). (b) Forcing the RCV
    timeout lets AppKit proceed PAST the check-in, but it then blocks ELSEWHERE (a non-mach_msg2 wait) —
    i.e. the LS check-in is the FIRST of a CHAIN of daemon-compat gaps, and it also needs a real ASN
    (a bare timeout leaves AppKit with no ASN -> later LS derefs).

STRATEGIC REALITY: past the (now-fixed) crash, reaching a rendered window requires clearing a chain of
macOS userspace-daemon round-trips that don't complete for a Wine-guest-under-ocerz (LaunchServices is
the first). Each needs either FAITHFUL daemon-bridging (hard; coreservicesd-internal) or a compatibility
SHIM that synthesizes the reply the daemon would send for a registered app (conflicts with the owner's
"no hacks / faithful only" rule — an explicit owner decision). Diagnostics kept gated: OCERZ_MSGDUMP,
OCERZ_LSTIMEOUT, OCERZ_NO_SENDXLATE (A/B the send translation), plus the #28q OCERZ_MACHMSG family. The
send-translation FIX itself (#28q) is on by default and make-check stays green.

## UPDATE #28s (2026-06-24): ran a 10-agent workflow to break the LaunchServices _LSApplicationCheckIn wall; its top leads were well-reasoned but FAILED empirically. The wall is a black-box coreservicesd refusal I cannot crack without privileged (sudo) tracing. Mach-send fix still the session's landed deliverable; make-check green.

The workflow (break-ls-checkin-wall, 6 investigators + synth + adversarial verify) produced 3 ranked
faithful breakthroughs. I tested the top two against the REAL ocerz boot; both are disproven:
  * #1 DELETED-EXECUTABLE-VNODE (its highest-confidence): theory = coreservicesd resolves the checking-in
    pid via proc_pidpath_audittoken and silently never replies if the process's launch-time executable
    file is `(deleted)` on disk; the empirical agent PROVED this is a real deterministic trigger of the
    EXACT symptom under real Rosetta (unlink/move the spawned executable -> _LSApplicationCheckIn hangs
    forever), and ocerz links in place (Makefile) + re-execs THIS binary per child, so rebuilding during
    a boot makes children `(deleted)`. DISPROVEN as the cause here: ran from an immutable installed copy
    (~/ocerz-install/ocerz, never rebuilt during the boot), confirmed via `lsof -p <gui-child>` that the
    txt/executable vnode is PRESENT (no `(deleted)`), and it STILL stalls at id=10054. Same symptom,
    different unidentified cause. (Keep the operational hygiene anyway: run from a stable copy, never
    `make` during a boot.)
  * #2 TRANSLATION IDENTITY (proc_translated / oah): a real x86_64-under-Rosetta process is kernel-
    stamped translated (proc_translated=1, hw.cputype=x86_64); ocerz is native arm64. Added a FAITHFUL
    sys_sysctlbyname intercept (src/syscall.c) returning sysctl.proc_translated=1 (the guest IS a
    translated process) + OCERZ_SYSCTLLOG. RESULT: the guest issues ZERO sysctl.proc_translated queries
    during the boot -> it checks translation via the commpage (oah), not sysctl, so the intercept can't
    reach it. The workflow's shipping variant (launch ocerz as an x86_64 Rosetta STUB so the kernel
    stamps it translated) is self-contradictory: an arm64 JIT cannot run inside a Rosetta-x86_64 process.
  * #3 os_log diff: the _LSApplicationCheckIn CLIENT runs in-guest and its os_log SHOULD route through
    ocerz-translated libsystem_trace; captured `log show --process ocerz/wine` during the stall ->
    EMPTY (the guest's os_log does not reach the unified log under a queryable process), so the client
    decision strings are invisible; the SERVER (coreservicesd) reason is gated below the default log
    threshold (needs `sudo log config --mode level:debug` for LaunchServices).
  * Also surfaced: a SEPARATE real ocerz bug (breakthrough #2-stack) — a bare native x86_64 Cocoa app
    crashes under ocerz at SIGBUS in _malloc_zone_malloc during [NSApplication sharedApplication]
    (guest descends the 8MB DYN_STACK_SIZE main stack into the 16KB uncommitted inter-allocation gap
    [aux..stack]; mem.c:634 leaves only one host page, not a real guard). This blocks the decisive
    "does a NATIVE Cocoa app under ocerz also hang at the check-in?" comparison; the Wine boot itself
    reaches the check-in (its Cocoa runs on a Wine-managed stack), so it is not the Wine-window blocker
    but is a genuine faithful fix to make (larger / guard-page-aware on-demand-growing guest main stack,
    dyld.c:934/mem.c:634).

NET: coreservicesd receives the check-in (send succeeds, present vnode, real reply port) and SILENTLY
never replies, for a reason that is NOT: audit session (asid=0 Mac-wide), signature (adhoc), .app/ASN,
deleted vnode, proc_translated, a callback (zero traffic back), or anything in any log I can read
without root. It is a black-box LaunchServices/coreservicesd identity decision specific to running
x86_64 Wine under a THIRD-PARTY arm64 translator (Apple's Rosetta is kernel-recognized; ocerz is not).
The single most likely UNLOCK now needs the user (sudo): enable LaunchServices/coreservices debug
logging and reproduce to read coreservicesd's actual bail reason. Faithful, landed this session: the
Mach send-pointer translation (#28q) + the proc_translated sysctl intercept; both default-on, make-check
green; diagnostics gated (OCERZ_SYSCTLLOG added). The window remains blocked ONLY at this LS check-in.

## UPDATE #28t (2026-06-24): Path B — the native-Cocoa-under-ocerz crash that blocks the LS-wall scoping experiment is NOT a stack-size bug; it is an infinite malloc recursion via a CoreFoundation constant zone wrongly serving as the default malloc zone. Precisely root-caused; fix is a malloc-zone/init-ordering change. make-check green.

To scope whether the LaunchServices _LSApplicationCheckIn wall (#28r/#28s) is ocerz-fundamental or
Wine-specific, the plan was to run a bare native x86_64 Cocoa app under ocerz and see if it ALSO hangs
at the check-in. That app (miniapp_hang.m: [NSApplication sharedApplication] + finishLaunching) CRASHES
under ocerz inside sharedApplication, BEFORE reaching the check-in. Root-caused precisely (added an
r12-r15 + [r15+0x18] dump to the vm.c crash handler):
  * SIGBUS at host_addr=0x340103ff8 (rbp-0x38) in the UNCOMMITTED 16KB inter-allocation gap
    [0x340100000,0x340104000) that mem.c:634 (bump_next = gaddr+glen+OCERZ_HOST_PAGE) leaves between
    the 1MB `aux` and 8MB `stack` allocations -- a stack OVERFLOW, but the cause is INFINITE RECURSION,
    not stack size (the workflow's 256MB test ran longer and hit the SAME base, the tell).
  * The rbp-chain repeats two return addrs: libsystem_malloc+0x1d74f (public malloc, func_A) <->
    libsystem_malloc+0x39219 (malloc_zone_malloc, func_B). func_A calls func_B(+0x39122); func_B does
    `call [r15+0x18]` = zone->malloc; that returns into the public malloc -> loop.
  * r15 (the zone) = 0x7ff840095a00 = a malloc_zone_t in the shared cache's RO DATA mapping whose
    function pointers are ALL CoreFoundation (malloc=CF+0x10dad9, calloc=CF+0x10dae5, free=CF+0x10db64,
    realloc=CF+0x10db69) -- a CoreFoundation CONSTANT zone (cache chained ptrs, ocerz unchains them
    correctly to 0x7ff802fc0ad9). So `malloc -> malloc_zone_malloc(CF_zone) -> CF zone-malloc ->
    public malloc -> ...` because the DEFAULT malloc zone (malloc_zones[0]) resolves to this CF
    constant zone instead of the real runtime nano/scalable zone. CF's zone-malloc is meant to
    delegate to the REAL system zone, not loop.
  * Likely root: initializer ORDERING -- a native CoreFoundation app initializes CF very early, and
    under ocerz's mini-dyld CF registers/installs its zone before libmalloc creates+installs the real
    default zone, so the CF constant zone ends up as malloc_zones[0]. The Wine boot does not hit this
    (its malloc path / init order differs), which is why Wine reaches the LS check-in while the bare
    Cocoa app dies in sharedApplication.

IMPLICATION: this is a real, separate, FAITHFUL fix to make (native macOS Cocoa apps under ocerz need
the real default malloc zone, i.e. correct libmalloc-vs-CF init ordering / default-zone install) AND it
unblocks the decisive native-vs-Wine LS-check-in comparison. It is NOT the Wine-window blocker itself.
NEXT to fix it: trace where malloc_zones[0] is set under ocerz and why CF's constant zone wins; compare
ocerz's initializer order for libsystem_malloc vs CoreFoundation against real dyld; ensure libmalloc's
real default zone is installed first (or that CF's zone-malloc delegates to the real zone). Kept diag:
the vm.c crash dump now prints r12-r15 + [r15+0x18]. make-check green (syscall 96/0, decode 190).

## UPDATE #28u (2026-06-24): ★ LANDED the CF-init fix (native Cocoa apps no longer recurse-crash; make-check green, Wine unaffected) AND proved the LaunchServices wall is WINE-SPECIFIC, not ocerz-fundamental — a native Cocoa app under ocerz PASSES the same _LSApplicationCheckIn that Wine hangs on.

FIX LANDED (faithful, default-on, make-check green): the native-Cocoa malloc recursion (#28t) was NOT a
malloc/zone bug — proven by a workflow with a live OCERZ_ZONEPROBE: malloc_zones[0] is the REAL nano
zone in BOTH the crashing and working runs. The real defect: ocerz ran ONLY libSystem's initializer and
jumped to main, gating the full dependency-ordered initializer phase (run_load_phase+run_init_phase,
which runs __CFInitialize and every framework init like real dyld) behind getenv("OCERZ_INITPHASE").
Without __CFInitialize, CoreFoundation's runtime class-bridge table (___CFRuntimeBuiltinObjCClassTable)
stays ZERO, so __CFAllocatorAllocateImpl takes the branch that passes CF's own constant allocator
(___kCFAllocatorSystemDefault, the RO-cache zone whose malloc is ___CFAllocatorCustomMalloc) as the
zone -> malloc_type_zone_malloc -> [zone+0x18]=CF malloc -> public malloc -> infinite 2-frame loop ->
stack overflow. FIX (src/dyld.c): added a `links_cf` img flag set when the main image's LC_LOAD_DYLIBs
name CoreFoundation/Foundation/AppKit (dyld.c:517 scan), and changed the gate (dyld.c:2527) to run the
init phase when `(img.links_cf || OCERZ_INITPHASE) && !OCERZ_NOINITPHASE`. So native CF/Cocoa apps run
__CFInitialize before main (real-dyld behavior); the Wine PE loader links NONE of CF/Foundation/AppKit
in its main image (verified via otool -L) so its path is byte-for-byte unchanged. VERIFIED: ./ocerz
./miniapp (a native [NSApplication sharedApplication]+finishLaunching app) no longer crashes by default;
make-check green (syscall 96/0, decode 190, sse 246/0, loader 54/0). init_mark_done_closure still bounds
done-marking to /usr/lib/system/ umbrellas so malloc/pthread are not double-initialized.

★ MAJOR SCOPING RESULT (redirects the entire #28r-#28s LS investigation): with the init phase, the
native Cocoa app under ocerz sends the FULL LaunchServices check-in sequence id=10050/10019/10054/10052
to coreservicesd and CONTINUES PAST it (id=10052 follows id=10054 -> coreservicesd REPLIED), then does
the same service lookups as Wine (cfprefsd, launchservicesd, windowserver.active, dock.server,
runningboard, pasteboard, opendirectoryd, ...) and reaches a WindowServer-stage call (id=4, port
0x2a03) where it blocks. So _LSApplicationCheckIn does NOT fundamentally hang under ocerz -- the Wine
boot's hang at the SAME id=10054 is WINE-SPECIFIC (Wine's check-in message / process context differs
from a native app's, even though both run as the same ocerz arm64 process with the same executable
vnode). The earlier "coreservicesd black box / daemon-policy gap" framing (#28r) is SUPERSEDED: there is
now a WORKING reference (native app) to diff the FAILING case (Wine) against.

NEXT (two tracks, both now well-defined): (1) WINE LS: diff Wine's id=10054 check-in message + process
context against the native app's (which works) to find the Wine-specific difference that makes
coreservicesd withhold the reply. (2) DEEPER COMMON WALL: both the native app (at id=4) and (eventually)
Wine block at the WindowServer/CGS connection stage -- the known SkyLight/WindowServer frontier; a
window for EITHER requires getting past it. Kept gated diag: OCERZ_ZONEPROBE (workflow-added), the vm.c
r12-r15 + [r15+0x18] crash dump. Two faithful fixes landed this session total: Mach send-pointer
translation (#28q) + CF init phase (this).

## UPDATE #28v (2026-06-24): the deep COMMON wall (for both native Cocoa and Wine) is the WindowServer/CARenderServer connection inside [NSApplication sharedApplication] — request sent cleanly, WS never replies. Traced precisely via a native window-app repro.

Built winapp.m (native x86_64: sharedApplication + create NSWindow + makeKeyAndOrderFront + finishLaunching + run loop) and ran under ocerz (the CF-init fix #28u makes it get this far). It BLOCKS inside [NSApplication sharedApplication] (only "[winapp] start" prints, never "sharedApplication done"):
  * sample: main thread 100% parked in ocerz_handle_syscall (the mach_msg2 trap), 0% CPU -> a BLOCK, not
    a spin (distinct from the #16/#17 kevent spin).
  * Immediately-preceding bootstrap lookups (via coreservicesd id=1073742031): com.apple.windowserver.active
    AND com.apple.CARenderServer -> this is the CoreGraphics/SkyLight WindowServer connection handshake.
  * The app SENDS the WS connect request cleanly: id=96519, complex (bits=0x80130013), opts SEND|TIMEOUT
    to rport=0x7f03 (the WS port), lport=0 (so the reply port 0x2a03 is passed as a PORT DESCRIPTOR in the
    complex body, MAKE_SEND). ZERO MACH-SEND-ERR -> the kernel accepted it and the real WS received it.
  * The app then parks in a RCV-only mach_msg2 (opts=0x40700420e: MACH_RCV_MSG, NO RCV_TIMEOUT) on
    rport=0x2a03 waiting for the WS's reply -> the WS never sends -> deadlock forever. Deterministic.
So both paths converge here: Wine hangs earlier (its Wine-specific LS check-in, #28u) but would land here
too; the native app sails through LS and dies at the WS connection. A rendered window for EITHER requires
the WindowServer to complete this connection.

This is the same shape as the coreservicesd LS no-reply, but with the WindowServer — the strictest
GUI-session/identity-gated daemon (CGS/SkyLight). ocerz forwards the request faithfully (shared port
namespace, real kernel, real WS receives it); the WS chooses not to reply. Likely gated on GUI-session /
connection-policy the WS enforces on a process that is not a normal window-serving app (this Mac Studio is
headless/remote, asid=0 for everything, yet the user's real Rosetta-Wine DOES get windows here -- so it is
NOT a blanket headless refusal, it is something specific to ocerz's process/connection vs a Rosetta one).
Reading WHY the WS withholds the reply needs the WS/SkyLight side (privileged: `sudo log config
--subsystem com.apple.windowserver --mode level:debug`, or com.apple.SkyLight), analogous to the pending
coreservicesd trace. Alternatively a deep CGS-connection-protocol audit of ocerz's forwarding of the
id=96519 port-descriptor handshake. This is the project's central GUI frontier (cf. #16/#17 + the CLAUDE.md
D3DMetal/SkyLight notes). Repro kept: $SP/winapp(.m). No code change this update (diagnosis only);
make-check remains green; the two landed fixes this session (Mach send #28q, CF init #28u) stand.

## UPDATE #28w (2026-07-05): ★★★ #28v WAS A RED HERRING. The WindowServer DOES reply — it creates a live console connection. The real wall is a libdispatch dispatch_sync hang, and OCERZ_HOSTWQ=1 RESOLVES it (sharedApplication now RETURNS). Next wall: a memmove SIGBUS on an uncommitted arena page. No code landed yet (deep diagnosis); make-check green.

The user enabled daemon debug logging (`sudo log config --subsystem {com.apple.windowserver,SkyLight,
coreservices} --mode level:debug,persist:debug`). Reading it OVERTURNS #28v and kills a whole family of
prior theories (session/asid, "WS withholds reply", garbage audit token). Method that finally worked:
`log stream --level debug --style compact --predicate 'process=="WindowServer" OR ...'` DURING a fresh
winapp reproduce (NOT `log show`, which returned 0 lines — the persist config does not retroactively
populate for these daemons). What the daemons actually do for our ocerz pid:
  * launchservicesd: `CONNECTION ... pid=28082 euid=501 asid=100002` — **we ARE in GUI session 100002**,
    NOT session 0. The earlier "asid=0 everywhere" was measured on a DIFFERENT (Bash-tool python) process.
  * coreservicesd: `10050/ServerCheckinWithResult_rpc serverCheckIn(pid=28082 uid=501) -> session 100002`,
    then launchservicesd `CopyAndUpdatePendingApplication(token=auditToken{uid=501 euid=501 gid=20 egid=20
    auditSessionID=100002 pid=28082})` — the LS check-in SUCCEEDS with the CORRECT token. (The `success=false`
    replies I feared were negative results of `registerCheckOnPidInformation` probing OTHER pids, not our
    check-in. The one-off garbage `auditToken{pid=1...}` on the raw XPC connection is a launchservicesd
    logging artifact, not a real identity we present — the check-in and TCC both see us correctly.)
  * tccd: `AUTHREQ_RESULT authValue=2` (GRANTED) for kTCCServiceListenEvent, target_token{pid:28082,
    auid:501,euid:501} — correct token, TCC allows us.
  * WindowServer: `[SkyLight:packages] resumed connection 277ad3` + `[ConnectionDebug] New conn 0x277ad3,
    PID 28082 in session 257 on console` — **the WS CREATES A LIVE CONSOLE CONNECTION for us**, and keeps
    it alive until we're killed (`Closing conn 0x277ad3` only on kill). So #28v's "WS never replies" is
    WRONG. The `_CGXPackagesSetWindowConstraints: Invalid window` errors are background noise (they fire
    before our app even starts). The GUI plumbing (session, TCC, LS, WS connection) all WORKS.

So where does winapp actually hang? Symbolicated the blocked backtrace (helper $SP/sym.c: reads
sharedCacheBaseAddress+slide from TASK_DYLD_INFO in a native x86_64 process, maps ocerz's UNSLID 0x7ff8...
cache addresses to slid addresses, dladdr's them). The main thread is NOT in mach_msg — it is in a
LIBDISPATCH SYNC WAIT:
    __ulock_wait                       [libsystem_kernel]   (futex)
    _dlock_wait                        [libdispatch]
    _dispatch_thread_event_wait_slow   [libdispatch]
    __DISPATCH_WAIT_FOR_QUEUE__        [libdispatch]        <- dispatch_sync on a contended serial queue
    _dispatch_lane_push_waiter         [libdispatch]        <- main thread parked itself as a waiter
The main thread does `dispatch_sync` onto a serial queue currently OWNED by a worker; it waits for the
worker to drain the queue and signal its _dispatch_thread_event (ulock). The worker never runs/finishes
-> hang forever. The `mach_msg2 RCV on 0x2a03` I mis-identified in #28v was a DIFFERENT thread; the actual
wall is the SAME libdispatch worker/workqueue frontier as UPDATE #17 (the winemac.drv dispatch_mgr spin).

★ KEY RESULT: `OCERZ_HOSTWQ=1` RESOLVES the dispatch_sync hang. With it, winapp prints
"[winapp] sharedApplication done" — sharedApplication RETURNS (default synthetic workqueue: it hangs
before that marker). So the default synthetic worker pool cannot drain a dispatch_sync-contended serial
queue, but routing the workqueue to the REAL kernel (HOSTWQ) can. Strong signal: HOSTWQ (or fixing the
synthetic path to spawn/hand-off a worker on dispatch_sync contention) is the correct direction, and is
the thing that stands between ocerz and a rendered Cocoa window. (HOSTWQ_ASYNC=1 behaves the same here.)

NEXT WALL under HOSTWQ (the new frontier): the main thread then SIGBUSes in `_platform_memmove$VARIANT$
Rosetta` — a ~17MB (rdx=0x1063fe0) backward copy (dst=src+count+0x20, dst>src) whose source read faults
at a 16KB-page-aligned address minus 0x10 (0x3676c7ff0 = rsi(0x3676c8000)-0x10; deterministic across runs,
always ...c7ff0; icount ~0xab97329). Host insn 0xf94001ea = `LDR X10,[X15]` -> a READ. Proven this is NOT
a memmove over-read: a synthetic x86_64 test ($SP/mm2.c) that memmoves ~17MB with the page immediately
below a page-aligned src set PROT_NONE does NOT fault under native Rosetta OR under ocerz, for the crash
config AND overlapping/backward configs. So memmove$Rosetta reads exactly its source; the guest's source
buffer legitimately extends down to ...c7ff0, and ocerz has NOT backed that page — a read of an
uncommitted/unbacked guest ARENA page returns SIGBUS instead of demand-zero. ocerz's crash_handler
(src/vm.c) only commits-on-demand at depth>0 (nested frame-build); a depth-0 read fault on an uncommitted
arena page is delivered as a guest signal (native app: no handler -> fatal). The fix is almost certainly
in the arena commit path: an uncommitted-but-reserved page should be committed demand-zero on READ (MAP_ANON
semantics), not faulted. STILL TO CONFIRM: the exact host mapping/prot of the source page (lldb batch-mode
signal handling fought ocerz's own SIGBUS handler; retry via a live vmmap/region query or an in-handler
region dump). This memmove is the messenger; the bug is the arena backing of that page.

Method/tooling kept for reuse: $SP/winapp(.m) native Cocoa repro (setActivationPolicy:Regular + NSWindow +
makeKeyAndOrderFront; under NATIVE Rosetta it shows a real 480x352 on-screen window = clean A/B reference);
$SP/wlist(.m) CGWindowList on-screen detector (owner pid+bounds); $SP/sym(.c) cache symbolicator;
$SP/mm2(.c) memmove$Rosetta over-read probe. OCERZ_HOSTWQ=1 is the config that gets furthest. Two landed
fixes from the prior session (Mach send #28q, CF init #28u) still stand; make-check green.

## UPDATE #28x (2026-07-05): ★ LANDED a real fix — multi-region shared-mapping relocation. The CSStore SIGBUS (#28w) is GONE; winapp under OCERZ_HOSTWQ now runs clean through "[winapp] WINDOW UP" and REGISTERS an NSWindow with the WindowServer. make-check green (15/15 diff + 2/2 dyn). Two smaller walls remain before a VISIBLE window.

Root cause of the #28w SIGBUS, nailed by instrumenting mig_vm_reply_relocate (syscall.c): the LaunchServices
CSStore database (~17MB, delivered as "lsinfopage=SharedMemory") is mapped into the guest via a MIG
mach_vm_map (reply ids 4900/4911/4913). ocerz relocates the kernel-placed mapping into the arena, but it
sized the mapping with a SINGLE mach_vm_region call -> it only saw the FIRST vm region. The kernel had
SPLIT the object into multiple CONTIGUOUS regions (observed: 0x1060000 RW + a contiguous 0x20000 ANONYMOUS
COW region), so ocerz relocated only 0x1060000 of it. CSStore::VM::AllocateCopy then memmove'd the full
0x1063fe0 -> read 0x3fe0 past the truncated copy into the unmapped 16KB map_donate gap -> SIGBUS in
_platform_memmove$VARIANT$Rosetta. (Backtrace: _NSXPCSerializationDecodeInvocationArgumentArray ->
-[NSXPCDecoder _decodeObjectOfClasses:atObject:] -> -[_CSStore initWithCoder:] ->
CSStore::Store::CreateWithXPCObject/CreateWithBytes/_Create -> VM::AllocateCopy.) FIX (landed, syscall.c
mig_vm_reply_relocate): after the first mach_vm_region, WALK FORWARD coalescing every region CONTIGUOUS with
it (na == running_end) into one span until a real GAP, then donate+remap+deallocate the whole span. NB the
tail region is ANONYMOUS (no memory object), so an early has_obj-gated version still truncated — the guard
had to be dropped; a freshly-mapped object sits in free space so a gap (not an unrelated mapping) bounds
the walk. Also fixed a crash-handler NESTED-FAULT (vm.c: the `[r15+0x18]` speculative dump faulted for r15~0
and _exit(139)'d before the fault-page/host_prot/region diagnostic printed) by guarding that load on
ocerz_addr_committed -- this is how the "fault-page: UNCOMMITTED region=[..16KB..] host_prot=0x700(NONE)"
diagnostic became readable and pinned the gap. Diagnostics kept (OCERZ_MACHMSG): BRIDGE-OOL (per-OOL
descriptor type/addr/bytes + >16MB skip flag) and MIGRELOC (haddr/raddr/first_rsize/coalesced_size/nregions).

STATE NOW: winapp+HOSTWQ prints start -> sharedApplication done -> window created, ordering front ->
WINDOW UP, no ocerz fault, and CGWindowList(kCGWindowListOptionAll) shows owner=winapp EXISTS. REMAINING
before a visible window: (1) the NSWindow is onscreen=0 bounds=0x0 -- created/registered but not realized/
sized (surface/CA-commit or activation not completing under ocerz); (2) shortly after WINDOW UP the process
dies with `ocerz: fatal: initializer call to 0x3000007b0 aborted after ~6.08e8 instructions` -- a dyld
initializer ocerz runs faults (0x3000007b0 is just above OCERZ_LOW_LIMIT; likely a deferred framework init
from the #28u init-phase, or a bogus init function pointer). These are the next two targets. OCERZ_HOSTWQ is
still an env opt-in; making it (or an equivalent synthetic-workqueue dispatch_sync hand-off) the DEFAULT is
the standing decision once the path is proven to a visible window. $SP/wall(.m) = CGWindowList incl.
offscreen/alpha detector.

## UPDATE #28y (2026-07-05): the two post-WINDOW-UP walls both resolve to the CoreAnimation/Metal window-backing path — the GPU-rendering frontier. winapp+HOSTWQ steady state: creates an NSWindow (WS-registered, console session), but it stays onscreen=0 bounds=0x0, and ~608M instructions in an initializer aborts. No new fix (deep GPU territory); #28x fix + make-check green stand.

Characterized the remaining two walls precisely:
* THE 0x0/OFFSCREEN WINDOW: CGWindowList(kCGWindowListOptionAll) shows owner=winapp EXISTS but onscreen=0,
  bounds=0x0, alpha=1.0 — the window is registered with the WindowServer but its backing SURFACE never
  realizes (no CA/IOSurface committed -> WS has no size/shape). Under NATIVE Rosetta the identical binary
  shows 480x352. So a CGS/SkyLight window-shape or CoreAnimation surface-commit message is not landing.
* THE INITIALIZER ABORT (intermittent — HOSTWQ uses real kernel threads, so it is a RACE; ~608M insns,
  deterministic icount, timing-variable wall-clock): a guest UD2 at rip=0x7ff802b68adf = `_xpc_api_misuse
  +0x4e` [libxpc]. Caller neighborhood = `_xpc_bundle_variant_create_subpath.cold.1` [libxpc]; nearby
  cstrings "…_matching_key", "…integer_value_allowed", "…to match against during iteration" -> the misuse
  is an XPC DICTIONARY value that is nil/wrong-type during bundle-variant iteration. Reason prefix string
  (rax) = "API Misuse"; register fragments spell " OS_xpc_" / ": nil, r". This fires while loading
  `AGXMetalG14X.bundle` (the Apple-silicon GPU Metal driver, dlopen'd for the window's CA/Metal layer
  backing — DLPATH confirms it loads it, plus libobjc-trampolines). So an XPC object ocerz produced during
  the Metal driver's bundle load is nil where libxpc requires non-nil.
BOTH walls are the CA/Metal/GPU-driver rendering layer — the same class as the whole project's D3DMetal /
SkyLight history (CLAUDE.md). Getting a VISIBLE surface requires the CoreAnimation render-server
(com.apple.CARenderServer) + IOSurface + Metal path to work under ocerz, which is a multi-layer frontier,
not a single bug. Concrete next leads: (a) capture the WS SkyLight log for our window's create/shape to see
whether the WS receives 0x0 (ocerz CGS-message/OOL truncation, cf #28x) or a real size with no surface
(CA/IOSurface path); (b) trace the AGXMetalG14X bundle-load XPC dictionary that comes back nil (likely an
ocerz XPC/bundle-metadata or CFPreferences gap) — fixing it would also stop the intermittent abort.
Symbolication helpers: $SP/sym(.c), $SP/rdstr(.c) reads a cstring at a cache addr via TASK_DYLD_INFO slide.

## UPDATE #28z (2026-07-05): the intermittent post-WINDOW-UP abort is TRACED to libxpc's `_initial_images` global holding a NIL value — an ocerz image-list-presentation gap (cache-dylib enumeration), likely a POPULATION RACE under HOSTWQ. Added a guest-frame-pointer backtrace to the init-abort path; make-check green (96/0 syscall, 14/14 guest, 15/0 diff, 2/0 dyn).

Full guest call chain at the abort (new OCERZ initabort-bt fp-walk in vm.c, symbolicated):
  +[NSTextInputContext initialize]  [AppKit]   <- ObjC +initialize during window text-input setup
  +[NSRemoteView initializeOnAppKitThread] -> _ensureAuxServiceAwareOfHostApp -> auxiliaryProxyFor  [ViewBridge]
  +[NSXPCSharedListener connectToService:…] -> _endpointForListenerNamed:…  [ViewBridge]
  xpc_connection_resume -> _xpc_connection_init -> _xpc_uncork_domain -> _xpc_uncork_pid_domain_locked
  _xpc_init_pid_domain (+0x9c)  -> loads GLOBAL `_initial_images` (movq _initial_images,%rsi)
  _xpc_init_pid_domain_process_initial_images -> xpc_dictionary_apply -> _xpc_api_misuse (UD2)  [libxpc]
So NSTextInputContext's +initialize (part of window setup) opens a ViewBridge NSXPC connection; resuming it
runs _xpc_init_pid_domain, which xpc_dictionary_apply's over the libxpc GLOBAL `_initial_images` dict, and a
VALUE in it is nil ("API Misuse" + reg fragments " OS_xpc_"/": nil, r"). `_initial_images` is populated at
libxpc init from the process's INITIAL image set. KEY: OCERZ_IMGLOG shows ocerz_dyldapi_register_image is
called 0 times here — the initial images are all SHARED-CACHE dylibs, presented via ocerz's dyld4 APIs
object, NOT the disk-image path. So the nil is in ocerz's cache-image enumeration/notification to libxpc's
add-image handler (`_dyld_register_func_for_add_image`-style), and the fact that the abort is INTERMITTENT
(HOSTWQ = real kernel threads) strongly implies a RACE: real dyld serializes image loads + add-image
callbacks under the dyld lock; ocerz under HOSTWQ apparently notifies/loads images concurrently, so libxpc
iterates a half-populated `_initial_images` (transient nil value). This is the same class as the whole
Cocoa-under-ocerz concurrency story (#17). NEXT: find how ocerz drives libxpc's add-image callback for cache
dylibs and whether it is serialized vs the enumeration (dyldapi.c dyld4 APIs object); and separately the 0x0
CA surface (WS runs CreateApplication for winapp but no SetWindowShape with real dims lands -> the CA
transaction that sets shape/content never commits, same CA/render-server layer). Both are the CA/Metal/dyld
concurrency frontier the user chose to chase; this update pins the abort's exact mechanism. Diagnostics kept
(gated): OCERZ_IMGLOG (image registrations), plus the always-on initabort-bt fp backtrace on any init abort.
DECISIVE TEST (new OCERZ_INITTOL diagnostic forces init-tolerance = skip a faulting initializer): skipping
the XPC-misuse init does NOT realize the window — it stays onscreen=0 bounds=0x0 and the app still dies
downstream (a second fault). So the abort is NOT the window blocker; the 0x0 CA SURFACE is a separate,
DEEPER issue (the CoreAnimation layer tree / render-server surface never commits, independent of the
libxpc _initial_images abort). Both live in the CA/render-server + dyld-concurrency layer. Bottom line for
the session: window CREATION works and a real memory bug is fixed (#28x), but a VISIBLE surface needs the
CA/CARenderServer/IOSurface path under ocerz — a multi-part frontier, not one bug. OCERZ_HOSTWQ=1 is the
config that reaches all of this.

## UPDATE #28ab (2026-07-05): ★★★★★ A FULL COCOA WINDOW NOW RENDERS ON SCREEN AND PERSISTS under ocerz — CGWindowList reports onscreen=1 bounds=490x384, WINDOW UP reached, process survives. THREE real bugs fixed this grind (all make-check green: 96/0 syscall, 14/14 guest, 15/0 differential, 2/2 dynamic). This is the milestone the whole project was aiming at.

The "0x0 surface / crash cascade" of #28y/#28z/#28aa was NOT one deep frontier — it was three concrete,
independent bugs that the earlier walls had masked. Fixed in order (each exposed the next):

1) libdyld __progname was NULL (src/dyld.c). ROOT-CAUSED via an in-guest SIGSEGV catcher ($SP/catch2.c):
   xmlReadMemory("<r><x:e/></r>") crashed under ocerz but not Rosetta, in libxml2's dispatch_once quirk
   `__startElementNSNeedsUndeclaredPrefixQuirk` -> strcmp(rdi, "Microsoft Document Connection") with rdi=NULL.
   Walking the caller's GOT decode showed rdi = *(libdyld.dylib::__progname) = 0. getprogname() worked
   (that reads libsystem_c's copy, set via the ProgramVars/leaf path in build_frame) but libdyld exports its
   OWN __progname which real dyld sets at launch and ocerz had left NULL. AppKit asset init (SVG via CoreSVG)
   reads it -> deref NULL -> deterministic SIGSEGV a few seconds after the window drew. FIX: right after the
   existing `_environ` write in dyld.c, `ocerz_cache_resolve(&cache,"___progname")` and store the argv[0]
   basename (mirrors the ProgramVars leaf real dyld also writes to libdyld's __progname).

2) Missing SSE3 ADDSUBPS/ADDSUBPD (0F D0, mand F2/66) — src/decode.c + interp_sse.c + decode.h. With #1
   fixed the app ran further into framework vector math and ocerz's decoder hit `decode failed (-1)` on
   `f2 0f d0 f8` = addsubps xmm7,xmm0. Added the opcode (even lanes subtract a-b, odd lanes add a+b),
   decode case mirroring HADDP (0F 7C), interp impl, and the dispatch group; verified interp==jit correct
   ($SP/asub.c: 9 22 27 44 / 95 206, matches native).

3) ★THE KEYSTONE — iokit_user_client_trap (Mach trap 100) unhandled (src/syscall.c). With #1+#2 fixed the
   CoreAnimation commit reached the IOSurface path and ocerz fataled "unknown Mach trap: class=1 num=100".
   Backtrace: CA::Context::commit_transaction -> CA::Layer::prepare_commit/prepare_contents ->
   CA::Render::copy_image -> __csiCompressImageProviderCopyIOSurfaceWithOptions [CoreUI] -> IOSurfaceClientLock
   [IOSurface] -> iokit_user_client_trap [IOKit]. The window's IOSurface-backed layer/asset content cannot be
   committed to the render server without it. FIX: forward trap 100 to the host (a[0]=connection is a real
   host port via the shared namespace; p1..p6 are scalars the user-client trap method interprets; p5/p6 read
   from the stack like mach_msg2). With that, CA::Context::commit_transaction completes and the surface goes
   on screen.

RESULT (winapp2: setActivationPolicy:Regular + titled NSWindow + layer-backed content + forced
[w display]/[CATransaction flush] + run loop; OCERZ_HOSTWQ=1): ~4/6 runs reach WINDOW UP with
onscreen=1 bounds=490x384 and the process persists. (screencapture returns "could not create image from
display" — headless/remote Mac, no physical framebuffer to grab — but CGWindowList onscreen=1 + real bounds
is definitive, and the owner visually confirmed an earlier window.) REMAINING (intermittent, ~2/6 runs, the
HOSTWQ real-thread concurrency frontier, NOT a rendering bug): a dispatch_sync hang at "start", and an
occasional early death — both timing/race, the same libdispatch concurrency class as #17/#28z. The window
RENDERS; making it 100% deterministic is the next polish. Wine's winemac.drv drives the same Cocoa/CA/IOSurface
path, so this de-risks the actual Wine goal enormously. Repro/tools kept in $SP: winapp2/wall/sym/catch2/asub.
make-check green throughout.

RELIABILITY (measured): OCERZ_HOSTWQ=1 alone ~4/6 reach onscreen=1; OCERZ_HOSTWQ=1 + OCERZ_HOSTWQ_ASYNC=1 ~6/8
(75%). The ~25% failures are TWO intermittent races (NOT rendering, NOT the 3 fixed bugs), both the Mach/XPC/
libdispatch-under-HOSTWQ concurrency frontier (#17/#28z class):
  (a) dispatch_sync hang at "start" (main thread parks; a HOSTWQ worker doesn't reliably drain the contended
      serial queue in time).
  (b) SIGSEGV in XPC message DESERIALIZATION: xpc_receive_mach_msg -> _xpc_dictionary_create_from_received_
      message -> _xpc_dictionary_deserialize_apply -> _xpc_data_deserialize -> _xpc_data_get_wire_value+0x94,
      reading a bad guest low-shadow addr (varies: 0x10088c000, 0x1053f0000) — a mis-relocated/racing OOL wire
      pointer in a received XPC message (ties to ocerz_bridge_mach_msg inbound OOL; possibly the >16MB-OOL
      skip that leaves a stale host pointer, or a HOSTWQ receive race). These are the next targets for making
      the window 100% deterministic (needed for Wine's winemac.drv 5s Cocoa-startup timeout). NB: making
      OCERZ_HOSTWQ the DEFAULT is deferred — the synthetic workqueue is what wineboot uses; flipping the
      default must be regression-tested against the Wine boot path first.

## UPDATE #28aa (2026-07-05): ★★★ A WINDOW VISIBLY RENDERED ON SCREEN under ocerz (user confirmed by eye). The CoreAnimation surface path WORKS. It renders then the process dies ~seconds later on a libxml2 NULL-global deref during AppKit asset init. make-check green.

How the window rendered: $SP/winapp2.m / winapp3.m force the CA surface synchronously — after makeKeyAndOrderFront:
setActivationPolicy:Regular + activateIgnoringOtherApps + [w display] + [CATransaction flush]/[commit], and
[CATransaction flush] every run-loop cycle. Under OCERZ_HOSTWQ=1 (+OCERZ_INITTOL=1) the user SAW the window
appear and close. So ocerz's CA/render-server/IOSurface path can produce a real on-screen surface — the #28y
"0x0 window" was the LAZY commit not firing; forcing it works. (CGWindowList still reports 0x0 for the app
placeholder, but the visible surface is real.) This is the biggest milestone: ocerz renders a Cocoa window.
WHY IT DOESN'T PERSIST: deterministic SIGSEGV, guest_addr=0x0, rip=_platform_strcmp+0x55, in a libxml2
dispatch_once quirk `__startElementNSNeedsUndeclaredPrefixQuirk` (parsing an SVG asset via CoreSVG during
AppKit init; backtrace CoreSVG SVGReader -> xmlParseElement -> ... -> strcmp). The quirk does
`rax=<cache global>; rdi=*rax; strcmp(rdi, "Microsoft Document Connection")` and rdi (the name) is NULL under
ocerz. RULED OUT: it is NOT __progname/argv0/NSProcessInfo.processName (all valid under ocerz, verified by
$SP/pnprobe). It is a specific cache DATA global (unslid ~0x7ff830c99938, in a higher subcache) whose target
is NULL under ocerz but populated natively — likely a cross-dylib symbol-binding gap OR a value real dyld
populates that ocerz's mini-dyld doesn't. RELATED SMELL: ocerz's _dyld_image_count returns 595 (the WHOLE
shared cache) vs Rosetta's 45 (actually-linked) — ocerz over-reports every cache dylib as loaded (nullscan:
595 images, 0 NULL names, so not the direct cause, but the same over-eager cache-image model may feed the
libxpc _initial_images nil of #28z). Rosetta does NOT hit this quirk in 40s -> under ocerz the app takes a
different (fallback) asset path, likely downstream of the ViewBridge/XPC image-list issue. NEXT to make the
window PERSIST: identify the NULL cache global (disassemble the quirk in a process with libxml2 mapped; read
*global), and/or fix the 595-vs-45 image-count model + the _initial_images nil. Repro: $SP/winapp2, winapp3
(borderless), pnprobe, nullscan, nameprobe. Diagnostic knob OCERZ_INITTOL (force init-tolerance).

## UPDATE #28ac (2026-07-05): TSO ordering fix LANDED (correct, make-check green) — but it is NOT the binding wall. Two big results this session (via ultracode multi-agent workflows): (1) ★ the GHOST WINDOW IS NOT A GPU/SURFACE PROBLEM — the IOSurface CPU backing is PROVABLY coherent cross-process; (2) ★ the real binding wall (convergent from independent agents) is the libdispatch event-source↔HOSTWQ dispatch gap (#17) + a secondary inbound-OOL crash. No GPU/Metal work is needed for CPU-drawn windows.

TSO FIX (landed, make-check 96/0 + 14/14 + differential interp==jit 15/0 + 2/0, RCsc superset of x86 TSO):
diagnosed root cause = ocerz translated PLAIN guest loads/stores to UNORDERED arm64 ldr/str in both tiers (only
atomics/fences were ordered). Fix: guest loads->load-acquire, stores->store-release. Files: include/ocerz/mem.h
(ocerz_ld/ocerz_st -> __atomic_load_n/store_n ACQUIRE/RELEASE for naturally-aligned 1/2/4/8, else memcpy +
__atomic_thread_fence; ocerz_ld128/st128 -> two 8-byte ACQ/REL atomics when aligned, else fenced memcpy);
src/a64emit.c + include/ocerz/a64emit.h (new a64_ldar/a64_stlr/a64_dmb_ish, encodings LDAR x/w/h/b =
c8/88/48/08 dffc00, STLR = ...9ffc00, DMB ISH = d5033bbf, byte-verified vs clang); src/jit.c (new
emit_guest_load_ordered/emit_guest_store_ordered wired into emit_mov_mem + emit_arith_mem — the guest EA already
lands in base reg JTA at offset 0, so the base-only [Xn] ldar/stlr form fits). KEY WRINKLE: LDAR/STLR
ALIGNMENT-FAULT on unaligned addresses (x86 allows unaligned), so the JIT emits a runtime split: movz scratch,
#(size-1); ands xzr,JTA,scratch; b.eq aligned; [unaligned: DMB ISH + plain str / plain ldr + DMB ISH]; aligned:
stlr/ldar (scratch = JTU, dead after holding gbase). Also landed the confirmed latent adc/sbb bug: interp.c
op_arith hoisted `int cin = CF` OUT of the locked-RMW retry loop. PERF FOLLOW-UP: the per-access alignment check
adds ~3 insns+branch on the hottest path; optimize later via Apple-Silicon hardware-TSO mode (Rosetta's trick,
privileged) or a SIGBUS-based unaligned fallback. MEASURED IMPACT: the fix is correct + removes a real latent
hazard but gave NO measurable app-level reliability gain (winapp2 ~50% window-register, Calculator 0/6 and
Stickies 0/3 never launch) — the binding walls are elsewhere. It is a necessary prerequisite, kept.

★ GHOST WINDOW = NOT GPU / NOT SURFACE MEMORY (proven, huge de-risk). Cross-process experiment ($SP/xsurf.m): a
writer running UNDER ocerz created IOSurface id=34, locked (via forwarded iokit_user_client_trap trap-100,
conn=0x1303 index=2=lock/3=unlock), wrote a pattern into the mapped base; a SEPARATE NATIVE process (the
WindowServer's mechanism) IOSurfaceLookup'd the same global id, got an INDEPENDENT mapping at a different address,
and read back the identical bytes -> COHERENT. Mechanism: the dynamic loader path is IDENTITY-mapped (guest_base=0,
ocerz_g2h(G)=G; src/mem.c:432, src/dyld.c:2415), so the guest touches the REAL host IOSurface pages directly; and
mig_vm_reply_relocate (syscall.c:2573) uses mach_vm_remap(copy=FALSE, syscall.c:2626) which ALIASES, never copies.
So a guest's drawn pixels land in the real kernel IOSurface any IOSurfaceLookup (WindowServer's, by id or send-right
in the shared Mach namespace) will composite. The window is blank ONLY because the guest's CoreAnimation
content-render+commit never COMPLETES (it spins or faults before drawing); attachment is strictly downstream and
not defective. The prior mach_vm_map/0x10d164000 IOSurface-relocation theory (#28aa) is a DEAD END for pixels.

★ THE BINDING WALL (convergent, both the impact-measurement agent AND the ghost agent independently): the
libdispatch event-source <-> OCERZ_HOSTWQ workqueue dispatch gap = the #16/#17 frontier. An event source (kevent
EVFILT_READ on ~21 fds/Mach ports, LEVEL-triggered, data!=0) re-fires forever because its handler never runs on a
worker to drain it -> __ulock_wait / libdispatch lock LIVELOCK @96% CPU -> CoreAnimation's display/commit runloop
sources never drain -> the app never reaches drawRect/commit. This blocks Calculator (0/6, main-thread samples in
libxpc +0x1859a / __ulock_wait / libswiftCore), Stickies (0/3), and ~half of winapp2. ORTHOGONAL to memory ordering
(TSO fix can't touch it). This is THE thing to fix for real apps to draw.

SECONDARY (intermittent XPC crashes): the inbound-OOL gap in the SYNCHRONOUS mach_msg2 handler (syscall.c case 47,
~2774-2955). It relocates only the mach_vm reply scalars (ids 4900/4911/4913) + the 3712 thread handle, but NEVER
relocates inbound OOL descriptors of RECEIVED complex replies. So a synchronous XPC/MIG reply carrying OOL leaves
raw host pointers in the guest buffer; under identity g2h these work only while the host mapping is alive, and
libxpc faults where it has been freed (host 0x102bec000, on the AppKit->LaunchServices->libxpc checkin path;
fault-page host_prot=0). FIX DIRECTION: add an inbound-OOL relocate on case-47 complex replies mirroring
ocerz_bridge_mach_msg (syscall.c:1034, which today copies OOL only on the HOSTWQ kevent path) but SHARING (identity
g2h) rather than memcpy-copying; OR stop relocating+deallocating kernel mappings the guest can already reach.

NEXT TARGET: fix the #17 libdispatch event-source dispatch gap (-> apps draw -> the already-coherent surface
attaches -> PIXELS) + the inbound-OOL crash. Repro/tools in $SP: xsurf.m/iosurf.m (surface coherency), win.m
(full Cocoa app), winapp2/wall/sym. Two workflows ran (wf_e35841d7 diagnosis recovered from transcripts;
wf_9bc948e1 TSO impl+verify+ghost). make-check green throughout.

## UPDATE #28ad (2026-07-05): OOL fix LANDED (case-47 inbound-OOL relocate, validated firing) + the binding wall (#17 dispatch gap) PRECISELY diagnosed — but NOT closed. The real bug is a WRONG WORKQUEUE FLAG CONSTANT + missing workloop-worker drain plumbing. The flag fix alone crashes; the remaining work is mapped exactly.

LANDED — OOL fix (src/syscall.c, make-check green incl differential 15/0): new static ocerz_reply_relocate_ool()
called from the case-47 mach_msg2 handler (guarded: a[1]&0x2 RCV, flat-not-vector, r47==0 success, rcv_size from
a[6]). Mirrors ocerz_bridge_mach_msg's OOL descriptor loop — walks a RECEIVED COMPLEX reply body, memcpys each
OOL/OOL_PORTS/OOL_VOLATILE descriptor's kernel-owned host buffer into a fresh guest arena buffer, patches the
descriptor. Before this, case 47 relocated only the mach_vm reply scalars (ids 4900/4911/4913) + the 3712 handle,
so received OOL kept raw below-arena host VAs (e.g. 0x102bec000/0x101018000/0x101038000) that ocerz never tracked
(region_for_range NULL, so the guest's own vm_deallocate no-ops) -> libxpc UAF-faults on the AppKit->LaunchServices
check-in. VALIDATED LIVE: during Calculator boot it fired on real host OOL (0x101018000/116KB, 0x101038000/1.4KB)
and relocated them; the arena copy is freed by the guest's own vm_deallocate exactly like a native receiver.
LIMITATION: FLAT complex replies only; MACH64_MSG_VECTOR receives skipped (follow-up). Crash-rate reduction NOT
measurable (intermittent + environmental noise).

★ BINDING WALL PRECISELY DIAGNOSED (the #16/#17 libdispatch dispatch gap) — the true bug: ocerz's workqueue FLAG
CONSTANT is WRONG. Ground truth from disassembling _pthread_wqthread on THIS machine (scratchpad/wqdecode.c, in-C
TBNZ decode via dlsym — lldb is SIP/attach-blocked here): guest R8 flag bits route the worker — bit22 (0x400000) =
WORKLOOP -> _dispatch_workloop_worker_thread (3-arg, vtable+0x8, kernreturn op 0x100); bit19 (0x80000) = KEVENT ->
_dispatch_kevent_worker_thread (2-arg, +0x30, op 0x40); neither -> _dispatch_worker_thread2 (1-arg, +0x20, op 0x4);
bit14 (0x4000) = QoS. BUT src/syscall.c:905 defines OCERZ_WQ_FLAG_WORKLOOP = 0x80000 — that is objectively the
KEVENT bit; the true WORKLOOP bit 0x400000 is NEVER set. So dispatch WORKLOOP workers (which own the CoreAnimation/
CFRunLoop read/machport sources) were routed to _dispatch_kevent_worker_thread and the kernel-supplied workloop_id
(at keventlist-8, confirmed) was DROPPED -> the workloop's sources never drain -> its thread-request is never
satisfied -> the MAIN thread re-commits it forever -> ocerz spawns a FRESH inline worker per re-fire -> unbounded
worker churn thrashes a GLOBAL objc os_unfair_lock (cache __DATA 0x7ff8436ac940) whose emulated critical section
is enormous -> the MAIN thread (mach port 0x103) STARVES in __ulock_wait2 on that lock, never wins, RIPDUMP pins it
at libsystem_kernel 0x7ff802e31336 for 12+s -> Cocoa startup never reaches CoreAnimation display/commit -> BLANK
WINDOW. Confirmed via new env-gated diagnostics left in syscall.c: OCERZ_KEVID (kevent_id workloop commits),
OCERZ_ULOCKLOG (ulock_wait2/wake targets), OCERZ_WQHIST (per-worker rip-history at exit).

WHY THE FLAG FIX ALONE CRASHES (10/10 guest_rip=0x0, REVERTED): routing workloop workers to the TRUE
_dispatch_workloop_worker_thread drain null-calls, because ocerz spawns FRESH inline workers (fresh OcerzCPU +
synthetic pthread + fresh mach port) per re-fire, lacking the persistent workqueue-thread context/TSD the workloop
drain dereferences (a continuation IMP comes back null). EXACT REMAINING WORK to close it (3 items, mapped by the
agent): (1) make the guest dispatch workloop object resolvable from the kernel-delivered workloop_id (forwarded
through the host kernel; verify id->dispatch_workloop_t and that its state permits an inline-worker drain);
(2) sys_workq_kernreturn op 0x40/0x100 must cleanly TERMINATE/park the inline worker (currently a ret_ok(0) stub);
(3) copy the guest worker's OUTGOING evbuf re-arm changelist+count back into the host events/*nevents so EV_DISPATCH
sources re-arm (both callbacks force *nevents=0). Fix loci: src/syscall.c ocerz_hostwq_bridge (~1271),
ocerz_hostwq_workloop_cb (~1419), ocerz_hostwq_kevent_cb, sys_workq_kernreturn (~824), the flag defines (~904-905),
ocerz_spawn_workloop_worker (~978). This is the deepest HOSTWQ frontier; the fresh-inline-worker model may need to
become a persistent-worker/TSD model. Ground-truth (bit constants, keventlist-8 id, ops 0x4/0x40/0x100, vtable
0x8/0x20/0x30) reproducible via scratchpad/wqdecode.c. NB the ghost surface is NOT the blocker (surface coherency
proven #28ac) — closing this dispatch gap is what makes apps DRAW.

MACHINE: agents' native x86_64 symbolication (dladdr) WEDGES Rosetta into unkillable U-state procs while ocerz
spins -> environmental noise (flaky winapp2 rate, transient test_mmap_fixed_outside flake — the pristine baseline
flakes identically, proven not-our-code). A REBOOT clears the wedged procs for reliable measurement. Prefer the
region-map/wqdecode symbolication (compute dylib base statically) over live dladdr while ocerz runs.

## UPDATE #28ae (2026-07-16): ★ THE #16/#17 DISPATCH GAP IS CLOSED AND VERIFIED — root was a KERNEL-FEATURE-ADVERTISEMENT bug, not a worker-context defect. + guest pthread-list corruption FIXED. ★★ BUT THE CENTRAL THEORY IS WRONG: with the dispatch gap closed, APPS STILL DO NOT DRAW (windows remain onscreen=no/0x0, byte-identical to baseline). The dispatch gap was NOT the drawing blocker.

DISPATCH GAP — ROOT (conf 0.95, A/B proven both directions, then reverted): the guest_rip=0x0 null-call is NOT
inside _dispatch_workloop_worker_thread. _pthread_wqthread's bit22 (0x400000 WORKLOOP) branch loads libpthread's
__libdispatch_workloopfunction from cache __DATA 0x7ff8436bd638 and `call rax`; that slot was 0. WHY: ocerz's
sys_bsdthread_register (syscall.c:427) returned the pthread feature word 0x4000005f, which lacks bit7 =
PTHREAD_FEATURE_WORKLOOP (0x80). Guest libdispatch's __dispatch_root_queues_init_once tests bit7; with it clear it
takes the KEVENT-ONLY model and passes workloop_func=NULL to _pthread_workqueue_init_with_workloop, and libpthread
stores that NULL. So ocerz simultaneously told the guest "no workloop support" AND delivered bit22 workloop workers
— the two halves of the workqueue contract disagreed. The 10/10 rip=0 that made the flag fix look "insufficient"
(#28ad) was just the other half of the same contract bug. FIX (already in tree, verified live): feat |= 0x80 gated
on OCERZ_HOSTWQ (so make check, which runs without HOSTWQ, still sees byte-identical 0x4000005f) + WORKLOOP=0x400000
/ KEVENT=0x80000 + workloop_id at evbuf-8 + op 0x40/0x100 terminate. Host slot values confirmed:
0x7ff8436bd638=_dispatch_workloop_worker_thread, 0x7ff8436bd660=_dispatch_kevent_worker_thread,
0x7ff8436beed0=_dispatch_worker_thread2.
★ THE "FRESH INLINE WORKER LACKS PERSISTENT TSD -> NEEDS ARCHITECTURAL REWRITE" THEORY IS FALSIFIED. With bit7
advertised, ocerz's existing fresh inline workers (fresh OcerzCPU + synthetic pthread + fresh mach port per
callback) run the REAL workloop drain with NO fault: 215/215 workloop callbacks, r8=0x480000, WQ-ENTER/WQ-EXIT 1:1,
workers park via workq_kernreturn op 0x100, 0 guest_rip=0 across 12 runs. libdispatch establishes its own per-thread
state; the 5 things it needs (synthetic pthread + cookie-obfuscated self, gs_base=pth+0xe0, a REAL kernel thread
port at pth+0xf8, evbuf at pth+0x8000 with workloop_id at evbuf-8, R8 flags) are what the bridge already supplies.
The workloop_id needs NO translation — the kernel hands back the guest's own dispatch_workloop_t heap pointer.
MAIN-THREAD STARVATION GONE: RIPDUMP shows ZERO samples at the old starvation site 0x7ff802e31336; ULOCKLOG shows
main now WAKING the objc lock 0x7ff8436ac940 instead of waiting on it (inverted).

LANDED THIS PASS — guest pthread-list corruption (syscall.c): every workqueue callback re-ran
_pthread_wqthread_setup on an already-linked node -> _pthread_count grew unbounded (2,29,57,82,110,138,167 over 151
callbacks) with observed selfcycle (self->next == self). Fixed via WQ_FLAG_THREAD_REUSE + cookie handling ->
2,7,12,17,33 over 101 callbacks (per-callback re-registration eliminated). Residual growth = a per-HOST-thread
registration leak (not yet fixed). make check green.
REFUTED BY MEASUREMENT (do not re-attempt): (a) "the bridge's nev<=0 early return drops zero-event workloop
wakeups" — FALSE: nev is NEVER 0 (215/215 callbacks carry nev=1,2,16); the early return never fires. (b) the
evcap/re-arm-truncation "fix" — implementing it was measured as a 14000x REGRESSION; rejected.

★★ THE CRUCIAL CORRECTION — THE DISPATCH GAP WAS NOT THE DRAWING BLOCKER. With it closed and verified, apps STILL
DO NOT DRAW: winapp2 registers 5 windows, ALL onscreen=no with 0x0 bounds — BYTE-IDENTICAL to the pre-fix baseline
(arm64 CGWindowListCopyWindowInfo check). '[winapp2] WINDOW UP' printing does NOT imply an onscreen window (it only
means the app's own code ran past [w display]/[CATransaction flush]). ONSCREEN NSWindow: 0/12. winapp2 WINDOW-UP
rate 7/12 vs 5/12 baseline = NOT significant at n=12 (one noisy ~50% distribution; the older "50-75%" and "2/6"
figures are the same distribution). Calculator: 0 windows registered, no crash, no GUI, unchanged — it is NOT gated
by the dispatch path at all. CONCLUSION: the #28ac/#28ad claim that the dispatch gap is "the ONE binding wall for
apps to draw" is WRONG. Drawing is blocked by something else, still unidentified. What we now know for certain:
IOSurface memory is coherent (#28ac), the dispatch/workloop path works, the main thread is no longer starved — yet
no window ever acquires geometry or a surface. NEXT: find why an NSWindow never gets real bounds/shape committed to
the WindowServer (earlier WS logs showed CreateApplication + SetNotifications but NEVER a window-shape message),
i.e. investigate the CGS/SkyLight window-create + shape path itself rather than dispatch or IOSurface.

---

## UPDATE #29 (2026-07-16) — REAL macOS APPS: the objc `+load`/category wall destroyed

Target this session: make a **real** macOS application (not a synthetic test) create a window
under ocerz. Test apps: `/Applications/Mousecape.app` (x86_64-only Cocoa) and
`/Applications/Platypus.app`. `make check` green throughout (differential interp==jit 15/0).

### Test-target methodology (important, cost me a false start)
* `/System/Applications/*` apps (Calculator, TextEdit) are **launch-constrained**: exec'ing them
  directly from a shell gets `EXC_CRASH (SIGKILL Code Signature Invalid) / Namespace CODESIGNING,
  Code 4, Launch Constraint Violation` **natively, with no translator involved** (rip=<unavailable>,
  all regs 0, zero frames). They are NOT valid ocerz targets. The old note "Calculator under ocerz:
  0 windows, no crash" was measuring this, not ocerz. Use non-system apps under /Applications.
* `CGWindowListCreateImage` / `screencapture -l<id>` return **no image** for windows that are
  definitely real and rendered, when the caller lacks Screen Recording permission. A native
  Rosetta control returns `no-img` identically. **`no-img` is NOT evidence of a ghost window** --
  the earlier "the window is a ghost" conclusion was unsound.
* `OBJC_PRINT_*` env vars apply to **ocerz's own host arm64 process** too, and both share a pid.
  `./ocerz` with NO guest prints "IMAGES: processing 6 newly-mapped images" + "performing initial
  category attach". That 6-image batch is HOST noise; the guest's batch is the 426-image one.
  Always baseline with `./ocerz` (no guest) before reading objc output.

### Bugs found and FIXED (all faithful; each verified by measurement)
1. **BSD syscalls missing** (`src/syscall.c` bsd_table): `[441] guarded_open_np` (5 args,
   ptr_mask 0x03), `[443] guarded_kqueue_np`, `[322] iopolicysys` (2, 0x02),
   `[466] faccessat` (4, 0x02). Identified from `$(xcrun --show-sdk-path)/usr/include/sys/syscall.h`
   and confirmed against the observed registers (441: rdx=0x1f = the standard GUARD_CLOSE|DUP|
   SOCKET_IPC|FILEPORT|WRITE set; 466: rdi=0xfffffffe = AT_FDCWD). guarded_open_np alone took
   Mousecape from 49M to 1.96B instructions.
2. **Mach trap 40 = `_kernelrpc_mach_port_get_attributes_trap`** (`dispatch_mach` case 40):
   args 3/4 are guest out-buffers and need g2h; ports pass through. Identity proven against the
   LIVE shared cache: the symbol is at unslid 0x7ff802e2faf0 and the faulting rip 0x7ff802e2fafa
   is +0xa (a 12-byte stub; next symbol at 0x7ff802e2fafc), corroborated by ret=0x7ff802e31c2a
   landing inside `mach_port_get_attributes`+0x22.
3. **★ ABSOLUTE export symbols** (`src/cache.c`, `include/ocerz/cache.h`, `src/dyld.c`).
   libobjc exports `__objc_empty_vtable` as `[absolute]` with value **0**
   (`dyld_info -exports /usr/lib/libobjc.A.dylib`). An ABSOLUTE export's trie value is literal and
   must NOT be slid by the image base. ocerz (a) always added the image base and (b) used
   `value == 0` as its "not found" sentinel through the whole resolve chain, so it could never
   represent "found, value 0" -> `OCERZ_FATAL("unresolved import: __objc_empty_vtable")` x114.
   Old ObjC class structures bind that symbol into **class+0x18** (the modern runtime's cache_t
   mask/occupied word), so 56 of Mousecape's class objects carried their raw on-disk fixup
   encoding -> corrupt method cache + superclass chain -> **libswiftCore spun forever at 200% CPU**
   in `swift_conformsToProtocolMaybeInstantiateSuperclasses` -> `getMatchingType` ->
   `getSuperclassForMaybeIncompleteMetadata` (a superclass walk that never terminates).
   FIX: parse `EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE` (flags&3==2), return the value unslid, and thread
   a real `found` flag through `trie_lookup` / `resolve_in_dylib` / `ocerz_cache_resolve_ex` /
   `ocerz_image_self_resolve_ex` / `disk_flat_resolve_ex` / `resolve_import`. 114 -> 0 errors.
4. **`_dyld_register_for_bulk_image_loads` (vtable slot 0x290)** was unimplemented -> returned 0,
   dropping the registration. libxpc registers `_xpc_dyld_image_callback` there from
   `_xpc_collect_images` and builds its "initial images" dictionary INSIDE that callback; the
   dropped registration left libxpc's global (0x7ff8436b33e8, __DATA_DIRTY+0xb38) NULL, and
   `_xpc_init_pid_domain` later handed that NULL to `xpc_dictionary_apply` -> `_xpc_api_misuse`
   abort, on the ViewBridge/NSRemoteView path AppKit takes from `+[NSTextInputContext initialize]`.
   Slot number read from the libdyld trampoline (`mov rax,[rax+0x290]`); callback arrives in rsi.
   Real dyld invokes it synchronously with every already-loaded image. Proven by the runtime:
   `unimplemented vtable slot +0x290 (a0=0x7ff802b2e114=_xpc_dyld_image_callback,
   caller=_xpc_collect_images+0x26)`.
5. **★★ THE BIG ONE — objc `+load` and ALL categories never ran** (`src/dyldapi.c`). Three
   compounding bugs:
   a. `ocerz_dyldapi_run_image_loads` **hand-rolled the +load scan** (walking __objc_nlclslist /
      __objc_nlcatlist and calling each IMP itself), completely bypassing libobjc's `init`
      (load_images) callback. ocerz captured only `mapped` (cb+0x08) and never `init` (cb+0x10).
      `load_images` is the ONLY caller of `loadAllCategoriesIfNeeded`, and objc defers EVERY
      category present at startup until then -- so no category anywhere ever attached.
      FIX: capture `g_objc_init_cb` and drive `load_images(info)` from the load phase (which is
      also correctly AFTER `_dyld_objc_register_callbacks` returns -- objc gates the attach on
      `didCallDyldNotifyRegister`, verified live: didInitialAttachCategories=0,
      didCallDyldNotifyRegister=1 at that point).
   b. **`_dyld_section_location_kind` enum was shifted.** ocerz's table went classlist[12] ->
      nlclslist[13] -> catlist[14], missing dyld's `__objc_stublist` entry at **14**, so every kind
      from 14 up was off by one (asking for catlist(15) returned "__objc_catlist2", absent) and
      kinds 18/19/20 (protolist/fork_ok/rawisa -- which libobjc really requests) fell off the end.
      Mapping derived from the REAL dyld, not guessed: called its own `_dyld_lookup_section_info`
      for each kind and matched the returned address to the image's sections ->
      **6=imageinfo, 12=classlist, 13=nlclslist, 15=catlist, 17=nlcatlist** (see `$SP/enum.m`).
   c. **`_dyld_objc_notify_mapped_info` layout was wrong**: it is
      `{mh@0, path@0x08, sectionLocations@0x10, objcImageInfo@0x18}`, not
      `{mh, objcImageInfo, path, sectionLocations}`. Proven from `load_images`'s own prologue:
      it consumes `[rbx+8]` as a `%s` for "IMAGES: calling +load methods in %s" and passes
      `[rbx+0x10]` as the sectionInfo arg. Also: `sectionLocations` must be **non-NULL** --
      libobjc treats a null handle as "no sections". ocerz IS the dyld, so the handle is opaque
      and ocerz's own slot 0x378 derives sections from the mach_header; ocerz's handle for an
      image is therefore its **mach_header**.
   d. **Slot 0x378 hid objc sections from shared-cache images.** That is correct for the class/
      category DISCOVERY kinds (objc has preoptimized data; answering from the Mach-O too makes it
      register every cache class twice -- measured: "Class QLTBitmapImage is implemented in both
      ... and ..."), but the **+load MARKER kinds (nlclslist=13, nlcatlist=17) must be reported for
      every image**: `load_images` early-returns unless `hasLoadMethods()` sees one, so hiding them
      from the cache meant Foundation's +load never ran and nothing ever triggered the category
      attach. FIX: report swift kinds + the two +load markers for all images; keep discovery kinds
      cache-hidden. Verified: +load RAN, categories ATTACHED, **0 duplicate-class warnings**.

### Verified results
* `+load` now runs (it never did before: NATIVE "+load RAN" vs OCERZ silence -> now identical).
* Categories now attach: `class_getInstanceMethod(NSString, ocerzCatProbe)` NULL -> non-NULL;
  both cache-class and local-class categories (repro: `$SP/cat3.m`, `$SP/cat4.m`).
* **Platypus**: the `-[_NSTaggedPointerColor inverted]` unrecognized-selector NSException out of
  `NSApplicationMain` -> `loadNibNamed:` (its own `NSColor(Inverted)` category, 56 category binds
  in `__DATA,__objc_const`) is GONE; it now loads its nib and registers windows.
* **Mousecape**: runs end-to-end with ZERO ocerz faults, loads BOTH nibs
  (`Base.lproj/MainMenu.nib`, `Base.lproj/Library.nib`), reads its Info.plist (NSBundle resolves),
  emits real AppKit NSLog diagnostics, registers windows with the WindowServer under its own name,
  and parks its main thread in `mach_msg2_trap` (a healthy idle Cocoa run loop, not a deadlock).
  Before the category fix it reached **4 windows at 3840x30 -- byte-identical geometry to the
  native Rosetta control's 4 menu-bar windows** -- and idled at 0% CPU.

### Remaining wall (next)
The app's own main window (native: `on=True 711x339`) still does not acquire geometry / go
onscreen; the menu-bar windows do. Non-fatal `NSXPCSharedListener 'ClientCallsAuxiliary':
Connection invalid` (ViewBridge auxiliary) is logged and survived. Next: find why the main
NSWindow is never ordered front / never gets its shape committed (does the app's delegate
`applicationDidFinishLaunching:` fire? is the launch AppleEvent / LaunchServices check-in
needed?), now that objc categories/+load are no longer masking it.

### Kept diagnostics (all env-gated)
`OCERZ_SYSPROBE` (enumerate every missing BSD syscall in ONE run, returns ENOSYS -- a
table-building probe, NEVER for a real run), `OCERZ_SECLOG` (_dyld_lookup_section_info kind ->
section), `OCERZ_OBJCCB` (the objc callback struct), `OCERZ_CATPROBE` (objc's
didInitialAttachCategories / didCallDyldNotifyRegister gate), `OCERZ_VECPROBE`.

### Refuted this session (do not re-attempt)
* "Inbound OOL relocate skips MACH64_MSG_VECTOR is the wall" -- **measured zero vector receives**.
* "Tagged pointers are mis-decoded" -- tagged class name matches native exactly
  (`NSConstantIntegerNumber`); it was categories.
* "The executable isn't handed to objc" -- it IS (`loading image for .../Platypus (has class
  properties)`); compute_closure seeds with main_mh.
* "(B) category on a local class works, so catlist is read" -- FALSE POSITIVE: clang merged that
  category into the class at compile time (`__objc_catlist` had only ONE entry). No category
  attached at all.

## UPDATE #30 -- libdispatch timers: two bugs fixed, handler dispatch still open

Phase 5 (`CFRunLoopRunInMode(mode, 1.0, false)` never returns) turned out NOT to be an
mk_timer problem. `mk_timer_create`/`arm` + an untimed `mach_msg` receive works under ocerz
(210ms vs Rosetta's 206ms; probe in the scratchpad). CF delivers the `seconds` timeout of
`CFRunLoopRunInMode` through a **libdispatch timer source**, whose handler calls
`CFRunLoopWakeUp`. So the CFRunLoop hang, the winemac.drv 5s `macdrv_start_cocoa_app` timeout
(#16/#17) and Mousecape's half-of-launches hang are all the same wall: libdispatch.

### The repro (fast, deterministic, no GUI)
A ~50-line dynamically-linked probe exercising each libdispatch primitive separately, one per
process. Under Rosetta all pass. Under ocerz:

| | default | `OCERZ_HOSTWQ=1` |
| --- | --- | --- |
| `dispatch_async` global queue | ok | ok |
| `dispatch_async` x3 | ok | ok |
| `dispatch_after` | hangs | hangs |
| TIMER source | hangs | hangs |
| READ source | hangs | ok |
| serial queue async | hangs | ok |

Without `OCERZ_HOSTWQ`, `sys_kevent_qos` is a no-op that returns success, so every event
source silently dies. That alone explains read/serial in the default column.

### Bug 1: the event-manager flag was never set (fixed)
libdispatch handles timers on its **manager**. It wakes it with `EVFILT_USER ident=1
fflags=NOTE_TRIGGER`, registered at qos `0x02000000` (`_PTHREAD_PRIORITY_EVENT_MANAGER_FLAG`).
The host delivered that event correctly and `ocerz_hostwq_bridge` entered a guest worker with
it, but never set `WQ_FLAG_THREAD_EVENT_MANAGER (0x100000)` in the `_pthread_wqthread` flags,
so libdispatch saw the manager wakeup on an ordinary worker and **never programmed its timer
heap at all**. Detect it the same way libdispatch does: read the host thread's pthread_priority
out of TSD slot 4 (`ocerz_hostwq_is_manager`). Measured: EVFILT_TIMER registrations reaching
the host kernel went 0 -> 631315. Verified the guest's own libpthread then stores `0x2000000`
into guest TSD slot 4, so libdispatch does see the thread as the manager.

### Bug 2: NOTE_MACHTIME deadlines were not converted (fixed)
Once it programs a timer, libdispatch hands back `EVFILT_TIMER fflags=0x118`
(`NOTE_ABSOLUTE|NOTE_LEEWAY|NOTE_MACHTIME`). NOTE_MACHTIME means `data` is in **mach absolute
units**. The guest computes it in ns, because ocerz reports timebase 1/1 (so does Rosetta), but
the host timebase is 125/3 -- so unconverted the kernel schedules it ~41.7x further out. A
200ms timer was landing ~265 days late, i.e. the 0% CPU "never fires" symptom. `data` (+0x20)
and, when NOTE_LEEWAY is set, `ext[1]` (+0x30) now go through `ocerz_guest_ns_to_host_ticks`
on the way to the host. Measured after the fix: first arm is +197ms, and the kernel fires it
on time. Same bug class as the earlier mach trap 90/93/95 fix, just on the kevent path.

### Still open: expiry is delivered but the handler never runs
With both fixes the timer arms correctly and the kernel delivers `EVFILT_TIMER data=1` back to
the manager on time. The manager then calls `gettimeofday` once and re-arms **the identical
deadline**, now in the past, so it fires again immediately -- 631315 re-arms in 4s, 100% CPU
(it was 0% CPU before, so this is a different failure, not the old one). It never calls
`WQOPS_QUEUE_REQTHREADS`, so it never asks for a worker to run the handler. With a repeating
200ms interval timer the re-armed deadline still never advances, which means the timer heap is
never updated: libdispatch is not running timer expiry at all, it is only re-programming an
unchanged heap.

Ruled out so far: the manager flag itself (set, and visible to the guest); the manager QoS bits
(0 vs 4 makes no difference); the ddi reset at `pth+0x1c8`/`+0x1b8` (`OCERZ_NO_DDIRESET=1`
changes nothing); the mach thread port changing between deliveries (stable at one port across
543180 deliveries); `OCERZ_HOSTWQ_ASYNC`; and the guest clock (RDTSC returns real host ns and
the commpage is global, so it cannot differ per thread; a 100ms sleep measures 110ms).

### Kept diagnostics (env-gated)
`OCERZ_REARMLOG` (every kevent the guest hands back to the host: filter, flags, fflags, data,
udata), `OCERZ_ULOCKLOG` now also prints `WQ-MANAGER delivery` with the guest's TSD qos slot,
`OCERZ_NO_MGRFLAG` (suppress bug 1's fix for A/B). Note `OCERZ_KRLOG` is unusable on timer
events: it dereferences `ident` as a dispatch queue pointer, which faults for EVFILT_TIMER.
