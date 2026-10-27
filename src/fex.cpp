/*
 * The FEXCore engine behind -fex (include/ocerz/fex.h).
 *
 * One FEXCore context serves the process.  FEXCore keeps the guest state while
 * guest code runs, and it is copied into the OcerzCPU and back around every
 * system call and every call out to ocerz, so ocerz's own layers see the
 * registers exactly as they do under ocerz's JIT.
 *
 * FEXCore needs guest addresses to be host addresses, which is what identity
 * memory mode gives (include/ocerz/mem.h), so -fex runs only there.
 *
 * FEXCore's options are set as FEX's own frontends set them from the
 * environment: FEX_<OPTION>=value for any option in its table, FEX_TSOENABLED=0
 * or FEX_MULTIBLOCK=0 for instance.
 *
 * ---- threads and nesting ----
 * Guest code runs nested on one host thread: a system call or a dyld API call
 * made from guest code can run guest code again through ocerz_vm_call (an
 * initializer, an image callback), which can call out again, and so on.  Each
 * level is its own FEXCore thread, kept per host thread in levels() and made the
 * first time that depth is reached, then handed whichever cpu runs at that
 * depth.  t_cur is the level whose code is running, which every callback from
 * FEXCore works on.  FEXCore checks a thread's code segment for long mode, so
 * each level has a descriptor table holding macOS's 64-bit user code selector,
 * 0x2b, and FEXCore's calls and returns keep a stack of their own, which the
 * frontend provides: 4 MB between two guard pages, starting a quarter of the way
 * in, as FEX's Linux frontend sets it.
 *
 * ---- leaving FEXCore ----
 * A system call runs inside FEXCore's callback and guest code goes on from where
 * it says.  When the call ends the guest's run (an exit, a thread's end, a
 * pending signal), the callback points the guest at fex_exit_hlt instead.  The
 * context runs with fault-free synchronous faults, so a HLT, an int3 or a ud2
 * reaches fex_sync_fault with the guest state spilled, and that jumps back to
 * the level's ocerz_fex_run, which hands the run loop its step code.  An int3
 * becomes the guest's SIGTRAP; any other trap but our own HLT is fatal, as it
 * is under ocerz's interpreter.  FEXCore raises no divide error: arm64's divide
 * answers 0 for a zero divisor.
 *
 * A thread looping in translated code, with no system call to notice anything
 * at, is stopped through its interrupt page: every compiled block FEXCore emits
 * on macOS stores to that page first and records its guest rip
 * (tools/fex-macos.patch), so protecting the page stops the thread at a block's
 * start, the one place its registers and rip are known to agree.  FEXCore can
 * name only a block's start for a pc inside it, so stopping it anywhere else
 * would run the block's first instructions twice.
 *
 * ocerz answers some addresses itself that hold no code a host could map: the
 * dyld API's trap slots and the sentinel a call from ocerz returns to, both
 * below 4 GB, where macOS gives an arm64 process no memory; native mode's bridge
 * to arm64 frameworks is one more such slot.  Each is a FEXCore
 * thunk trampoline, which leaves the address in R11 and goes on at code ocerz
 * holds in its own memory, since FEXCore reads guest code through host
 * pointers: the sentinel goes to fex_exit_hlt, and a trap slot to fex_trap_insn,
 * FEXCore's thunk instruction, which calls fex_trap with the guest registers
 * spilled.  fex_trap runs ocerz_dyldapi_dispatch; that returns to the caller
 * itself, and the thunk instruction pops a return address of its own, so the
 * address is pushed back for it.  A HLT anywhere else is the guest's own, which
 * on x86 is a fault.
 *
 * ---- guest code that changes ----
 * Every page FEXCore translates from is armed like a page ocerz's JIT translated
 * from (ocerz_mem_arm_exec, ocerz_cache_arm_exec), so the guest's first write
 * to it faults; the fault disarms the page, drops FEXCore's translations of it
 * and lets the write go again.  ocerz's other reasons to drop translations reach
 * FEXCore through ocerz_engine_invalidate.  Dropping a range drops it from the
 * shared code buffers and from every FEXCore thread's own lookup cache, so every
 * level of every host thread is listed in g_all.
 *
 * ---- fork ----
 * A forked child cannot execute its parent's MAP_JIT memory, which holds all of
 * FEXCore's code, its dispatcher included.  So when the system call that comes
 * back in a new process is a fork, the callback does not return into that code:
 * the child makes a context of its own, forgets the parent's threads, and jumps
 * back to the level's ocerz_fex_run with the guest state in the cpu.  A native
 * fork comes back through the bridge, the same way, and an outer level of the
 * parent's that a callback returns to leaves for its run loop as well.
 *
 * ---- faults ----
 * FEXCore's ordered loads and stores fault when misaligned across 16 bytes, and
 * FEXCore rewrites such an instruction in place once handed the fault.  The x86
 * commpage, at 0x7fffffe00000, lies above the highest address macOS maps for an
 * arm64 process, so a guest read of it faults too: the load is decoded, done
 * from ocerz's copy of the commpage, and stepped over.  Any other fault in
 * translated code is the guest's: the registers FEXCore keeps in host registers
 * are spilled from the host context by the mapping FEXCore gave its signal
 * delegator, the guest rip is found from the host pc, and crash_handler goes on
 * to deliver the signal as it does for ocerz's own code, jumping back to the run
 * loop past this level's ocerz_fex_run.  So each level notes its frame, and a
 * level whose frame is gone by the next entry is dropped.
 */
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/Thunks.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/IR/IR.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <vector>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>

extern "C" {
#include "ocerz/cache.h"
#include "ocerz/cpu.h"
#include "ocerz/dyldapi.h"
#include "ocerz/fex.h"
#include "ocerz/flags.h"
#include "ocerz/interp.h"
#include "ocerz/mem.h"
#include "ocerz/syscall.h"
#include "ocerz/vm.h"
}

namespace {

using FEXCore::X86State::REG_R11;

constexpr uint16_t MACOS_USER_CS = 0x2b;
constexpr uint16_t MACOS_USER_SS = 0x23;
constexpr size_t CALLRET_GUARD = 16384;
constexpr size_t CALLRET_SIZE = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;
constexpr int MAX_LEVELS = 128;

const uint8_t fex_exit_hlt = 0xf4;

struct TrapInsn {
    uint8_t op[2];
    uint8_t name[32];
};
const TrapInsn fex_trap_insn = {{0x0f, 0x3f}, {'o', 'c', 'e', 'r', 'z', ' ', 'd', 'y', 'l', 'd', ' ', 'a', 'p', 'i'}};
const TrapInsn fex_bridge_insn = {{0x0f, 0x3f}, {'o', 'c', 'e', 'r', 'z', ' ', 'b', 'r', 'i', 'd', 'g', 'e'}};

struct FexThread {
    uint64_t generation;
    OcerzVM *vm;
    OcerzCPU *cpu;
    FEXCore::Core::InternalThreadState *thread;
    FEXCore::Core::CPUState::gdt_segment gdt[32];
    uint8_t *callret;
    jmp_buf out;
    uintptr_t frame;
    bool leave;
    bool cpu_current;
    uint64_t resume_rip;
    int step;
};

/* Never destroyed: a host thread's levels are taken apart when it ends, which at exit can be after static
 * destructors have run. */
FEXCore::Context::Context *g_ctx;
std::once_flag g_init_once;
uint64_t g_sentinel;
pid_t g_pid;
uint64_t g_generation;

std::mutex g_all_lock;
std::vector<FexThread *> g_all;

void destroy_level(FexThread *ft)
{
    {
        std::scoped_lock lk(g_all_lock);
        std::erase(g_all, ft);
    }
    g_ctx->DestroyThread(ft->thread);
    munmap(ft->callret, CALLRET_SIZE + 2 * CALLRET_GUARD);
    delete ft;
}

/* A host thread's levels live on the heap and are taken apart by a pthread key's destructor, which runs when the
 * thread ends but not at exit(): exit() runs C++ thread_local destructors first, and atexit handlers and C++
 * destructors still call guest code after them (native mode's exit is the host's). */
struct Levels {
    FexThread *at[MAX_LEVELS];
    int active;
};

pthread_key_t g_levels_key;
thread_local Levels *t_lv;
thread_local FexThread *t_cur;

void levels_gone(void *p)
{
    auto *lv = (Levels *)p;
    for (int i = 0; i < MAX_LEVELS; i++)
        if (lv->at[i] && lv->at[i]->generation == g_generation)
            destroy_level(lv->at[i]);
    delete lv;
}

Levels &levels()
{
    if (!t_lv) {
        t_lv = new Levels {};
        pthread_setspecific(g_levels_key, t_lv);
    }
    return *t_lv;
}

void to_cpu(FexThread *ft)
{
    auto &S = ft->thread->CurrentFrame->State;
    OcerzCPU *cpu = ft->cpu;
    memcpy(cpu->gpr, S.gregs, sizeof cpu->gpr);
    cpu->rip = S.rip;
    cpu->rflags = g_ctx->ReconstructCompactedEFLAGS(ft->thread, false, nullptr, 0);
    cpu->cc_op = OCERZ_CC_NONE;
    cpu->fs_base = S.fs_cached;
    cpu->gs_base = S.gs_cached;
    cpu->mxcsr = S.mxcsr;
    cpu->fcw = S.FCW;
    __uint128_t lo[16], hi[16];
    g_ctx->ReconstructXMMRegisters(ft->thread, lo, hi);
    uint64_t upper = 0;
    for (int i = 0; i < 16; i++) {
        cpu->xmm[i].lo = (uint64_t)lo[i];
        cpu->xmm[i].hi = (uint64_t)(lo[i] >> 64);
        cpu->ymmh[i].lo = (uint64_t)hi[i];
        cpu->ymmh[i].hi = (uint64_t)(hi[i] >> 64);
        upper |= cpu->ymmh[i].lo | cpu->ymmh[i].hi;
    }
    cpu->ymmh_all_zero = upper == 0;
}

void from_cpu(FexThread *ft)
{
    auto &S = ft->thread->CurrentFrame->State;
    OcerzCPU *cpu = ft->cpu;
    if (cpu->cc_op != OCERZ_CC_NONE)
        ocerz_flags_materialize(cpu);
    memcpy(S.gregs, cpu->gpr, sizeof S.gregs);
    S.rip = cpu->rip;
    g_ctx->SetFlagsFromCompactedEFLAGS(ft->thread, (uint32_t)cpu->rflags);
    S.fs_cached = cpu->fs_base;
    S.gs_cached = cpu->gs_base;
    S.mxcsr = cpu->mxcsr;
    S.FCW = cpu->fcw;
    __uint128_t lo[16], hi[16];
    for (int i = 0; i < 16; i++) {
        lo[i] = (__uint128_t)cpu->xmm[i].hi << 64 | cpu->xmm[i].lo;
        hi[i] = (__uint128_t)cpu->ymmh[i].hi << 64 | cpu->ymmh[i].lo;
    }
    g_ctx->SetXMMRegistersFromState(ft->thread, lo, hi);
}

bool must_leave(FexThread *ft, int r)
{
    OcerzCPU *cpu = ft->cpu;
    return r != OCERZ_STEP_OK || ft->vm->exited || cpu->terminated || cpu->interrupt ||
           (__atomic_load_n(&cpu->sig_pending, __ATOMIC_RELAXED) & ~cpu->sig_mask) || ocerz_peek_pending_async_sig();
}

void fork_child_restart();

/* After a call out of guest code (a system call, a bridged native call, which may have run callbacks of its own):
 * in a forked child the context is made anew, and a level of the old one does not go back into code this process
 * cannot run but jumps to its ocerz_fex_run, the guest state in the cpu, where the run loop goes on with a new one. */
void leave_if_forked(FexThread *ft, int r)
{
    if (getpid() != g_pid)
        fork_child_restart();
    if (ft->generation == g_generation)
        return;
    ft->leave = true;
    ft->cpu_current = true;
    ft->step = r;
    ft->resume_rip = ft->cpu->rip;
    _longjmp(ft->out, 1);
}

void fex_syscall(FEXCore::Core::CpuStateFrame *Frame)
{
    FexThread *ft = t_cur;
    OcerzCPU *cpu = ft->cpu;
    auto &S = Frame->State;
    uint64_t at = S.rip;
    to_cpu(ft);
    cpu->cur_rip = at;
    cpu->rip = at + 2;
    int r = ocerz_handle_syscall(ft->vm, cpu);
    leave_if_forked(ft, r);
    from_cpu(ft);
    if (must_leave(ft, r)) {
        ft->leave = true;
        ft->step = r;
        ft->resume_rip = cpu->rip;
        S.rip = (uint64_t)&fex_exit_hlt;
    }
}

void trap_dispatch(uint64_t slot)
{
    FexThread *ft = t_cur;
    OcerzCPU *cpu = ft->cpu;
    to_cpu(ft);
    cpu->cur_rip = slot;
    cpu->rip = slot;
    int r = ocerz_dyldapi_dispatch(ft->vm, cpu);
    leave_if_forked(ft, r);
    uint64_t back = cpu->rip;
    if (must_leave(ft, r)) {
        ft->leave = true;
        ft->step = r;
        ft->resume_rip = back;
        back = (uint64_t)&fex_exit_hlt;
    }
    cpu->gpr[OCERZ_RSP] -= 8;
    *(uint64_t *)cpu->gpr[OCERZ_RSP] = back;
    from_cpu(ft);
}

void fex_trap(void *)
{
    trap_dispatch(t_cur->thread->CurrentFrame->State.gregs[REG_R11]);
}

/* A synthesized native-mode stub leaves its export's id in R11, so its trampoline is a plain one. */
void fex_bridge(void *)
{
    trap_dispatch(OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF);
}

const FEXCore::SignalDelegatorConfig &g_signals_config();

[[noreturn]] void fex_sync_fault(FEXCore::Context::Context *, FEXCore::Core::CpuStateFrame *Frame)
{
    FexThread *ft = t_cur;
    OcerzCPU *cpu = ft->cpu;
    const auto fault = Frame->SynchronousFaultData;
    auto &S = Frame->State;
    if (S.rip == (uint64_t)&fex_exit_hlt) {
        if (!ft->leave) {
            to_cpu(ft);
            ft->leave = true;
            ft->resume_rip = S.gregs[REG_R11];
            ft->step = S.gregs[REG_R11] == g_sentinel ? OCERZ_STEP_OK : OCERZ_STEP_FATAL;
            from_cpu(ft);
        }
        _longjmp(ft->out, 1);
    }
    to_cpu(ft);
    cpu->cur_rip = cpu->rip;
    int delivered = 0;
    if (fault.TrapNo == FEXCore::X86State::X86_TRAPNO_BP || fault.TrapNo == FEXCore::X86State::X86_TRAPNO_DB)
        delivered = ocerz_signal_deliver(cpu, OCERZ_SIGTRAP, cpu->rip, 0, 0);
    if (!delivered)
        fprintf(stderr, "ocerz: guest fault: x86 trap %u, signal %u, at rip %#llx\n", (unsigned)fault.TrapNo,
                (unsigned)fault.Signal, (unsigned long long)cpu->rip);
    ft->leave = true;
    ft->step = delivered ? OCERZ_STEP_OK : OCERZ_STEP_FATAL;
    ft->resume_rip = cpu->rip;
    from_cpu(ft);
    _longjmp(ft->out, 1);
}

/* The guest registers FEXCore held in host registers when translated code faulted, put in its state. */
void spill_host_regs(FexThread *ft, ucontext_t *uc, mcontext_t mc = nullptr)
{
    if (!mc)
        mc = uc->uc_mcontext;
    auto &S = ft->thread->CurrentFrame->State;
    const auto &cfg = g_signals_config();
    S.rip = g_ctx->RestoreRIPFromHostPC(ft->thread, mc->__ss.__pc);
    for (size_t i = 0; i < cfg.SRAGPRCount; i++) {
        const unsigned r = cfg.SRAGPRMapping[i];
        S.gregs[i] = r == 31 ? mc->__ss.__sp : r == 30 ? mc->__ss.__lr : r == 29 ? mc->__ss.__fp : mc->__ss.__x[r];
    }
    for (size_t i = 0; i < cfg.SRAFPRCount; i++)
        memcpy(&S.xmm.sse.data[i][0], &mc->__ns.__v[cfg.SRAFPRMapping[i]], sizeof(__uint128_t));
    g_ctx->SetFlagsFromCompactedEFLAGS(
        ft->thread, g_ctx->ReconstructCompactedEFLAGS(ft->thread, true, (const uint64_t *)&mc->__ss.__x[0], mc->__ss.__cpsr));
}

class FexSyscalls final : public FEXCore::HLE::SyscallHandler {
public:
    void HandleSyscall(FEXCore::Core::CpuStateFrame *Frame) override
    {
        fex_syscall(Frame);
    }
    FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState *, uint64_t) override
    {
        return {0, UINT64_MAX, true};
    }
    std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState *,
                                                                                   uint64_t) override
    {
        return std::nullopt;
    }
    void MarkGuestExecutableRange(FEXCore::Core::InternalThreadState *, uint64_t start, uint64_t length) override
    {
        ocerz_cache_arm_exec(start, start + length);
        ocerz_mem_arm_exec(start, start + length);
    }
};

class FexThunks final : public FEXCore::ThunkHandler {
public:
    FEXCore::ThunkedFunction *LookupThunk(const FEXCore::IR::SHA256Sum &sum) override
    {
        if (memcmp(&sum, fex_trap_insn.name, sizeof fex_trap_insn.name) == 0)
            return fex_trap;
        if (memcmp(&sum, fex_bridge_insn.name, sizeof fex_bridge_insn.name) == 0)
            return fex_bridge;
        return nullptr;
    }
};

class FexSignals final : public FEXCore::SignalDelegator {};

FexSyscalls g_syscalls;
FexThunks g_thunks;
FexSignals g_signals;

const FEXCore::SignalDelegatorConfig &g_signals_config()
{
    return g_signals.GetConfig();
}

bool feat(const char *name)
{
    int v = 0;
    size_t n = sizeof v;
    return sysctlbyname(name, &v, &n, nullptr, 0) == 0 && v;
}

FEXCore::HostFeatures host_features()
{
    FEXCore::HostFeatures hf {};
    hf.DCacheLineLog2 = 4;
    hf.SupportsCacheMaintenanceOps = true;
    hf.SupportsAES = feat("hw.optional.arm.FEAT_AES");
    hf.SupportsCRC = feat("hw.optional.arm.FEAT_CRC32");
    hf.SupportsSHA = feat("hw.optional.arm.FEAT_SHA1") && feat("hw.optional.arm.FEAT_SHA256");
    hf.SupportsAtomics = feat("hw.optional.arm.FEAT_LSE");
    hf.SupportsAFP = feat("hw.optional.arm.FEAT_AFP");
    hf.SupportsRCPC = feat("hw.optional.arm.FEAT_LRCPC");
    hf.SupportsTSOImm9 = feat("hw.optional.arm.FEAT_LRCPC2");
    hf.SupportsPMULL_128Bit = feat("hw.optional.arm.FEAT_PMULL");
    hf.SupportsCSSC = feat("hw.optional.arm.FEAT_CSSC");
    hf.SupportsFCMA = feat("hw.optional.arm.FEAT_FCMA");
    hf.SupportsFlagM = feat("hw.optional.arm.FEAT_FlagM");
    hf.SupportsFlagM2 = feat("hw.optional.arm.FEAT_FlagM2");
    hf.SupportsFRINTTS = feat("hw.optional.arm.FEAT_FRINTTS");
    hf.SupportsRPRES = feat("hw.optional.arm.FEAT_RPRES");
    hf.SupportsECV = feat("hw.optional.arm.FEAT_ECV");
    hf.SupportsWFXT = feat("hw.optional.arm.FEAT_WFxT");
    hf.SupportsI8MM = feat("hw.optional.arm.FEAT_I8MM");
    hf.SupportsDotProd = feat("hw.optional.arm.FEAT_DotProd");
    hf.SupportsBF16 = feat("hw.optional.arm.FEAT_BF16");
    hf.SupportsCLZERO = true;
    hf.Supports3DNow = true;
    hf.SupportsAVX = true;
    hf.SupportsAES256 = hf.SupportsAES;
    hf.SupportsPreserveAllABI = true;
    hf.ProcessFeatures.HostType = FEXCore::HostFeatures::HostTypeEnum::Linux;
    hf.ProcessFeatures.Is64BitMode = 1;
    hf.ProcessFeatures.SupportsFaultFreeSynchronousFaults = 1;
    return hf;
}

void new_context();

void fex_init()
{
    LogMan::Throw::InstallHandler([](const char *m) { fprintf(stderr, "ocerz: FEXCore: %s\n", m); });
    LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char *m) {
        if (level <= LogMan::ERROR || getenv("OCERZ_FEX_LOG"))
            fprintf(stderr, "ocerz: FEXCore: %s\n", m);
    });
    pthread_key_create(&g_levels_key, levels_gone);
    FEXCore::Config::Initialize();
    /* Apple's processors have no FEAT_RNG, and macOS programs use rdrand where CPUID offers it. */
    FEXCore::Config::Set(FEXCore::Config::CONFIG_SOFTWARERNG, "1");
    static const std::pair<const char *, FEXCore::Config::ConfigOption> env_options[] = {
#define OPT_BASE(type, group, enum, json, default) {"FEX_" #enum, FEXCore::Config::ConfigOption::CONFIG_##enum},
#include <FEXCore/Config/ConfigValues.inl>
    };
    for (const auto &[name, option] : env_options)
        if (const char *v = getenv(name))
            FEXCore::Config::Set(option, v);
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_INTERPRETER_INSTALLED, "0");
    new_context();
}

void new_context()
{
    g_pid = getpid();
    g_generation++;
    g_ctx = FEXCore::Context::Context::CreateNewContext(host_features()).release();
    g_ctx->SetSignalDelegator(&g_signals);
    g_ctx->SetSyscallHandler(&g_syscalls);
    g_ctx->SetThunkHandler(&g_thunks);
    g_ctx->EnableExitOnHLT();
    if (!g_ctx->InitCore()) {
        fprintf(stderr, "ocerz: fatal: FEXCore did not start\n");
        exit(70);
    }
    g_sentinel = ocerz_vm_call_sentinel();
    g_ctx->AddThunkTrampolineIRHandler(g_sentinel, (uintptr_t)&fex_exit_hlt);
    for (uint64_t off = 0; off <= 0x2000; off += 8)
        g_ctx->AddThunkTrampolineIRHandler(OCERZ_DYLDAPI_LO + off, (uintptr_t)&fex_trap_insn);
    g_ctx->AddPlainTrampolineIRHandler(OCERZ_DYLDAPI_LO + OCERZ_BRIDGE_OFF, (uintptr_t)&fex_bridge_insn);
}

/* In a forked child: the parent's context and threads stay where they are, unused, since their code cannot run here
 * and taking them apart would run their destructors against it. */
void fork_child_restart()
{
    g_ctx = nullptr;
    g_all.clear();
    new (&g_all_lock) std::mutex();
    Levels &lv = levels();
    for (int i = 0; i < MAX_LEVELS; i++)
        lv.at[i] = nullptr;
    lv.active = 0;
    new_context();
}

FexThread *new_level()
{
    auto *ft = new FexThread {};
    ft->generation = g_generation;
    ft->thread = g_ctx->CreateThread();
    auto &S = ft->thread->CurrentFrame->State;
    S.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = ft->gdt;
    S.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = ft->gdt;
    S.cs_idx = MACOS_USER_CS;
    S.ss_idx = MACOS_USER_SS;
    auto *cs = FEXCore::Core::CPUState::GetSegmentFromIndex(S, S.cs_idx);
    FEXCore::Core::CPUState::SetGDTLimit(cs, 0xF'FFFFU);
    cs->L = 1;
    ft->callret = (uint8_t *)mmap(nullptr, CALLRET_SIZE + 2 * CALLRET_GUARD, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (ft->callret == MAP_FAILED) {
        fprintf(stderr, "ocerz: fatal: no memory for FEXCore's call stack\n");
        exit(70);
    }
    ft->thread->CallRetStackBase = ft->callret + CALLRET_GUARD;
    mprotect(ft->thread->CallRetStackBase, CALLRET_SIZE, PROT_READ | PROT_WRITE);
    ft->thread->CurrentFrame->Pointers.SynchronousExceptionHandler = (uint64_t)&fex_sync_fault;
    std::scoped_lock lk(g_all_lock);
    g_all.push_back(ft);
    return ft;
}

/* The commpage load the instruction faulted on, done from ocerz's copy; false if it is none. */
bool commpage_load(arm_thread_state64_t *ss, uint32_t insn)
{
    uint64_t *x = &ss->__x[0];
    auto reg = [&](unsigned n) { return n == 31 ? (uint64_t)ss->__sp : x[n]; };
    const unsigned size = insn >> 30, rt = insn & 31, rn = (insn >> 5) & 31;
    unsigned opc = 1;
    uint64_t addr;
    if ((insn & 0x3f000000) == 0x39000000) {
        opc = (insn >> 22) & 3;
        addr = reg(rn) + (((insn >> 10) & 0xfff) << size);
    } else if ((insn & 0x3f200c00) == 0x38000000 || (insn & 0x3f200c00) == 0x19000000) {
        opc = (insn >> 22) & 3;
        addr = reg(rn) + (uint64_t)((int64_t)((uint64_t)((insn >> 12) & 0x1ff) << 55) >> 55);
    } else if ((insn & 0x3ffffc00) == 0x38bfc000 || (insn & 0x3ffffc00) == 0x08dffc00) {
        addr = reg(rn);
    } else if ((insn & 0x3f200c00) == 0x38200800) {
        opc = (insn >> 22) & 3;
        const unsigned rm = (insn >> 16) & 31, option = (insn >> 13) & 7, s = (insn >> 12) & 1;
        uint64_t off = x[rm];
        if (option == 2)
            off = (uint32_t)off;
        else if (option == 6)
            off = (uint64_t)(int64_t)(int32_t)off;
        addr = reg(rn) + (s ? off << size : off);
    } else {
        return false;
    }
    if (opc == 0 || addr < OCERZ_COMMPAGE_LO || addr + (1u << size) > OCERZ_COMMPAGE_HI || !ocerz_commpage)
        return false;
    uint64_t v = 0;
    memcpy(&v, ocerz_commpage + (addr - OCERZ_COMMPAGE_LO), 1u << size);
    if (opc >= 2 && size < 3) {
        const unsigned bits = 8u << size;
        v = (uint64_t)((int64_t)(v << (64 - bits)) >> (64 - bits));
        if (opc == 3)
            v = (uint32_t)v;
    }
    if (rt != 31)
        x[rt] = v;
    ss->__pc += 4;
    return true;
}

} // namespace

extern "C" int ocerz_fex_run(OcerzVM *vm, OcerzCPU *cpu)
{
    std::call_once(g_init_once, fex_init);
    Levels &lv = levels();
    const uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    while (lv.active > 0 && (!lv.at[lv.active - 1] || lv.at[lv.active - 1]->frame <= here))
        lv.active--;
    t_cur = lv.active > 0 ? lv.at[lv.active - 1] : nullptr;
    const int depth = lv.active;
    if (depth >= MAX_LEVELS) {
        fprintf(stderr, "ocerz: fatal: guest calls nested more than %d deep\n", MAX_LEVELS);
        return OCERZ_STEP_FATAL;
    }
    FexThread *ft = lv.at[depth];
    if (!ft)
        ft = lv.at[depth] = new_level();
    auto &S = ft->thread->CurrentFrame->State;
    if (ft->cpu != cpu)
        S.callret_sp = (uint64_t)ft->thread->CallRetStackBase + CALLRET_SIZE / 4;
    ft->vm = vm;
    ft->cpu = cpu;
    ft->frame = here;
    ft->leave = false;
    ft->cpu_current = false;
    from_cpu(ft);

    FexThread *outer = t_cur;
    t_cur = ft;
    lv.active = depth + 1;
    if (_setjmp(ft->out) == 0)
        g_ctx->ExecuteThread(ft->thread);
    lv.active = depth;
    t_cur = outer;

    if (!ft->leave) {
        to_cpu(ft);
        fprintf(stderr, "ocerz: FEXCore left guest code at %#llx unasked\n", (unsigned long long)cpu->rip);
        return OCERZ_STEP_FATAL;
    }
    if (ft->cpu_current)
        return ft->step;
    S.rip = ft->resume_rip;
    to_cpu(ft);
    return ft->step;
}

/* A store to the protected interrupt page.  At a check in translated code the thread is at a block's start, so it
 * leaves there if it still must; anywhere else (the dispatcher stores to the page too) the store is stepped over and
 * the page stays protected for the next check. */
static int interrupt_stop(FexThread *ft, mcontext_t mc)
{
    if (!g_ctx->IsAddressInCodeBuffer(ft->thread, (uintptr_t)mc->__ss.__pc)) {
        mc->__ss.__pc += 4;
        return 1;
    }
    mprotect(ft->thread->InterruptFaultPage, sizeof ft->thread->InterruptFaultPage, PROT_READ | PROT_WRITE);
    if (ft->leave || ft->thread->CurrentFrame->InSyscallInfo != 0 || !must_leave(ft, OCERZ_STEP_OK))
        return 1;
    spill_host_regs(ft, (ucontext_t *)nullptr, mc);
    auto &S = ft->thread->CurrentFrame->State;
    ft->leave = true;
    ft->step = OCERZ_STEP_OK;
    ft->resume_rip = S.rip;
    S.rip = (uint64_t)&fex_exit_hlt;
    mc->__ss.__pc = g_signals_config().AbsoluteLoopTopAddressFillSRA;
    return 1;
}

/* In order: the call-return stack running past its guard pages, which happens when calls are left by something
 * other than their ret (a signal handler's sigreturn, a longjmp) and is answered as FEX's frontends answer it, by
 * putting the stack pointer, x25 in translated code, back to its starting place; a commpage read, a misaligned ordered access (x0..x28, fp, lr and sp lie in that order in the thread
 * state, the register array FEXCore patches), a fault in the shared cache (ocerz's own lazy mapping and write
 * tracking, in crash_handler), a write to an armed page of guest code, and otherwise the guest's own fault, which
 * crash_handler delivers from the registers spilled here. */
extern "C" int ocerz_fex_fault(int sig, siginfo_t *si, void *uc)
{
    FexThread *ft = t_cur;
    if (!ft || !uc || (sig != SIGSEGV && sig != SIGBUS))
        return 0;
    auto *mc = ((ucontext_t *)uc)->uc_mcontext;
    uintptr_t pc = (uintptr_t)mc->__ss.__pc;
    const uint64_t a = (uint64_t)(uintptr_t)si->si_addr;
    /* Only translated code reads the x86 commpage, and commpage_load decodes the instruction before it does anything,
     * so such a fault skips the code-buffer test: a thread may be running blocks of a shared code buffer FEXCore
     * started after it last looked, which IsAddressInCodeBuffer does not count as its own (Ollama.app's threads). */
    if (a >= OCERZ_COMMPAGE_LO && a < OCERZ_COMMPAGE_HI && commpage_load(&mc->__ss, *(const uint32_t *)pc))
        return 1;
    if (!g_ctx->IsAddressInCodeBuffer(ft->thread, pc) &&
        (uint64_t)(uintptr_t)si->si_addr - (uint64_t)(uintptr_t)ft->thread->InterruptFaultPage >= sizeof ft->thread->InterruptFaultPage)
        return 0;
    const uint64_t esr = mc->__es.__esr, ec = (esr >> 26) & 0x3f;
    if (a - (uint64_t)(uintptr_t)ft->thread->InterruptFaultPage < sizeof ft->thread->InterruptFaultPage)
        return interrupt_stop(ft, mc);
    if (a - (uint64_t)(uintptr_t)ft->callret < CALLRET_SIZE + 2 * CALLRET_GUARD) {
        mc->__ss.__x[25] = (uint64_t)(uintptr_t)ft->thread->CallRetStackBase + CALLRET_SIZE / 4;
        return 1;
    }
    if (sig == SIGBUS && (esr & 0x3f) == 0x21) {
        auto moved = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
            ft->thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, pc, (uint64_t *)&mc->__ss.__x[0]);
        if (!moved)
            return 0;
        mc->__ss.__pc = pc + *moved;
        return 1;
    }
    if (ocerz_cache_region((uintptr_t)a))
        return 0;
    if ((ec == 0x24 || ec == 0x25) && (esr & (1u << 6)) && ocerz_mem_exec_write_fault(a) == 1) {
        ocerz_fex_invalidate(a & ~(uint64_t)(OCERZ_HOST_PAGE_SIZE - 1), OCERZ_HOST_PAGE_SIZE);
        return 1;
    }
    spill_host_regs(ft, (ucontext_t *)uc);
    to_cpu(ft);
    ft->cpu->cur_rip = ft->cpu->rip;
    return 0;
}

/* Called from a signal handler: a thread that must leave (exit, end, a signal to take) protects its interrupt page,
 * so the next interrupt check in translated code - at every compiled block's start (tools/fex-macos.patch) - faults
 * where its registers and rip agree, and ocerz_fex_fault stops it there. */
extern "C" int ocerz_fex_async(void *uc)
{
    FexThread *ft = t_cur;
    if (!ft || ft->leave || !must_leave(ft, OCERZ_STEP_OK))
        return 0;
    return mprotect(ft->thread->InterruptFaultPage, sizeof ft->thread->InterruptFaultPage, PROT_READ) == 0;
}

extern "C" void ocerz_fex_invalidate(uint64_t addr, uint64_t len)
{
    if (!g_ctx)
        return;
    std::scoped_lock lk(g_ctx->GetCodeInvalidationMutex());
    g_ctx->InvalidateCodeBuffersCodeRange(addr, len);
    std::scoped_lock all(g_all_lock);
    for (FexThread *ft : g_all)
        g_ctx->InvalidateThreadCachedCodeRange(ft->thread, addr, len);
}

/* For a suspend from another thread: the target is stopped by the kernel, so its level's state holds still. */
extern "C" int ocerz_fex_gprs_at(OcerzCPU *cpu, const void *host_state, uint64_t out[16])
{
    const auto *hs = (const arm_thread_state64_t *)host_state;
    FexThread *ft = nullptr;
    {
        std::scoped_lock lk(g_all_lock);
        for (FexThread *t : g_all)
            if (t->cpu == cpu)
                ft = t;
    }
    if (!ft || ft->thread->CurrentFrame->InSyscallInfo != 0 ||
        !g_ctx->IsAddressInCodeBuffer(ft->thread, (uintptr_t)arm_thread_state64_get_pc(*hs)))
        return 0;
    const auto &cfg = g_signals_config();
    memcpy(out, ft->thread->CurrentFrame->State.gregs, 16 * sizeof(uint64_t));
    for (size_t i = 0; i < cfg.SRAGPRCount && i < 16; i++) {
        const unsigned r = cfg.SRAGPRMapping[i];
        out[i] = r == 31 ? arm_thread_state64_get_sp(*hs) : r == 30 ? arm_thread_state64_get_lr(*hs)
               : r == 29 ? arm_thread_state64_get_fp(*hs) : hs->__x[r];
    }
    return 1;
}

extern "C" void ocerz_fex_cpuid(uint32_t leaf, uint32_t subleaf, uint32_t out[4])
{
    std::call_once(g_init_once, fex_init);
    const auto r = g_ctx->RunCPUIDFunction(leaf, subleaf);
    out[0] = r.eax;
    out[1] = r.ebx;
    out[2] = r.ecx;
    out[3] = r.edx;
}

extern "C" int ocerz_fex_pc_in_code(const void *pc)
{
    const Levels *lv = t_lv;
    for (int i = 0; lv && i < lv->active; i++)
        if (g_ctx->IsAddressInCodeBuffer(lv->at[i]->thread, (uintptr_t)pc))
            return 1;
    return 0;
}

/* Whether this Mac can run -fex (include/ocerz/fex.h); why names what it lacks. */
extern "C" int ocerz_fex_supported(const char **why)
{
    if (__builtin_available(macOS 14.4, *)) {
    } else {
        *why = "macOS 14.4 or newer";
        return 0;
    }
    size_t pg = (size_t)getpagesize();
    void *p = mmap(nullptr, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    bool zeroes = false;
    if (p != MAP_FAILED) {
        *(volatile char *)p = 1;
        zeroes = madvise(p, pg, MADV_ZERO) == 0 && *(volatile char *)p == 0;
        munmap(p, pg);
    }
    if (!zeroes) {
        *why = "a kernel with MADV_ZERO";
        return 0;
    }
    return 1;
}
