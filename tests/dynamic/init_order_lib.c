int init_dep_ready;

__attribute__((constructor)) static void dep_ctor(void)
{
    init_dep_ready = 1;
}
