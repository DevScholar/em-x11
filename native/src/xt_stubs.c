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
#include "keysym_table.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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
 * No cursor *art* in a browser, but we want the right CSS cursor to
 * appear when the pointer crosses into a window that XDefineCursor'd a
 * particular shape (twm sets XC_fleur on resize edges, XC_arrow on the
 * title bar, XC_X_cursor on root). The trick is encoding enough info in
 * the Cursor xid that the JS host can map it to a CSS keyword without a
 * separate registration round-trip:
 *
 *   font cursors  : 0x70000000 | shape  (shape == X11/cursorfont.h
 *                                        constant, 0..152)
 *   pixmap cursors: g_cursor_next++ counter, no shape info -- the host
 *                   falls back to 'default' for these. We don't have a
 *                   client that depends on per-pixmap cursor art yet.
 *
 * XFreeCursor is a no-op (no resources to release). */

static XID g_cursor_next = 0x20000001;
#define EMX11_FONT_CURSOR_TAG 0x70000000u

Cursor XCreateFontCursor(Display *dpy, unsigned int shape) {
    (void)dpy;
    return (Cursor)(EMX11_FONT_CURSOR_TAG | (shape & 0xFFFu));
}

int XFreeCursor(Display *dpy, Cursor cursor) {
    (void)dpy; (void)cursor;
    return 1;
}

/* -- Font sets (XIM / Motif path) -------------------------------------
 *
 * Xlib's XFontSet bundles XFontStructs that cover the charsets the
 * client's locale needs. em-x11 renders every font through canvas
 * fillText (UTF-8 by construction), so any single loaded font already
 * covers all charsets. We still parse the full comma-separated XLFD
 * list so XFontsOfFontSet enumerates what Motif's XmRenderTable code
 * expects (each tag in a RenderTable typically maps to one entry).
 *
 * struct _XOC is opaque in Xlib.h. typedef makes XOC == XFontSet, so
 * the same struct backs both XCreateFontSet and XCreateOC.
 */

#define EMX11_FONTSET_MAX_FONTS 8

struct _XOC {
    Display         *dpy;
    XFontStruct     *fonts[EMX11_FONTSET_MAX_FONTS];
    char            *names[EMX11_FONTSET_MAX_FONTS];
    int              n_fonts;
    XFontStruct    **font_struct_list;          /* XFontsOfFontSet — stable until free */
    char           **font_name_list;
    XFontSetExtents  extents;
    char            *base_name_list;
    char            *locale;
};

static char *xfs_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static void xfs_trim(char *s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
}

XFontSet XCreateFontSet(Display *dpy, _Xconst char *base_font_name_list,
                        char ***missing_charset_list_return,
                        int *missing_charset_count_return,
                        char **def_string_return) {
    if (missing_charset_list_return)  *missing_charset_list_return  = NULL;
    if (missing_charset_count_return) *missing_charset_count_return = 0;
    if (def_string_return)            *def_string_return            = (char *)"";

    const char *raw = base_font_name_list && *base_font_name_list
                      ? base_font_name_list : "fixed";

    struct _XOC *set = calloc(1, sizeof *set);
    if (!set) return NULL;
    set->dpy = dpy;
    set->base_name_list = xfs_strdup(raw);
    set->locale = xfs_strdup("C");

    char *work = xfs_strdup(raw);
    char *missing_buf[16];
    int   missing = 0;
    int   loaded  = 0;
    char *p = work;
    while (p && *p && loaded < EMX11_FONTSET_MAX_FONTS) {
        while (*p == ' ' || *p == '\t') p++;
        char *comma = strchr(p, ',');
        if (comma) *comma = '\0';
        xfs_trim(p);
        if (*p) {
            XFontStruct *fs = XLoadQueryFont(dpy, p);
            if (fs) {
                set->fonts[loaded] = fs;
                set->names[loaded] = xfs_strdup(p);
                loaded++;
            } else if (missing < 16) {
                missing_buf[missing++] = xfs_strdup(p);
            }
        }
        p = comma ? comma + 1 : NULL;
    }
    free(work);

    if (loaded == 0) {
        XFontStruct *fs = XLoadQueryFont(dpy, "fixed");
        if (!fs) {
            free(set->base_name_list);
            free(set->locale);
            for (int i = 0; i < missing; i++) free(missing_buf[i]);
            free(set);
            return NULL;
        }
        set->fonts[0] = fs;
        set->names[0] = xfs_strdup("fixed");
        loaded = 1;
    }
    set->n_fonts = loaded;

    set->font_struct_list = calloc((size_t)loaded, sizeof(XFontStruct *));
    set->font_name_list   = calloc((size_t)loaded, sizeof(char *));
    for (int i = 0; i < loaded; i++) {
        set->font_struct_list[i] = set->fonts[i];
        set->font_name_list[i]   = set->names[i];
    }

    int max_w = 0, max_asc = 0, max_dsc = 0;
    for (int i = 0; i < loaded; i++) {
        XFontStruct *fs = set->fonts[i];
        if (fs->max_bounds.width > max_w) max_w = fs->max_bounds.width;
        if (fs->ascent  > max_asc) max_asc = fs->ascent;
        if (fs->descent > max_dsc) max_dsc = fs->descent;
    }
    if (max_w   <= 0) max_w   = 8;
    if (max_asc <= 0) max_asc = 10;
    if (max_dsc <= 0) max_dsc = 2;
    set->extents.max_logical_extent.x      = 0;
    set->extents.max_logical_extent.y      = (short)(-max_asc);
    set->extents.max_logical_extent.width  = (unsigned short)max_w;
    set->extents.max_logical_extent.height = (unsigned short)(max_asc + max_dsc);
    set->extents.max_ink_extent = set->extents.max_logical_extent;

    if (missing_charset_count_return) *missing_charset_count_return = missing;
    if (missing_charset_list_return && missing > 0) {
        char **arr = calloc((size_t)missing, sizeof(char *));
        for (int i = 0; i < missing; i++) arr[i] = missing_buf[i];
        *missing_charset_list_return = arr;
    } else {
        for (int i = 0; i < missing; i++) free(missing_buf[i]);
    }
    return (XFontSet)set;
}

void XFreeFontSet(Display *dpy, XFontSet font_set) {
    struct _XOC *set = (struct _XOC *)font_set;
    if (!set) return;
    for (int i = 0; i < set->n_fonts; i++) {
        if (set->fonts[i]) XFreeFont(dpy, set->fonts[i]);
        free(set->names[i]);
    }
    free(set->font_struct_list);
    free(set->font_name_list);
    free(set->base_name_list);
    free(set->locale);
    free(set);
}

XFontSetExtents *XExtentsOfFontSet(XFontSet font_set) {
    struct _XOC *set = (struct _XOC *)font_set;
    if (set) return &set->extents;
    static XFontSetExtents empty;
    return &empty;
}

int XFontsOfFontSet(XFontSet font_set, XFontStruct ***font_struct_list_return,
                    char ***font_name_list_return) {
    struct _XOC *set = (struct _XOC *)font_set;
    if (!set) {
        if (font_struct_list_return) *font_struct_list_return = NULL;
        if (font_name_list_return)   *font_name_list_return   = NULL;
        return 0;
    }
    if (font_struct_list_return) *font_struct_list_return = set->font_struct_list;
    if (font_name_list_return)   *font_name_list_return   = set->font_name_list;
    return set->n_fonts;
}

char *XBaseFontNameListOfFontSet(XFontSet font_set) {
    struct _XOC *set = (struct _XOC *)font_set;
    return set ? set->base_name_list : (char *)"";
}

char *XLocaleOfFontSet(XFontSet font_set) {
    struct _XOC *set = (struct _XOC *)font_set;
    return set && set->locale ? set->locale : (char *)"C";
}

Bool XContextDependentDrawing(XFontSet font_set) { (void)font_set; return False; }
Bool XDirectionalDependentDrawing(XFontSet font_set) { (void)font_set; return False; }
Bool XContextualDrawing(XFontSet font_set) { (void)font_set; return False; }

XFontStruct *emx11_fontset_font(XFontSet font_set) {
    struct _XOC *set = (struct _XOC *)font_set;
    return (set && set->n_fonts > 0) ? set->fonts[0] : NULL;
}

/* -- XOM / XOC --------------------------------------------------------
 *
 * Motif's XmRenderTable path opens an XOM, then creates XOCs with
 * XNBaseFontName. Xlib typedefs XOC == XFontSet, so an XOC built via
 * XCreateOC is just an XFontSet behind the scenes. We pull XNBaseFontName
 * out of the varargs and delegate to XCreateFontSet.
 */

struct _XOM {
    Display *dpy;
    char    *res_name;
    char    *res_class;
    char    *locale;
};

XOM XOpenOM(Display *dpy, struct _XrmHashBucketRec *rdb,
            _Xconst char *res_name, _Xconst char *res_class) {
    (void)rdb;
    struct _XOM *om = calloc(1, sizeof *om);
    if (!om) return NULL;
    om->dpy = dpy;
    om->res_name  = xfs_strdup(res_name);
    om->res_class = xfs_strdup(res_class);
    om->locale    = xfs_strdup("C");
    return (XOM)om;
}

Status XCloseOM(XOM om_) {
    struct _XOM *om = (struct _XOM *)om_;
    if (!om) return 0;
    free(om->res_name);
    free(om->res_class);
    free(om->locale);
    free(om);
    return 1;
}

Display *XDisplayOfOM(XOM om_) {
    struct _XOM *om = (struct _XOM *)om_;
    return om ? om->dpy : NULL;
}

char *XLocaleOfOM(XOM om_) {
    struct _XOM *om = (struct _XOM *)om_;
    return om && om->locale ? om->locale : (char *)"C";
}

char *XSetOMValues(XOM om, ...) { (void)om; return NULL; }
char *XGetOMValues(XOM om, ...) { (void)om; return NULL; }

XOC XCreateOC(XOM om_, ...) {
    struct _XOM *om = (struct _XOM *)om_;
    if (!om) return NULL;
    const char *base_name = NULL;
    XFontSet    inherit   = NULL;

    va_list ap;
    va_start(ap, om_);
    for (;;) {
        const char *attr = va_arg(ap, const char *);
        if (!attr) break;
        if (strcmp(attr, XNBaseFontName) == 0) {
            base_name = va_arg(ap, const char *);
        } else if (strcmp(attr, "fontSet") == 0) {
            inherit = va_arg(ap, XFontSet);
        } else {
            /* skip unknown value */
            (void)va_arg(ap, void *);
        }
    }
    va_end(ap);

    if (inherit) return (XOC)inherit;
    return (XOC)XCreateFontSet(om->dpy, base_name ? base_name : "fixed",
                               NULL, NULL, NULL);
}

void XDestroyOC(XOC oc) {
    /* XCreateOC returned an XFontSet; route through XFreeFontSet so all
     * the wrapped XFontStructs are released. */
    struct _XOC *set = (struct _XOC *)oc;
    if (set) XFreeFontSet(set->dpy, (XFontSet)oc);
}

XOM XOMOfOC(XOC oc) { (void)oc; return NULL; }
char *XSetOCValues(XOC oc, ...) { (void)oc; return NULL; }
char *XGetOCValues(XOC oc, ...) { (void)oc; return NULL; }

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

int XUnionRegion(Region src1, Region src2, Region dst) {
    EmxRegion *a = (EmxRegion *)src1;
    EmxRegion *b = (EmxRegion *)src2;
    EmxRegion *d = (EmxRegion *)dst;
    if (!d) return 0;
    int empty_a = (!a || a->is_empty);
    int empty_b = (!b || b->is_empty);
    if (empty_a && empty_b) {
        d->is_empty = 1;
        d->x1 = d->y1 = d->x2 = d->y2 = 0;
        return 1;
    }
    if (empty_a) {
        d->x1 = b->x1; d->y1 = b->y1;
        d->x2 = b->x2; d->y2 = b->y2;
        d->is_empty = 0;
        return 1;
    }
    if (empty_b) {
        d->x1 = a->x1; d->y1 = a->y1;
        d->x2 = a->x2; d->y2 = a->y2;
        d->is_empty = 0;
        return 1;
    }
    d->x1 = a->x1 < b->x1 ? a->x1 : b->x1;
    d->y1 = a->y1 < b->y1 ? a->y1 : b->y1;
    d->x2 = a->x2 > b->x2 ? a->x2 : b->x2;
    d->y2 = a->y2 > b->y2 ? a->y2 : b->y2;
    d->is_empty = 0;
    return 1;
}

Bool XPointInRegion(Region r, int x, int y) {
    EmxRegion *er = (EmxRegion *)r;
    if (!er || er->is_empty) return False;
    return (x >= er->x1 && x < er->x2 && y >= er->y1 && y < er->y2)
           ? True : False;
}

/* XPolygonRegion: Tk uses this for non-rectangular clip regions (e.g.
 * canvas polygon items, rounded buttons). With our bounding-rect model
 * we collapse the polygon to its AABB -- the clip becomes rectangular,
 * losing concavity but still tight to the shape's extent. That matches
 * every other region op we do. `fill_rule` is ignored. */
Region XPolygonRegion(XPoint *points, int n, int fill_rule) {
    (void)fill_rule;
    EmxRegion *r = calloc(1, sizeof(EmxRegion));
    if (!r) return NULL;
    if (!points || n <= 0) {
        r->is_empty = 1;
        return (Region)r;
    }
    int x1 = points[0].x, y1 = points[0].y;
    int x2 = x1, y2 = y1;
    for (int i = 1; i < n; i++) {
        if (points[i].x < x1) x1 = points[i].x;
        if (points[i].y < y1) y1 = points[i].y;
        if (points[i].x > x2) x2 = points[i].x;
        if (points[i].y > y2) y2 = points[i].y;
    }
    /* Xlib polygons are fill-inclusive on the top-left edge and exclusive
     * on the bottom-right; +1 so XPointInRegion on a 1x1 polygon still
     * returns True. */
    r->x1 = x1; r->y1 = y1;
    r->x2 = x2 + 1; r->y2 = y2 + 1;
    r->is_empty = 0;
    return (Region)r;
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

/* -- Keysym <-> name. The table is auto-generated from keysymdef.h
 *    (see keysym_table.c / gen_keysyms.awk). Tk's tk.tcl parses binding
 *    specs like `<Control-Key-slash>` at startup and calls
 *    XStringToKeysym on every named keysym; any NoSymbol return aborts
 *    Tk_Init with "bad event type or keysym". A hand-rolled table can't
 *    keep up with Tk's full name set, so we ship the full ~2000-entry
 *    X11 table and fall back to the single-ASCII-char passthrough only
 *    after a miss. */

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

/* -- Passive input grabs --------------------------------------------------
 *
 * Passive button grabs are load-bearing for any window manager: twm
 * installs them on every managed frame (add_window.c:1039) so click-to-
 * raise / drag-titlebar / right-click menus all work. The grab table
 * lives on the Host (TS) side; we just forward the registration through
 * a bridge. devices.ts walks the parent chain at ButtonPress time and
 * redirects the event to the deepest matching grab window.
 *
 * AnyButton == 0 and AnyModifier == 1<<15 (X.h) survive the bridge
 * verbatim; the host treats them as wildcards.
 *
 * Keyboard grabs (XGrabKey) remain stubs -- twm only uses them for
 * f.warpring / accelerator keys we don't exercise yet. Active pointer/
 * keyboard grabs (XUngrabKeyboard / XUngrabPointer) are also still
 * no-ops; click delivery is owner-events-style and we have no sync-
 * mode replay queue, so XAllowEvents is irrelevant.
 */

int XGrabButton(Display *dpy, unsigned int button, unsigned int modifiers,
                Window grab_window, Bool owner_events,
                unsigned int event_mask, int pointer_mode, int keyboard_mode,
                Window confine_to, Cursor cursor) {
    (void)dpy;
    emx11_js_grab_button(grab_window, button, modifiers,
                         owner_events ? 1 : 0, event_mask,
                         pointer_mode, keyboard_mode, confine_to, cursor);
    return 1;
}

int XUngrabButton(Display *dpy, unsigned int button, unsigned int modifiers,
                  Window grab_window) {
    (void)dpy;
    emx11_js_ungrab_button(grab_window, button, modifiers);
    return 1;
}

int XGrabKey(Display *a, int b, unsigned int c, Window d, Bool e,
             int f, int g) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g;
    return 1;
}

int XUngrabKeyboard(Display *dpy, Time t) { (void)dpy; (void)t; return 1; }
int XUngrabPointer (Display *dpy, Time t) {
    (void)dpy; (void)t;
    emx11_js_set_grab_cursor(0);
    emx11_js_ungrab_pointer();
    return 1;
}

/* -- Window manager convenience calls -------------------------------- */

int XMapRaised(Display *dpy, Window w) {
    XMapWindow(dpy, w);
    emx11_js_window_raise(w);
    return 1;
}

int XMapSubwindows(Display *dpy, Window w) {
    /* Xt uses this from RealizeWidget to map all children of a composite
     * in one shot when every child is managed+mapped_when_managed (see
     * Intrinsic.c:RealizeWidget). Real X maps in bottom-to-top stacking
     * order; we have no z-order yet, so iteration order is fine. */
    if (!dpy) return 0;
    for (int i = 0; i < EMX11_MAX_WINDOWS; i++) {
        EmxWindow *c = &dpy->windows[i];
        if (c->in_use && c->parent == w && !c->mapped) {
            XMapWindow(dpy, c->id);
        }
    }
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

/* XrmQGetSearchList / XrmQGetSearchResource are now implemented in
 * native/src/xrm.c (they walk the real database we build there). */

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
