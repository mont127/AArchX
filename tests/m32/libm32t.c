static int base = 40;
int *m32t_ptr = &base;                       /* a data pointer: rebased when the dylib slides */
__attribute__((constructor)) static void init(void) { base += 1; }
int m32t_add(int a) { return *m32t_ptr + a; }
