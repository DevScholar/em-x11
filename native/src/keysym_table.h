/*
 * Keysym name <-> code table, auto-generated from X11/keysymdef.h.
 *
 * Used by XStringToKeysym / XKeysymToString in xt_stubs.c. The table
 * itself lives in keysym_table.c, which is produced by running
 * gen_keysyms.awk over keysymdef.h (done once; re-run if we upgrade
 * the X11 proto headers).
 */
#ifndef EMX11_KEYSYM_TABLE_H
#define EMX11_KEYSYM_TABLE_H

#include <X11/X.h>

struct KeysymEntry {
    const char *name;
    KeySym      keysym;
};

extern const struct KeysymEntry g_keysym_table[];

#endif
