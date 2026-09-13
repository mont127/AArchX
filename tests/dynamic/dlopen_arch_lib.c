#if defined(__x86_64__)
int arch_value(void)
{
    return 64;
}
#else
int arch_value(void)
{
    return 0xa64;
}
#endif
