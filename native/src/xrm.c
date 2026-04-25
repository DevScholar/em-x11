/*
 * XResourceManager (Xrm) stubs.
 *
 * Xrm is the Athena-era config system that parses resource strings such as
 *     Xterm*background: black
 * and serves them through a quark-based hash. Xt uses Xrm for widget
 * defaults, -xrm command-line options, and .Xdefaults file parsing.
 *
 * em-x11 implements just enough Xrm to let Xt link and behave as if no
 * resource database were provided. That lets widgets fall back to their
 * compiled-in defaults -- good enough for xeyes / xclock demos. A real
 * database implementation can come later if users need -xrm overrides.
 *
 * The one piece we do implement properly is the quark interning: Xt calls
 * XrmStringToQuark constantly to identify resource names, and returning a
 * distinct non-zero id per distinct string is necessary for its internal
 * hash tables to function.
 */

#include "emx11_internal.h"

#include <X11/Xresource.h>
#include <stdlib.h>
#include <string.h>

/* -- Quark table ---------------------------------------------------------- */

#define EMX11_MAX_QUARKS 4096

typedef struct {
    char *name;
} QuarkEntry;

static QuarkEntry g_quarks[EMX11_MAX_QUARKS];
static XrmQuark   g_next_quark = 1;             /* 0 reserved for NULLQUARK */

static XrmQuark quark_find(const char *s) {
    if (!s || !*s) return NULLQUARK;
    for (XrmQuark q = 1; q < g_next_quark && q < EMX11_MAX_QUARKS; q++) {
        if (strcmp(g_quarks[q].name, s) == 0) return q;
    }
    return NULLQUARK;
}

XrmQuark XrmStringToQuark(_Xconst char *s) {
    if (!s || !*s) return NULLQUARK;
    XrmQuark existing = quark_find(s);
    if (existing != NULLQUARK) return existing;
    if (g_next_quark >= EMX11_MAX_QUARKS) return NULLQUARK;
    XrmQuark q = g_next_quark++;
    g_quarks[q].name = strdup(s);
    return q;
}

XrmQuark XrmPermStringToQuark(_Xconst char *s) {
    return XrmStringToQuark(s);
}

char *XrmQuarkToString(XrmQuark quark) {
    if (quark <= 0 || quark >= g_next_quark) return NULL;
    return g_quarks[quark].name;
}

XrmQuark XrmUniqueQuark(void) {
    if (g_next_quark >= EMX11_MAX_QUARKS) return NULLQUARK;
    XrmQuark q = g_next_quark++;
    g_quarks[q].name = strdup("");
    return q;
}

/* -- Database stubs: always empty -------------------------------------- */

void XrmInitialize(void) { /* no-op */ }

XrmDatabase XrmGetStringDatabase(_Xconst char *data) { (void)data; return NULL; }
XrmDatabase XrmGetFileDatabase  (_Xconst char *file) { (void)file; return NULL; }
XrmDatabase XrmGetDatabase      (Display *dpy)       { (void)dpy;  return NULL; }

void XrmSetDatabase     (Display *dpy, XrmDatabase db)     { (void)dpy; (void)db; }
void XrmDestroyDatabase (XrmDatabase db)                   { (void)db; }
void XrmMergeDatabases  (XrmDatabase src, XrmDatabase *dst){ (void)src; (void)dst; }
void XrmPutResource     (XrmDatabase *db, _Xconst char *spec,
                         _Xconst char *type, XrmValue *value) {
    (void)db; (void)spec; (void)type; (void)value;
}
void XrmPutStringResource(XrmDatabase *db, _Xconst char *spec, _Xconst char *value) {
    (void)db; (void)spec; (void)value;
}
void XrmPutLineResource (XrmDatabase *db, _Xconst char *line) {
    (void)db; (void)line;
}
void XrmCombineDatabase (XrmDatabase src, XrmDatabase *dst, Bool override) {
    (void)src; (void)dst; (void)override;
}
Status XrmCombineFileDatabase(_Xconst char *file, XrmDatabase *dst, Bool override) {
    (void)file; (void)dst; (void)override;
    return 0;
}

Bool XrmGetResource(XrmDatabase db, _Xconst char *str_name,
                    _Xconst char *str_class, char **str_type_return,
                    XrmValue *value_return) {
    (void)db; (void)str_name; (void)str_class;
    if (str_type_return) *str_type_return = NULL;
    if (value_return) {
        value_return->size = 0;
        value_return->addr = NULL;
    }
    return False;
}

Bool XrmQGetResource(XrmDatabase db, XrmNameList quark_name,
                     XrmClassList quark_class, XrmRepresentation *type_return,
                     XrmValue *value_return) {
    (void)db; (void)quark_name; (void)quark_class;
    if (type_return)  *type_return = NULLQUARK;
    if (value_return) { value_return->size = 0; value_return->addr = NULL; }
    return False;
}

void XrmParseCommand(XrmDatabase *db, XrmOptionDescList table, int entries,
                     _Xconst char *name, int *argc_in_out, char **argv_in_out) {
    (void)db; (void)table; (void)entries; (void)name;
    (void)argc_in_out; (void)argv_in_out;
    /* Intentionally does nothing. Xt will fall back to its widget defaults. */
}

void XrmStringToQuarkList(_Xconst char *s, XrmQuarkList list) {
    if (!s || !list) { if (list) list[0] = NULLQUARK; return; }
    /* Split on '.' and '*' into a NULLQUARK-terminated array. */
    char buf[256];
    int  out = 0;
    const char *p = s;
    while (*p) {
        int len = 0;
        while (p[len] && p[len] != '.' && p[len] != '*' && len < (int)sizeof(buf) - 1) len++;
        if (len > 0) {
            memcpy(buf, p, (size_t)len);
            buf[len] = '\0';
            list[out++] = XrmStringToQuark(buf);
        }
        p += len;
        if (*p) p++;
    }
    list[out] = NULLQUARK;
}

void XrmStringToBindingQuarkList(_Xconst char *s, XrmBindingList bindings,
                                 XrmQuarkList quarks) {
    if (!s || !bindings || !quarks) {
        if (quarks) quarks[0] = NULLQUARK;
        return;
    }
    char buf[256];
    int  out = 0;
    const char *p = s;
    while (*p) {
        XrmBinding b = XrmBindTightly;
        if (*p == '*') { b = XrmBindLoosely; p++; }
        else if (*p == '.') { p++; }
        int len = 0;
        while (p[len] && p[len] != '.' && p[len] != '*' && len < (int)sizeof(buf) - 1) len++;
        if (len > 0) {
            memcpy(buf, p, (size_t)len);
            buf[len] = '\0';
            bindings[out] = b;
            quarks[out++] = XrmStringToQuark(buf);
        }
        p += len;
    }
    quarks[out] = NULLQUARK;
}
