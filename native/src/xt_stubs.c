/*
 * Xlib stubs needed by libXt to link.
 *
 * Xt pulls in several Xlib APIs we have not implemented yet: regions,
 * cursors, font sets, visual matching, and a handful of trivial
 * accessors. Xt mostly uses them for features we do not care about in
 * a browser (cursor art, XIM input, multi-screen selection), so the
 * implementations are minimal but honest -- they return sane "empty"
 * values so Xt's code paths do not misinterpret them.
 *
 * When a client stats to rely on real behavior (e.g. a widget draws
 * with an actual cursor), the relevant stub gets promoted here.
 */

#include "emx11_internal.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <stdlib.h>
#include <string.h>

/* -- Generic allocator -- */

int XFree(void *data) {
    free(data);
    return 1;
}

/* -- Display / Screen accessors -- */

Display *XDisplayOfScreen(Screen *screen) {
    return screen ? screen->display : NULL;
}

int XScreenNumberOfScreen(Screen *screen) {
    (void)screen;
    return 0;                                   /* single-screen browser */
}

char *XDisplayName(const char *string) {
    /* Real Xlib returns getenv("DISPLAY") when string is NULL/empty.
     * The value is only used in error messages, so a fixed literal is
     * fine for our purposes. */
    if (string && *string) return (char *)string;
    return (char *)":0";
}

int XDisplayKeycodes(Display *dpy, int *min_keycodes_return,
                     int *max_keycodes_return) {
    (void)dpy;
    if (min_keycodes_return) *min_keycodes_return = 8;
    if (max_keycodes_return) *max_keycodes_return = 255;
    return 1;
}

/* -- Regions -----------------------------------------------------------
 *
 * Xt uses regions for expose coalescing -- XCreateRegion, then
 * XUnionRectWithRegion for every expose rect, then XClipBox to get the
 * enclosing rectangle for a single redraw pass. We model a region as a
 * single bounding rectangle plus an "empty" flag; that's enough for
 * Xt's use pattern (bounding box only) even though it loses concavity.
 */

typedef struct _XRegion {
    int   x1, y1, x2, y2;
    int   is_empty;
} EmxRegion;

Region XCreateRegion(void) {
    EmxRegion *r = calloc(1, sizeof(EmxRegion));
    if (r) r->is_empty = 1;
    return (Region)r;
}

int XDestroyRegion(Region region) {
    free(region);
    return 1;
}

int XUnionRectWithRegion(XRectangle *rect, Region src, Region dst) {
    if (!rect || !dst) return 0;
    EmxRegion *d = (EmxRegion *)dst;

    /* If src is different from dst, copy src first. Xt almost always
     * passes the same region as src and dst, but honor the API. */
    if (src && src != dst) {
        *d = *(EmxRegion *)src;
    }

    int rx1 = rect->x;
    int ry1 = rect->y;
    int rx2 = rect->x + rect->width;
    int ry2 = rect->y + rect->height;

    if (d->is_empty) {
        d->x1 = rx1; d->y1 = ry1;
        d->x2 = rx2; d->y2 = ry2;
        d->is_empty = 0;
    } else {
        if (rx1 < d->x1) d->x1 = rx1;
        if (ry1 < d->y1) d->y1 = ry1;
        if (rx2 > d->x2) d->x2 = rx2;
        if (ry2 > d->y2) d->y2 = ry2;
    }
    return 1;
}

int XClipBox(Region r, XRectangle *rect_return) {
    if (!rect_return) return 0;
    EmxRegion *er = (EmxRegion *)r;
    if (!er || er->is_empty) {
        memset(rect_return, 0, sizeof(*rect_return));
        return 1;
    }
    rect_return->x      = (short)er->x1;
    rect_return->y      = (short)er->y1;
    rect_return->width  = (unsigned short)(er->x2 - er->x1);
    rect_return->height = (unsigned short)(er->y2 - er->y1);
    return 1;
}

/* -- Cursors -----------------------------------------------------------
 *
 * No cursor art in a browser; we just hand out unique ids so callers
 * can compare for equality and XFreeCursor releases nothing. */

static XID g_cursor_next = 0x20000001;

Cursor XCreateFontCursor(Display *dpy, unsigned int shape) {
    (void)dpy; (void)shape;
    return (Cursor)(g_cursor_next++);
}

int XFreeCursor(Display *dpy, Cursor cursor) {
    (void)dpy; (void)cursor;
    return 1;
}

/* -- Font sets (XIM path) ---------------------------------------------
 *
 * Xt calls XCreateFontSet when a locale is set; we always return NULL
 * and let Xt fall back to the single-font XLoadQueryFont path. */

XFontSet XCreateFontSet(Display *dpy, _Xconst char *base_font_name_list,
                        char ***missing_charset_list_return,
                        int *missing_charset_count_return,
                        char **def_string_return) {
    (void)dpy; (void)base_font_name_list;
    if (missing_charset_list_return)  *missing_charset_list_return  = NULL;
    if (missing_charset_count_return) *missing_charset_count_return = 0;
    if (def_string_return)            *def_string_return            = (char *)"";
    return NULL;
}

void XFreeFontSet(Display *dpy, XFontSet font_set) {
    (void)dpy; (void)font_set;
}

void XFreeStringList(char **list) {
    if (!list) return;
    for (char **p = list; *p; p++) free(*p);
    free(list);
}

/* -- Visual matching ---------------------------------------------------
 *
 * em-x11 only ever creates one TrueColor 32-bit visual, so any query
 * for that visual class succeeds with the display's default; anything
 * else fails. */

Status XMatchVisualInfo(Display *dpy, int screen, int depth,
                        int class_, XVisualInfo *vinfo_return) {
    if (!dpy || !vinfo_return) return 0;
    if (screen != 0) return 0;
    if (class_ != TrueColor) return 0;

    Visual *v = dpy->screens[0].root_visual;
    memset(vinfo_return, 0, sizeof(*vinfo_return));
    vinfo_return->visual        = v;
    vinfo_return->visualid      = v ? v->visualid : 0;
    vinfo_return->screen        = 0;
    vinfo_return->depth         = depth ? depth : dpy->screens[0].root_depth;
    vinfo_return->class         = TrueColor;
    vinfo_return->red_mask      = 0x00ff0000;
    vinfo_return->green_mask    = 0x0000ff00;
    vinfo_return->blue_mask     = 0x000000ff;
    vinfo_return->colormap_size = 256;
    vinfo_return->bits_per_rgb  = 8;
    return 1;
}

/* -- Region extras used by Xt expose-coalescing code paths ------------ */

Bool XEmptyRegion(Region r) {
    EmxRegion *er = (EmxRegion *)r;
    return (!er || er->is_empty) ? True : False;
}

int XIntersectRegion(Region src1, Region src2, Region dst) {
    EmxRegion *a = (EmxRegion *)src1;
    EmxRegion *b = (EmxRegion *)src2;
    EmxRegion *d = (EmxRegion *)dst;
    if (!d) return 0;
    if (!a || !b || a->is_empty || b->is_empty) {
        d->is_empty = 1;
        d->x1 = d->y1 = d->x2 = d->y2 = 0;
        return 1;
    }
    int x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    int y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    int x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    int y2 = a->y2 < b->y2 ? a->y2 : b->y2;
    if (x1 >= x2 || y1 >= y2) {
        d->is_empty = 1;
        d->x1 = d->y1 = d->x2 = d->y2 = 0;
    } else {
        d->is_empty = 0;
        d->x1 = x1; d->y1 = y1;
        d->x2 = x2; d->y2 = y2;
    }
    return 1;
}

/* -- Event helpers ----------------------------------------------------
 *
 * Xt uses XCheckIfEvent during selection transfer, XIfEvent / XPeekEvent
 * rarely, and XFilterEvent once per event to give XIMs a shot at
 * keystrokes. For a first-run smoke test, "no filter, queue unchanged"
 * is the right answer. */

Bool XFilterEvent(XEvent *event, Window w) {
    (void)event; (void)w;
    return False;                               /* no XIM plumbed */
}

Bool XCheckIfEvent(Display *dpy, XEvent *event_return,
                   Bool (*predicate)(Display *, XEvent *, XPointer),
                   XPointer arg) {
    if (!dpy || !event_return || !predicate) return False;
    unsigned int i = dpy->event_head;
    while (i != dpy->event_tail) {
        XEvent *e = &dpy->event_queue[i];
        if (predicate(dpy, e, arg)) {
            *event_return = *e;
            /* Splice out slot i by shifting later entries forward. */
            unsigned int next = (i + 1) % EMX11_EVENT_QUEUE_CAPACITY;
            while (next != dpy->event_tail) {
                dpy->event_queue[i] = dpy->event_queue[next];
                i = next;
                next = (next + 1) % EMX11_EVENT_QUEUE_CAPACITY;
            }
            dpy->event_tail =
                (dpy->event_tail + EMX11_EVENT_QUEUE_CAPACITY - 1) %
                EMX11_EVENT_QUEUE_CAPACITY;
            return True;
        }
        i = (i + 1) % EMX11_EVENT_QUEUE_CAPACITY;
    }
    return False;
}

#include <emscripten.h>

int XIfEvent(Display *dpy, XEvent *event_return,
             Bool (*predicate)(Display *, XEvent *, XPointer),
             XPointer arg) {
    while (!XCheckIfEvent(dpy, event_return, predicate, arg)) {
        emscripten_sleep(1);
    }
    return 1;
}

int XPeekEvent(Display *dpy, XEvent *event_return) {
    while (dpy->event_head == dpy->event_tail) emscripten_sleep(1);
    *event_return = dpy->event_queue[dpy->event_head];
    return 1;
}

int XPutBackEvent(Display *dpy, XEvent *event) {
    if (!dpy || !event) return 0;
    dpy->event_head = (dpy->event_head + EMX11_EVENT_QUEUE_CAPACITY - 1) %
                       EMX11_EVENT_QUEUE_CAPACITY;
    dpy->event_queue[dpy->event_head] = *event;
    return 1;
}

int XSynchronize_noop(Display *dpy) { (void)dpy; return 0; }

int (*XSynchronize(Display *dpy, Bool onoff))(Display *) {
    (void)dpy; (void)onoff;
    return XSynchronize_noop;                   /* no network to sync */
}

/* -- Keyboard helpers ------------------------------------------------- */

KeySym *XGetKeyboardMapping(Display *dpy, unsigned int first, int count,
                            int *keysyms_per_keycode_return) {
    if (keysyms_per_keycode_return) *keysyms_per_keycode_return = 1;
    if (count <= 0) return NULL;
    KeySym *out = calloc((size_t)count, sizeof(KeySym));
    if (!out) return NULL;
    for (int i = 0; i < count; i++) {
        unsigned int kc = (unsigned int)first + (unsigned int)i;
        out[i] = (kc < 256) ? dpy->keysym_table[kc] : NoSymbol;
    }
    return out;
}

XModifierKeymap *XGetModifierMapping(Display *dpy) {
    (void)dpy;
    XModifierKeymap *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->max_keypermod = 2;
    m->modifiermap = calloc(8 * m->max_keypermod, sizeof(KeyCode));
    return m;
}

int XFreeModifiermap(XModifierKeymap *modmap) {
    if (!modmap) return 0;
    free(modmap->modifiermap);
    free(modmap);
    return 1;
}

int XRefreshKeyboardMapping(XMappingEvent *event) {
    (void)event;
    return 0;
}

/* -- Keysym <-> name. Xt parses translation tables like
 *    "<Key>Return: commit()" which needs StringToKeysym. A full table
 *    is large; for now we recognise the keysyms we actually produce
 *    and fall back to NoSymbol otherwise. */

struct KeysymEntry {
    const char *name;
    KeySym      keysym;
};

static const struct KeysymEntry g_keysym_table[] = {
    {"space",       0x0020},
    {"Return",      0xff0d},
    {"Tab",         0xff09},
    {"BackSpace",   0xff08},
    {"Escape",      0xff1b},
    {"Delete",      0xffff},
    {"Left",        0xff51},
    {"Up",          0xff52},
    {"Right",       0xff53},
    {"Down",        0xff54},
    {"Shift_L",     0xffe1},
    {"Shift_R",     0xffe2},
    {"Control_L",   0xffe3},
    {"Control_R",   0xffe4},
    {"Meta_L",      0xffe7},
    {"Meta_R",      0xffe8},
    {"Alt_L",       0xffe9},
    {"Alt_R",       0xffea},
    {NULL, 0}
};

char *XKeysymToString(KeySym keysym) {
    for (const struct KeysymEntry *e = g_keysym_table; e->name; e++) {
        if (e->keysym == keysym) return (char *)e->name;
    }
    return NULL;
}

KeySym XStringToKeysym(_Xconst char *s) {
    if (!s) return NoSymbol;
    /* Single-letter ASCII passes straight through. */
    if (s[0] && !s[1]) return (KeySym)(unsigned char)s[0];
    for (const struct KeysymEntry *e = g_keysym_table; e->name; e++) {
        if (strcmp(e->name, s) == 0) return e->keysym;
    }
    return NoSymbol;
}

void XConvertCase(KeySym keysym, KeySym *lower_return, KeySym *upper_return) {
    KeySym lo = keysym, hi = keysym;
    if (keysym >= 'A' && keysym <= 'Z') lo = keysym + ('a' - 'A');
    if (keysym >= 'a' && keysym <= 'z') hi = keysym - ('a' - 'A');
    if (lower_return) *lower_return = lo;
    if (upper_return) *upper_return = hi;
}

/* -- Input grabs are no-ops in a single-focus compositor. */

int XGrabButton(Display *a, unsigned int b, unsigned int c, Window d, Bool e,
                unsigned int f, int g, int h, Window i, Cursor j) {
    (void)a; (void)b; (void)c; (void)d; (void)e;
    (void)f; (void)g; (void)h; (void)i; (void)j;
    return 1;
}

int XGrabKey(Display *a, int b, unsigned int c, Window d, Bool e,
             int f, int g) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    return 1;
}

int XUngrabKeyboard(Display *dpy, Time t) { (void)dpy; (void)t; return 1; }
int XUngrabPointer (Display *dpy, Time t) { (void)dpy; (void)t; return 1; }

/* -- Window manager convenience calls -------------------------------- */

int XMapRaised(Display *dpy, Window w) {
    /* No z-order yet; just map. */
    return XMapWindow(dpy, w);
}

int XMapSubwindows(Display *dpy, Window w) {
    (void)dpy; (void)w;
    return 1;
}

int XIconifyWindow(Display *dpy, Window w, int screen) {
    (void)dpy; (void)w; (void)screen;
    return 1;
}

int XWithdrawWindow(Display *dpy, Window w, int screen) {
    (void)screen;
    return XUnmapWindow(dpy, w);
}

int XSetTransientForHint(Display *dpy, Window w, Window prop_window) {
    return XChangeProperty(dpy, w, XA_WM_TRANSIENT_FOR, XA_WINDOW, 32,
                           PropModeReplace,
                           (unsigned char *)&prop_window, 1);
}

int XSetCommand(Display *dpy, Window w, char **argv, int argc) {
    if (!argv || argc <= 0) return 0;
    size_t total = 0;
    for (int i = 0; i < argc; i++) total += strlen(argv[i]) + 1;
    unsigned char *buf = malloc(total);
    if (!buf) return 0;
    size_t o = 0;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        memcpy(buf + o, argv[i], n);
        o += n;
    }
    int ok = XChangeProperty(dpy, w, XA_WM_COMMAND, XA_STRING, 8,
                             PropModeReplace, buf, (int)total);
    free(buf);
    return ok;
}

int XWMGeometry(Display *dpy, int screen, _Xconst char *user_geom,
                _Xconst char *def_geom, unsigned int border_width,
                XSizeHints *hints, int *x_ret, int *y_ret,
                int *w_ret, int *h_ret, int *gravity_ret) {
    (void)dpy; (void)screen; (void)user_geom; (void)def_geom;
    (void)border_width; (void)hints;
    /* Always let the widget's own default size take effect. */
    if (x_ret) *x_ret = 0;
    if (y_ret) *y_ret = 0;
    if (w_ret) *w_ret = 0;
    if (h_ret) *h_ret = 0;
    if (gravity_ret) *gravity_ret = NorthWestGravity;
    return 0;                                   /* 0 = none supplied */
}

/* -- X context manager (id-based hash) -------------------------------
 *
 * Xt uses XUniqueContext/XSaveContext/XFindContext to map Window to
 * Widget. We implement the bare minimum: a linked list keyed on
 * (xid, context). O(n) but Xt's widget count is small. */

typedef struct ContextEntry {
    XID                  xid;
    XContext             context;
    XPointer             data;
    struct ContextEntry *next;
} ContextEntry;

static ContextEntry *g_context_head = NULL;

int XSaveContext(Display *dpy, XID xid, XContext context, const char *data) {
    (void)dpy;
    for (ContextEntry *e = g_context_head; e; e = e->next) {
        if (e->xid == xid && e->context == context) {
            e->data = (XPointer)data;
            return 0;
        }
    }
    ContextEntry *e = calloc(1, sizeof(*e));
    if (!e) return XCNOMEM;
    e->xid = xid;
    e->context = context;
    e->data = (XPointer)data;
    e->next = g_context_head;
    g_context_head = e;
    return 0;
}

int XFindContext(Display *dpy, XID xid, XContext context, XPointer *data_return) {
    (void)dpy;
    for (ContextEntry *e = g_context_head; e; e = e->next) {
        if (e->xid == xid && e->context == context) {
            if (data_return) *data_return = e->data;
            return 0;
        }
    }
    if (data_return) *data_return = NULL;
    return XCNOENT;
}

int XDeleteContext(Display *dpy, XID xid, XContext context) {
    (void)dpy;
    ContextEntry **link = &g_context_head;
    for (ContextEntry *e = g_context_head; e; link = &e->next, e = e->next) {
        if (e->xid == xid && e->context == context) {
            *link = e->next;
            free(e);
            return 0;
        }
    }
    return XCNOENT;
}

/* -- Xrm extensions -------------------------------------------------- */

char *XResourceManagerString(Display *dpy) {
    (void)dpy; return NULL;                     /* no .Xdefaults */
}

char *XScreenResourceString(Screen *screen) {
    (void)screen; return NULL;
}

Bool XrmEnumerateDatabase(XrmDatabase db, XrmNameList names,
                          XrmClassList classes, int mode,
                          Bool (*proc)(XrmDatabase *, XrmBindingList,
                                       XrmQuarkList, XrmRepresentation *,
                                       XrmValue *, XPointer),
                          XPointer arg) {
    (void)db; (void)names; (void)classes; (void)mode; (void)proc; (void)arg;
    return False;
}

Bool XrmQGetSearchList(XrmDatabase db, XrmNameList names, XrmClassList classes,
                       XrmSearchList list_return, int list_size) {
    (void)db; (void)names; (void)classes; (void)list_size;
    /* Return True to signal "search completed (with empty result)". Xt
     * retries with an ever-larger buffer when this returns False, so
     * returning False here infinite-loops into OOM. */
    if (list_return) list_return[0] = NULL;
    return True;
}

Bool XrmQGetSearchResource(XrmSearchList list, XrmName name, XrmClass class_,
                           XrmRepresentation *type_return,
                           XrmValue *value_return) {
    (void)list; (void)name; (void)class_;
    if (type_return)  *type_return  = NULLQUARK;
    if (value_return) { value_return->size = 0; value_return->addr = NULL; }
    return False;
}

void XrmQPutResource(XrmDatabase *db, XrmBindingList bindings,
                     XrmQuarkList quarks, XrmRepresentation type,
                     XrmValue *value) {
    (void)db; (void)bindings; (void)quarks; (void)type; (void)value;
}

/* -- Internal-connection watches are for async display input (XIM /
 *    DBus bridges). No-op for us. */

Status XAddConnectionWatch(Display *dpy, XConnectionWatchProc proc,
                           XPointer data) {
    (void)dpy; (void)proc; (void)data;
    return 1;
}

void XProcessInternalConnection(Display *dpy, int fd) {
    (void)dpy; (void)fd;
}

/* -- XmbTextListToTextProperty: same output shape as the plain-string
 *    variant, STRING encoding, NUL-separated. */

int XmbTextListToTextProperty(Display *dpy, char **list, int count,
                              XICCEncodingStyle style,
                              XTextProperty *text_prop_return) {
    (void)dpy; (void)style;
    return XStringListToTextProperty(list, count, text_prop_return) ? 0 : -1;
}
