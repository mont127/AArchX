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
