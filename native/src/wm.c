/*
 * Window-manager hints and ICCCM property helpers.
 *
 * In real X these functions write standard properties (WM_HINTS,
 * WM_NORMAL_HINTS, WM_PROTOCOLS, WM_CLASS) onto the client window, which
 * the window manager then reads. em-x11 has no separate WM process -- the
 * internal compositor IS the WM -- but we still write the properties
 * properly so ICCCM-compliant clients (everything) see consistent state.
 */

#include "emx11_internal.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

/* -- Allocator helpers -- */

XWMHints *XAllocWMHints(void) {
    return calloc(1, sizeof(XWMHints));
}

XSizeHints *XAllocSizeHints(void) {
    return calloc(1, sizeof(XSizeHints));
}

XClassHint *XAllocClassHint(void) {
    return calloc(1, sizeof(XClassHint));
}

XIconSize *XAllocIconSize(void) {
    return calloc(1, sizeof(XIconSize));
}

XStandardColormap *XAllocStandardColormap(void) {
    return calloc(1, sizeof(XStandardColormap));
}

/* -- Hint writers -- */

int XSetWMHints(Display *dpy, Window w, XWMHints *hints) {
    if (!hints) return 0;
    return XChangeProperty(dpy, w, XA_WM_HINTS, XA_WM_HINTS, 32,
                           PropModeReplace, (unsigned char *)hints,
                           sizeof(XWMHints) / 4);
}

XWMHints *XGetWMHints(Display *dpy, Window w) {
    /* Not implemented -- clients treat NULL as "no hints set" and use
     * their built-in defaults. */
    (void)dpy; (void)w;
    return NULL;
}

void XSetWMNormalHints(Display *dpy, Window w, XSizeHints *hints) {
    if (!hints) return;
    XChangeProperty(dpy, w, XA_WM_NORMAL_HINTS, XA_WM_SIZE_HINTS, 32,
                    PropModeReplace, (unsigned char *)hints,
                    sizeof(XSizeHints) / 4);
}

Status XGetWMNormalHints(Display *dpy, Window w,
                         XSizeHints *hints_return, long *supplied_return) {
    (void)dpy; (void)w; (void)hints_return;
    if (supplied_return) *supplied_return = 0;
    return 0;
}

void XSetWMSizeHints(Display *dpy, Window w,
                     XSizeHints *hints, Atom property) {
    if (hints) {
        XChangeProperty(dpy, w, property, XA_WM_SIZE_HINTS, 32,
                        PropModeReplace, (unsigned char *)hints,
                        sizeof(XSizeHints) / 4);
    }
}

int XSetClassHint(Display *dpy, Window w, XClassHint *class_hints) {
    if (!class_hints) return 0;
    /* WM_CLASS format: "res_name\0res_class\0" packed. */
    const char *res_name  = class_hints->res_name  ? class_hints->res_name  : "";
    const char *res_class = class_hints->res_class ? class_hints->res_class : "";
    size_t n1 = strlen(res_name);
    size_t n2 = strlen(res_class);
    size_t total = n1 + 1 + n2 + 1;
    unsigned char *buf = malloc(total);
    if (!buf) return 0;
    memcpy(buf, res_name, n1 + 1);
    memcpy(buf + n1 + 1, res_class, n2 + 1);
    int ok = XChangeProperty(dpy, w, XA_WM_CLASS, XA_STRING, 8,
                             PropModeReplace, buf, (int)total);
    free(buf);
    return ok;
}

Status XGetClassHint(Display *dpy, Window w, XClassHint *class_hints_return) {
    (void)dpy; (void)w;
    if (class_hints_return) {
        class_hints_return->res_name  = NULL;
        class_hints_return->res_class = NULL;
    }
    return 0;
}

Status XSetWMProtocols(Display *dpy, Window w, Atom *protocols, int count) {
    if (!protocols || count <= 0) return 0;
    Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    return XChangeProperty(dpy, w, wm_protocols, XA_ATOM, 32,
                           PropModeReplace, (unsigned char *)protocols, count);
}

Status XGetWMProtocols(Display *dpy, Window w,
                       Atom **protocols_return, int *count_return) {
    if (count_return) *count_return = 0;
    if (protocols_return) *protocols_return = NULL;

    Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", True);
    if (wm_protocols == None) return 0;

    Atom actual_type = 0;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    int rc = XGetWindowProperty(dpy, w, wm_protocols, 0, 65535, False,
                                XA_ATOM, &actual_type, &actual_format,
                                &nitems, &bytes_after, &data);
    if (rc != Success || !data || actual_format != 32) {
        free(data);
        return 0;
    }
    if (protocols_return) *protocols_return = (Atom *)data;
    if (count_return)     *count_return     = (int)nitems;
    return 1;
}

/* -- WM_NAME helpers -- */

void XSetWMName(Display *dpy, Window w, XTextProperty *text_prop) {
    if (!text_prop || !text_prop->value) return;
    XChangeProperty(dpy, w, XA_WM_NAME, text_prop->encoding,
                    text_prop->format, PropModeReplace,
                    text_prop->value, (int)text_prop->nitems);
}

Status XGetWMName(Display *dpy, Window w, XTextProperty *text_prop_return) {
    (void)dpy; (void)w;
    if (text_prop_return) memset(text_prop_return, 0, sizeof(*text_prop_return));
    return 0;
}

void XSetWMIconName(Display *dpy, Window w, XTextProperty *text_prop) {
    if (!text_prop || !text_prop->value) return;
    XChangeProperty(dpy, w, XA_WM_ICON_NAME, text_prop->encoding,
                    text_prop->format, PropModeReplace,
                    text_prop->value, (int)text_prop->nitems);
}

Status XStringListToTextProperty(char **list, int count,
                                 XTextProperty *text_prop_return) {
    if (!text_prop_return || !list || count <= 0) return 0;
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        total += strlen(list[i]) + 1;
    }
    unsigned char *buf = malloc(total ? total : 1);
    if (!buf) return 0;
    size_t o = 0;
    for (int i = 0; i < count; i++) {
        size_t n = strlen(list[i]) + 1;
        memcpy(buf + o, list[i], n);
        o += n;
    }
    text_prop_return->value    = buf;
    text_prop_return->encoding = XA_STRING;
    text_prop_return->format   = 8;
    text_prop_return->nitems   = (unsigned long)total;
    return 1;
}

void XSetWMProperties(Display *dpy, Window w, XTextProperty *window_name,
                      XTextProperty *icon_name, char **argv, int argc,
                      XSizeHints *normal_hints, XWMHints *wm_hints,
                      XClassHint *class_hints) {
    (void)argv; (void)argc;
    if (window_name) XSetWMName(dpy, w, window_name);
    if (icon_name)   XSetWMIconName(dpy, w, icon_name);
    if (normal_hints) XSetWMNormalHints(dpy, w, normal_hints);
    if (wm_hints)     XSetWMHints(dpy, w, wm_hints);
    if (class_hints)  XSetClassHint(dpy, w, class_hints);
}

Status XSetStandardProperties(Display *dpy, Window w,
                              _Xconst char *window_name,
                              _Xconst char *icon_name,
                              Pixmap icon_pixmap, char **argv, int argc,
                              XSizeHints *hints) {
    (void)icon_pixmap; (void)argv; (void)argc;
    if (window_name) XStoreName(dpy, w, window_name);
    if (icon_name) {
        XChangeProperty(dpy, w, XA_WM_ICON_NAME, XA_STRING, 8,
                        PropModeReplace,
                        (const unsigned char *)icon_name,
                        (int)strlen(icon_name));
    }
    if (hints) XSetWMNormalHints(dpy, w, hints);
    return 1;
}
