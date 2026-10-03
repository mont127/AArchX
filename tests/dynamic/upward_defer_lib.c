/* One library of upward_defer, built once per name with -DUD_NAME. */
void ud_log(const char *name);

__attribute__((constructor)) static void ud_init(void)
{
    ud_log(UD_NAME);
}
