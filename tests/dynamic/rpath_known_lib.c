/* libSystem exports getpagesize too: a bind that does not find this image for
   the @rpath name its user links it by gets libSystem's (rpath_known.c). */
int getpagesize(void)
{
    return 42;
}
