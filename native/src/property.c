/*
 * Window properties (XChangeProperty / XGetWindowProperty etc).
 *
 * Each window carries a linked list of (atom, type, format, data) tuples.
 * Clients attach arbitrary metadata via XChangeProperty; em-x11 only
 * consumes a handful internally (WM_NAME, WM_PROTOCOLS) but needs to
 * round-trip every property correctly so Xt/Xaw-based apps behave.
 *
 * Scope simplifications:
 *   - No property notification events generated on change.
 *   - PropModeReplace / PropModeAppend / PropModePrepend all work; no
 *     per-mode atomicity concerns in a single-threaded wasm process.
 */

#include "emx11_internal.h"

#include <stdlib.h>
#include <string.h>

static int format_bytes(int format) {
    switch (format) {
    case 8:  return 1;
    case 16: return 2;
    case 32: return 4;
    default: return 0;
    }
}

static EmxProperty *find_prop(EmxWindow *win, Atom property) {
    for (EmxProperty *p = win->properties; p; p = p->next) {
        if (p->name == property) return p;
    }
    return NULL;
}

static EmxProperty *remove_prop(EmxWindow *win, Atom property) {
    EmxProperty **pp = &win->properties;
    while (*pp) {
        if ((*pp)->name == property) {
            EmxProperty *hit = *pp;
            *pp = hit->next;
            return hit;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

static void free_prop(EmxProperty *p) {
    if (!p) return;
    free(p->data);
    free(p);
}

int XChangeProperty(Display *display, Window w, Atom property, Atom type,
                    int format, int mode,
                    _Xconst unsigned char *data, int nelements) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    int unit = format_bytes(format);
    if (unit == 0) return 0;
    size_t bytes = (size_t)unit * (size_t)nelements;

    if (mode == PropModeReplace || !find_prop(win, property)) {
        EmxProperty *old = remove_prop(win, property);
        free_prop(old);

        EmxProperty *p = calloc(1, sizeof(EmxProperty));
        if (!p) return 0;
        p->name   = property;
        p->type   = type;
        p->format = format;
        p->nitems = nelements;
        if (bytes > 0) {
            p->data = malloc(bytes);
            if (!p->data) { free(p); return 0; }
            memcpy(p->data, data, bytes);
        }
        p->next = win->properties;
        win->properties = p;
        return 1;
    }

    /* Append / prepend: concatenate bytes, keeping the recorded type/format. */
    EmxProperty *p = find_prop(win, property);
    size_t oldbytes = (size_t)format_bytes(p->format) * (size_t)p->nitems;
    size_t newbytes = oldbytes + bytes;
    unsigned char *buf = malloc(newbytes ? newbytes : 1);
    if (!buf) return 0;
    if (mode == PropModeAppend) {
        if (p->data) memcpy(buf, p->data, oldbytes);
        memcpy(buf + oldbytes, data, bytes);
    } else { /* PropModePrepend */
        memcpy(buf, data, bytes);
        if (p->data) memcpy(buf + bytes, p->data, oldbytes);
    }
    free(p->data);
    p->data   = buf;
    p->nitems = p->nitems + nelements;
    p->type   = type;
    p->format = format;
    return 1;
}

int XGetWindowProperty(Display *display, Window w, Atom property,
                       long long_offset, long long_length, Bool delete_prop,
                       Atom req_type, Atom *actual_type_return,
                       int *actual_format_return,
                       unsigned long *nitems_return,
                       unsigned long *bytes_after_return,
                       unsigned char **prop_return) {
    EmxWindow *win = emx11_window_find(display, w);
    if (actual_type_return)   *actual_type_return   = None;
    if (actual_format_return) *actual_format_return = 0;
    if (nitems_return)        *nitems_return        = 0;
    if (bytes_after_return)   *bytes_after_return   = 0;
    if (prop_return)          *prop_return          = NULL;
    if (!win) return BadWindow;

    EmxProperty *p = find_prop(win, property);
    if (!p) return Success;
    if (req_type != AnyPropertyType && req_type != p->type) {
        if (actual_type_return)   *actual_type_return   = p->type;
        if (actual_format_return) *actual_format_return = p->format;
        return Success;
    }

    int unit = format_bytes(p->format);
    /* long_offset is in 32-bit units per the Xlib spec -- but clients
     * commonly pass byte offsets with format=8. We conservatively
     * interpret it as units-of-`unit` so the common cases work. */
    long total_units = p->nitems;
    long start = long_offset < 0 ? 0 : long_offset;
    if (start > total_units) start = total_units;
    long avail = total_units - start;
    long wanted = long_length < 0 ? avail : long_length;
    if (wanted > avail) wanted = avail;

    size_t out_bytes = (size_t)unit * (size_t)wanted;
    /* Xlib allocates one extra byte and NUL-terminates for convenience. */
    unsigned char *out = malloc(out_bytes + 1);
    if (!out) return BadAlloc;
    if (out_bytes > 0) {
        memcpy(out, p->data + (size_t)unit * (size_t)start, out_bytes);
    }
    out[out_bytes] = 0;

    if (actual_type_return)   *actual_type_return   = p->type;
    if (actual_format_return) *actual_format_return = p->format;
    if (nitems_return)        *nitems_return        = (unsigned long)wanted;
    if (bytes_after_return)
        *bytes_after_return = (unsigned long)((avail - wanted) * unit);
    if (prop_return)          *prop_return          = out;
    else                      free(out);

    if (delete_prop && wanted == avail && start == 0) {
        EmxProperty *hit = remove_prop(win, property);
        free_prop(hit);
    }
    return Success;
}

int XDeleteProperty(Display *display, Window w, Atom property) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return BadWindow;
    EmxProperty *hit = remove_prop(win, property);
    free_prop(hit);
    return 1;
}

Atom *XListProperties(Display *display, Window w, int *num_prop_return) {
    if (num_prop_return) *num_prop_return = 0;
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return NULL;

    int n = 0;
    for (EmxProperty *p = win->properties; p; p = p->next) n++;
    if (n == 0) return NULL;

    Atom *list = malloc(sizeof(Atom) * (size_t)n);
    if (!list) return NULL;
    int i = 0;
    for (EmxProperty *p = win->properties; p; p = p->next) list[i++] = p->name;
    if (num_prop_return) *num_prop_return = n;
    return list;
}

/* Called from XDestroyWindow to release the property list. */
void emx11_window_free_properties(EmxWindow *win) {
    EmxProperty *p = win->properties;
    while (p) {
        EmxProperty *next = p->next;
        free_prop(p);
        p = next;
    }
    win->properties = NULL;
}
