/*
 * The ncurses headers sdkgen parses for /usr/lib/libncurses.5.4.dylib: curses.h
 * for the screen library and term.h for the terminfo and termcap entry points,
 * tgetent, tgetstr, tgoto, tputs and the rest, which are what a line editor
 * such as bash's readline calls.  Between them they declare every public export
 * of the library, and both compile together for x86_64 and arm64.
 *
 * Native mode synthesizes libncurses because /bin/sh is a shim that execs the
 * shell /private/var/select/sh names, bash by default, and bash links it: system
 * and popen run their command through /bin/sh, under ocerz in native mode as in
 * cache mode, so without this image a native-mode program could not run a
 * command at all.
 */
#include <curses.h>
#include <term.h>
