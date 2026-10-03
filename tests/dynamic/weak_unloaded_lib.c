__attribute__((weak, noinline)) long weak_twin(long v)
    __asm__("__ZN4llvm15AnalysisManagerINS_6ModuleEJEE13getResultImplEPNS_11AnalysisKeyERS1_");
__attribute__((weak, noinline)) long weak_twin(long v)
{
    return v + 0x5eed;
}

long (*volatile weak_twin_ref)(long) = weak_twin;

long weak_unloaded_call(long v)
{
    return weak_twin_ref(v);
}
