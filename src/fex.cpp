/*
 * The FEXCore engine behind -fex (include/ocerz/fex.h).
 *
 * One FEXCore context serves the process; each host thread that runs guest
 * code gets a FEXCore thread of its own the first time the run loop hands it a
 * cpu.  FEXCore keeps the guest state while guest code runs, and it is copied
 * into the OcerzCPU and back around every system call, so ocerz's system call
 * layer sees the registers exactly as it does under its own JIT.
 *
 * FEXCore needs guest addresses to be host addresses, which is what identity
 * memory mode gives (include/ocerz/mem.h), so -fex runs only there.
 *
 * A system call runs inside FEXCore's callback, on the guest thread, and
 * guest code goes on from where it says.  When the call ends the guest's run
 * (an exit, a thread's end, a pending signal), the callback points the guest at
 * fex_exit_hlt instead: FEXCore leaves ExecuteThread on a HLT, since the
 * context runs with EnableExitOnHLT, and the run loop gets the step code.  A
 * HLT anywhere else is the guest's own, which on x86 is a fault.
 *
 * FEXCore checks a thread's code segment for long mode, so each thread gets a
 * descriptor table holding macOS's 64-bit user code selector, 0x2b.  Its calls
 * and returns also keep a stack of their own, which the frontend provides: 4 MB
 * between two guard pages, starting a quarter of the way in so that returns
 * past the first call have room, as FEX's Linux frontend sets it.
 */
#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/HostFeatures.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>
#include <FEXCore/Utils/LogManager.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>

extern "C" {
#include "ocerz/cpu.h"
#include "ocerz/fex.h"
#include "ocerz/flags.h"
#include "ocerz/interp.h"
#include "ocerz/syscall.h"
#include "ocerz/vm.h"
}

namespace {

constexpr uint16_t MACOS_USER_CS = 0x2b;
constexpr size_t CALLRET_GUARD = 16384;
constexpr size_t CALLRET_SIZE = FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE;

const uint8_t fex_exit_hlt = 0xf4;

struct FexThread {
    OcerzVM *vm;
    OcerzCPU *cpu;
    FEXCore::Core::InternalThreadState *thread;
    FEXCore::Core::CPUState::gdt_segment gdt[32];
    uint8_t *callret;
    uint64_t resume_rip;
    int step;
};

thread_local FexThread *t_fex;

fextl::unique_ptr<FEXCore::Context::Context> g_ctx;
std::once_flag g_init_once;

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
    __uint128_t lo[16], hi[16];
    for (int i = 0; i < 16; i++) {
        lo[i] = (__uint128_t)cpu->xmm[i].hi << 64 | cpu->xmm[i].lo;
        hi[i] = (__uint128_t)cpu->ymmh[i].hi << 64 | cpu->ymmh[i].lo;
    }
    g_ctx->SetXMMRegistersFromState(ft->thread, lo, hi);
}

int signal_waiting(OcerzCPU *cpu)
{
    return (__atomic_load_n(&cpu->sig_pending, __ATOMIC_RELAXED) & ~cpu->sig_mask) || ocerz_peek_pending_async_sig();
}

void fex_syscall(FEXCore::Core::CpuStateFrame *Frame)
{
    FexThread *ft = t_fex;
    OcerzVM *vm = ft->vm;
    OcerzCPU *cpu = ft->cpu;
    auto &S = Frame->State;
    uint64_t at = S.rip;
    to_cpu(ft);
    cpu->cur_rip = at;
    cpu->rip = at + 2;
    int r = ocerz_handle_syscall(vm, cpu);
    from_cpu(ft);
    if (r != OCERZ_STEP_OK || vm->exited || cpu->terminated || cpu->interrupt || signal_waiting(cpu)) {
        ft->step = r;
        ft->resume_rip = cpu->rip;
        S.rip = (uint64_t)&fex_exit_hlt;
    }
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
};

class FexSignals final : public FEXCore::SignalDelegator {};

FexSyscalls g_syscalls;
FexSignals g_signals;

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
    return hf;
}

void fex_init()
{
    LogMan::Throw::InstallHandler([](const char *m) { fprintf(stderr, "ocerz: FEXCore: %s\n", m); });
    LogMan::Msg::InstallHandler([](LogMan::DebugLevels level, const char *m) {
        if (level <= LogMan::ERROR || getenv("OCERZ_FEX_LOG"))
            fprintf(stderr, "ocerz: FEXCore: %s\n", m);
    });
    FEXCore::Config::Initialize();
    FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");
    FEXCore::Config::Set(FEXCore::Config::CONFIG_INTERPRETER_INSTALLED, "0");
    g_ctx = FEXCore::Context::Context::CreateNewContext(host_features());
    g_ctx->SetSignalDelegator(&g_signals);
    g_ctx->SetSyscallHandler(&g_syscalls);
    g_ctx->EnableExitOnHLT();
    if (!g_ctx->InitCore()) {
        fprintf(stderr, "ocerz: fatal: FEXCore did not start\n");
        exit(70);
    }
}

FexThread *fex_thread(OcerzVM *vm, OcerzCPU *cpu)
{
    FexThread *ft = t_fex;
    if (ft && ft->cpu == cpu)
        return ft;
    if (ft) {
        g_ctx->DestroyThread(ft->thread);
        munmap(ft->callret, CALLRET_SIZE + 2 * CALLRET_GUARD);
        delete ft;
    }
    ft = new FexThread {};
    ft->vm = vm;
    ft->cpu = cpu;
    ft->thread = g_ctx->CreateThread();
    auto &S = ft->thread->CurrentFrame->State;
    S.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = ft->gdt;
    S.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = ft->gdt;
    S.cs_idx = MACOS_USER_CS;
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
    S.callret_sp = (uint64_t)ft->thread->CallRetStackBase + CALLRET_SIZE / 4;
    t_fex = ft;
    return ft;
}

} // namespace

extern "C" int ocerz_fex_run(OcerzVM *vm, OcerzCPU *cpu)
{
    std::call_once(g_init_once, fex_init);
    FexThread *ft = fex_thread(vm, cpu);
    from_cpu(ft);
    ft->step = OCERZ_STEP_OK;
    g_ctx->ExecuteThread(ft->thread);
    auto &S = ft->thread->CurrentFrame->State;
    if (S.rip != (uint64_t)&fex_exit_hlt) {
        to_cpu(ft);
        fprintf(stderr, "ocerz: guest HLT at %#llx\n", (unsigned long long)cpu->rip);
        return OCERZ_STEP_FATAL;
    }
    S.rip = ft->resume_rip;
    to_cpu(ft);
    return ft->step;
}

/* x0..x28, fp, lr and sp lie in that order in the thread state, which is the register array FEXCore patches. */
extern "C" int ocerz_fex_fault(int sig, siginfo_t *si, void *uc)
{
    FexThread *ft = t_fex;
    if (!ft || sig != SIGBUS || !uc)
        return 0;
    auto *mc = ((ucontext_t *)uc)->uc_mcontext;
    uintptr_t pc = (uintptr_t)mc->__ss.__pc;
    if (!g_ctx->IsAddressInCodeBuffer(ft->thread, pc))
        return 0;
    auto moved = FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(
        ft->thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, pc, (uint64_t *)&mc->__ss.__x[0]);
    if (!moved)
        return 0;
    mc->__ss.__pc = pc + *moved;
    return 1;
}
