/* The dylibs behind dlsym_deps.c: liba links libb and upward-links libu, libb
 * links libc.  Each exports one function named after itself. */
#if defined(LIB_C)
int c_only(void) { return 3; }
#elif defined(LIB_U)
int u_only(void) { return 5; }
#elif defined(LIB_B)
int c_only(void);
int b_only(void) { return 2 + c_only(); }
#else
int b_only(void);
int u_only(void);
int a_only(void) { return b_only() + u_only(); }
#endif
