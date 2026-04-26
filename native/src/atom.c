/*
 * Atoms.
 *
 * X interns strings into small integer identifiers (atoms) that are
 * referenced in protocol messages. In real X the server owns the atom
 * table and hands out ids on request. em-x11 runs that table at the
 * Host layer (TypeScript side) so every wasm module in the page --
 * every "X client" -- resolves the same name to the same id. That's
 * the fix for WM_PROTOCOLS / WM_DELETE_WINDOW mismatches the old
 * per-module counters used to produce.
 *
 * The predefined atoms XA_PRIMARY..XA_LAST_PREDEFINED (1..68) are
 * still answered in-process because their ids are fixed by the X
 * protocol and every module agrees on them trivially. Hot paths like
 * XA_WM_NAME (39) never round-trip to JS.
 */

#include "emx11_internal.h"

#include <X11/Xatom.h>
#include <stdlib.h>
#include <string.h>

/* Fixed-id predefined atoms (x11protocol.txt §PredefinedAtoms). Names
 * must match the X11 predefined atom set in X11/Xatom.h so
 * round-tripping is stable. Index in the array == atom id. */
static const char *const PREDEFINED_ATOMS[] = {
    NULL,                                   /* 0  = None */
    "PRIMARY",                              /* 1  */
    "SECONDARY",                            /* 2  */
    "ARC",                                  /* 3  */
    "ATOM",                                 /* 4  */
    "BITMAP",                               /* 5  */
    "CARDINAL",                             /* 6  */
    "COLORMAP",                             /* 7  */
    "CURSOR",                               /* 8  */
    "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3",
    "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7",
                                            /* 9..16 */
    "DRAWABLE",                             /* 17 */
    "FONT",                                 /* 18 */
    "INTEGER",                              /* 19 */
    "PIXMAP",                               /* 20 */
    "POINT",                                /* 21 */
    "RECTANGLE",                            /* 22 */
    "RESOURCE_MANAGER",                     /* 23 */
    "RGB_COLOR_MAP",                        /* 24 */
    "RGB_BEST_MAP",                         /* 25 */
    "RGB_BLUE_MAP",                         /* 26 */
    "RGB_DEFAULT_MAP",                      /* 27 */
    "RGB_GRAY_MAP",                         /* 28 */
    "RGB_GREEN_MAP",                        /* 29 */
    "RGB_RED_MAP",                          /* 30 */
    "STRING",                               /* 31 */
    "VISUALID",                             /* 32 */
    "WINDOW",                               /* 33 */
    "WM_COMMAND",                           /* 34 */
    "WM_HINTS",                             /* 35 */
    "WM_CLIENT_MACHINE",                    /* 36 */
    "WM_ICON_NAME",                         /* 37 */
    "WM_ICON_SIZE",                         /* 38 */
    "WM_NAME",                              /* 39 */
    "WM_NORMAL_HINTS",                      /* 40 */
    "WM_SIZE_HINTS",                        /* 41 */
    "WM_ZOOM_HINTS",                        /* 42 */
    "MIN_SPACE",                            /* 43 */
    "NORM_SPACE",                           /* 44 */
    "MAX_SPACE",                            /* 45 */
    "END_SPACE",                            /* 46 */
    "SUPERSCRIPT_X",                        /* 47 */
    "SUPERSCRIPT_Y",                        /* 48 */
    "SUBSCRIPT_X",                          /* 49 */
    "SUBSCRIPT_Y",                          /* 50 */
    "UNDERLINE_POSITION",                   /* 51 */
    "UNDERLINE_THICKNESS",                  /* 52 */
    "STRIKEOUT_ASCENT",                     /* 53 */
    "STRIKEOUT_DESCENT",                    /* 54 */
    "ITALIC_ANGLE",                         /* 55 */
    "X_HEIGHT",                             /* 56 */
    "QUAD_WIDTH",                           /* 57 */
    "WEIGHT",                               /* 58 */
    "POINT_SIZE",                           /* 59 */
    "RESOLUTION",                           /* 60 */
    "COPYRIGHT",                            /* 61 */
    "NOTICE",                               /* 62 */
    "FONT_NAME",                            /* 63 */
    "FAMILY_NAME",                          /* 64 */
    "FULL_NAME",                            /* 65 */
    "CAP_HEIGHT",                           /* 66 */
    "WM_CLASS",                             /* 67 */
    "WM_TRANSIENT_FOR",                     /* 68 */
};

#define PREDEFINED_COUNT (sizeof(PREDEFINED_ATOMS) / sizeof(PREDEFINED_ATOMS[0]))

/* Linear scan of the 68-entry table. Tk's hot predefined lookups
 * (WM_NAME, WM_PROTOCOLS isn't predefined but WM_HINTS is) bottom
 * out here without touching JS. */
static Atom find_predefined(const char *name) {
    for (Atom i = 1; i < PREDEFINED_COUNT; i++) {
        if (strcmp(PREDEFINED_ATOMS[i], name) == 0) return i;
    }
    return None;
}

Atom XInternAtom(Display *dpy, _Xconst char *name, Bool only_if_exists) {
    (void)dpy;
    if (!name) return None;
    Atom pre = find_predefined(name);
    if (pre != None) return pre;
    return emx11_js_intern_atom(name, only_if_exists);
}

Status XInternAtoms(Display *dpy, char **names, int count,
                    Bool only_if_exists, Atom *atoms_return) {
    if (!names || !atoms_return || count <= 0) return 0;
    Status ok = 1;
    for (int i = 0; i < count; i++) {
        atoms_return[i] = XInternAtom(dpy, names[i], only_if_exists);
        if (atoms_return[i] == None) ok = 0;
    }
    return ok;
}

char *XGetAtomName(Display *dpy, Atom atom) {
    (void)dpy;
    if (atom == None) return NULL;
    if (atom < PREDEFINED_COUNT) {
        /* strdup so the caller can XFree(== free) unconditionally. */
        return strdup(PREDEFINED_ATOMS[atom]);
    }
    return emx11_js_get_atom_name(atom);
}

Status XGetAtomNames(Display *dpy, Atom *atoms, int count, char **names_return) {
    if (!atoms || !names_return || count <= 0) return 0;
    Status ok = 1;
    for (int i = 0; i < count; i++) {
        names_return[i] = XGetAtomName(dpy, atoms[i]);
        if (!names_return[i]) ok = 0;
    }
    return ok;
}
