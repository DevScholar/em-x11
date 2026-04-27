/*
 * Xlib/Xt stubs needed by libXaw and by in-tree demos (currently xeyes).
 *
 * Split out from xt_stubs.c so the dependency chain is obvious: libXt
 * needs what's in xt_stubs.c; libXaw and the demos need everything here
 * *plus* what xt_stubs provides. Many of these are "link-time presence,
 * runtime noop" -- Xaw's Label widget references XCopyPlane in its
 * bitmap rendering path, xeyes references XIQueryVersion for XInput2
 * detection, but neither actually reaches a meaningful runtime code
 * path in em-x11 today. The stubs keep the linker happy without
 * pretending to implement machinery we have not yet built.
 *
 * Anything that starts to matter at runtime gets promoted from here
 * to a real implementation file.
 */

#include "emx11_internal.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- 16-bit text, genuine support.
 *
 * Tk's Unix font layer classifies em-x11's XFontStruct as a two-byte
 * (iso10646-1 / ucs-2be) font, so it issues XDrawString16 / XTextWidth16
 * with XChar2b arrays in UCS-2 big-endian. We translate those back into
 * UTF-8 (combining high + low surrogate pairs so astral codepoints like
 * emoji round-trip correctly) and hand the UTF-8 buffer to the same
 * browser-side text pipeline used for 8-bit strings. The browser's
 * canvas.fillText / measureText handle arbitrary Unicode coverage,
 * which is how em-x11 gets real CJK rendering without shipping glyph
 * tables. */

static int xchar2b_to_utf8(const XChar2b *s, int n,
                           unsigned char *out, int cap) {
    int w = 0, i = 0;
    while (i < n) {
        unsigned int cp = ((unsigned int)s[i].byte1 << 8) | s[i].byte2;
        i++;
        /* UTF-16 surrogate combining: high (D800..DBFF) + low (DC00..DFFF)
         * resolve to a single codepoint >= 0x10000. x11protocol has no
         * opinion; this is Tcl's ucs-2be behaviour for supplementary
         * chars (Tcl_UtfToExternal emits the surrogate pair). */
        if (cp >= 0xD800 && cp <= 0xDBFF && i < n) {
            unsigned int lo = ((unsigned int)s[i].byte1 << 8) | s[i].byte2;
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) {
            if (w + 1 > cap) break;
            out[w++] = (unsigned char)cp;
        } else if (cp < 0x800) {
            if (w + 2 > cap) break;
            out[w++] = (unsigned char)(0xC0 | (cp >> 6));
            out[w++] = (unsigned char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            if (w + 3 > cap) break;
            out[w++] = (unsigned char)(0xE0 | (cp >> 12));
            out[w++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[w++] = (unsigned char)(0x80 | (cp & 0x3F));
        } else {
            if (w + 4 > cap) break;
            out[w++] = (unsigned char)(0xF0 | (cp >> 18));
            out[w++] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
            out[w++] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
            out[w++] = (unsigned char)(0x80 | (cp & 0x3F));
        }
    }
    return w;
}

/* Worst case: every XChar2b expands to a 4-byte UTF-8 sequence (non-BMP
 * lone surrogates). Real BMP text is 1-3 bytes; +1 for trailing NUL. */
#define EMX11_UTF8_BUFLEN(n)  ((size_t)(n) * 4 + 1)

int XDrawString16(Display *dpy, Drawable d, GC gc, int x, int y,
                  _Xconst XChar2b *string, int length) {
    if (!string || length <= 0) return 1;
    size_t cap = EMX11_UTF8_BUFLEN(length);
    unsigned char stack[512];
    unsigned char *buf = cap <= sizeof stack ? stack
                                             : (unsigned char *)malloc(cap);
    if (!buf) return 0;
    int used = xchar2b_to_utf8(string, length, buf, (int)cap - 1);
    buf[used] = 0;
    int r = XDrawString(dpy, d, gc, x, y, (const char *)buf, used);
    if (buf != stack) free(buf);
    return r;
}

int XDrawImageString16(Display *dpy, Drawable d, GC gc, int x, int y,
                       _Xconst XChar2b *string, int length) {
    if (!string || length <= 0) return 1;
    size_t cap = EMX11_UTF8_BUFLEN(length);
    unsigned char stack[512];
    unsigned char *buf = cap <= sizeof stack ? stack
                                             : (unsigned char *)malloc(cap);
    if (!buf) return 0;
    int used = xchar2b_to_utf8(string, length, buf, (int)cap - 1);
    buf[used] = 0;
    int r = XDrawImageString(dpy, d, gc, x, y, (const char *)buf, used);
    if (buf != stack) free(buf);
    return r;
}

int XTextWidth16(XFontStruct *fs, _Xconst XChar2b *string, int count) {
    if (!string || count <= 0) return 0;
    size_t cap = EMX11_UTF8_BUFLEN(count);
    unsigned char stack[512];
    unsigned char *buf = cap <= sizeof stack ? stack
                                             : (unsigned char *)malloc(cap);
    if (!buf) return 0;
    int used = xchar2b_to_utf8(string, count, buf, (int)cap - 1);
    buf[used] = 0;
    int w = XTextWidth(fs, (const char *)buf, used);
    if (buf != stack) free(buf);
    return w;
}

/* Multibyte variants with XFontSet: xt_stubs.c hands back a real
 * (non-NULL) wrapper over a single underlying XFontStruct. Pull the
 * wrapped font out via the internal accessor and route through the
 * 8-bit XDrawString / XTextWidth paths. That way Xmb callers get the
 * same glyph coverage as the rest of the toolkit. */

extern XFontStruct *emx11_fontset_font(XFontSet font_set);

void XmbDrawString(Display *dpy, Drawable d, XFontSet font_set, GC gc,
                   int x, int y, _Xconst char *text, int bytes) {
    (void)font_set;
    XDrawString(dpy, d, gc, x, y, text, bytes);
}

int XmbTextEscapement(XFontSet font_set, _Xconst char *text, int bytes) {
    XFontStruct *fs = emx11_fontset_font(font_set);
    if (fs) return XTextWidth(fs, text, bytes);
    return bytes * 7;                           /* last-ditch fallback */
}

int XmbTextExtents(XFontSet font_set, _Xconst char *text, int nbytes,
                   XRectangle *ink, XRectangle *logical) {
    XFontStruct *fs = emx11_fontset_font(font_set);
    int width   = fs ? XTextWidth(fs, text, nbytes) : nbytes * 7;
    int ascent  = fs ? fs->ascent  : 10;
    int descent = fs ? fs->descent : 2;
    if (ink) {
        ink->x      = 0;
        ink->y      = (short)-ascent;
        ink->width  = (unsigned short)width;
        ink->height = (unsigned short)(ascent + descent);
    }
    if (logical) *logical = ink ? *ink :
        (XRectangle){0, (short)-ascent, (unsigned short)width,
                     (unsigned short)(ascent + descent)};
    return width;
}

/* XFontSet accessor shims live in xt_stubs.c alongside XCreateFontSet
 * now that the opaque _XOC struct is defined there. */

/* -- GC setter stubs.
 * Our GC only tracks foreground/background/line_width/line_style/fill_style/font.
 * Every other setter is accepted and silently ignored; Xaw sets a lot of
 * these on its shadow/highlight GCs but the canvas 2D backend has no
 * hook for plane masks, stipples, or graphics-exposures. */

int XSetArcMode(Display *dpy, GC gc, int arc_mode) {
    (void)dpy; (void)gc; (void)arc_mode; return 1;
}
int XSetDashes(Display *dpy, GC gc, int dash_offset,
               _Xconst char *dash_list, int n) {
    (void)dpy; (void)gc; (void)dash_offset; (void)dash_list; (void)n; return 1;
}
int XSetFillRule(Display *dpy, GC gc, int fill_rule) {
    (void)dpy; (void)gc; (void)fill_rule; return 1;
}
int XSetGraphicsExposures(Display *dpy, GC gc, Bool graphics_exposures) {
    (void)dpy; (void)gc; (void)graphics_exposures; return 1;
}
int XSetPlaneMask(Display *dpy, GC gc, unsigned long plane_mask) {
    (void)dpy; (void)gc; (void)plane_mask; return 1;
}
int XSetStipple(Display *dpy, GC gc, Pixmap stipple) {
    (void)dpy; (void)gc; (void)stipple; return 1;
}
int XSetSubwindowMode(Display *dpy, GC gc, int subwindow_mode) {
    (void)dpy; (void)gc; (void)subwindow_mode; return 1;
}
int XSetTSOrigin(Display *dpy, GC gc, int ts_x_origin, int ts_y_origin) {
    (void)dpy; (void)gc; (void)ts_x_origin; (void)ts_y_origin; return 1;
}
int XSetTile(Display *dpy, GC gc, Pixmap tile) {
    (void)dpy; (void)gc; (void)tile; return 1;
}
int XSetRegion(Display *dpy, GC gc, Region r) {
    (void)dpy; (void)gc; (void)r; return 1;
}

/* -- Pixmap-adjacent stubs.
 * XCreatePixmap / XFreePixmap live in pixmap.c with real backing canvases;
 * XCopyArea / XCopyPlane / XPutImage live in drawing.c. The BitmapData
 * loaders below route through real XCreatePixmap so the id is valid and
 * is cleaned up by XFreePixmap, but the bitmap bits themselves are not
 * painted into the pixmap yet -- that would need XReadBitmapFileData to
 * decode the X11 bitmap file format, which nothing currently exercises. */

Pixmap XCreatePixmapFromBitmapData(Display *dpy, Drawable d, char *data,
                                   unsigned int w, unsigned int h,
                                   unsigned long fg, unsigned long bg,
                                   unsigned int depth) {
    (void)data; (void)fg; (void)bg;
    return XCreatePixmap(dpy, d, w, h, depth);
}

int XReadBitmapFileData(_Xconst char *filename, unsigned int *w,
                        unsigned int *h, unsigned char **data,
                        int *x_hot, int *y_hot) {
    (void)filename;
    if (w)     *w = 0;
    if (h)     *h = 0;
    if (data)  *data = NULL;
    if (x_hot) *x_hot = -1;
    if (y_hot) *y_hot = -1;
    return BitmapFileInvalid;
}

int XReadBitmapFile(Display *dpy, Drawable d, _Xconst char *filename,
                    unsigned int *w, unsigned int *h, Pixmap *bitmap_return,
                    int *x_hot, int *y_hot) {
    (void)dpy; (void)d;
    return XReadBitmapFileData(filename, w, h, NULL, x_hot, y_hot) == 0 ?
        BitmapSuccess : BitmapFileInvalid;
    (void)bitmap_return;
}

int XWriteBitmapFile(Display *dpy, _Xconst char *filename, Pixmap bitmap,
                     unsigned int w, unsigned int h, int x_hot, int y_hot) {
    (void)dpy; (void)filename; (void)bitmap; (void)w; (void)h;
    (void)x_hot; (void)y_hot;
    return BitmapNoMemory;
}

/* -- Window border / cursor setters are widget-visual features we
 * do not model (no borders or cursor art in-canvas). Accept and ignore. */

int XSetWindowBorder(Display *dpy, Window w, unsigned long border) {
    (void)dpy; (void)w; (void)border; return 1;
}
int XSetWindowBorderPixmap(Display *dpy, Window w, Pixmap pixmap) {
    (void)dpy; (void)w; (void)pixmap; return 1;
}
int XDefineCursor(Display *dpy, Window w, Cursor cursor) {
    (void)dpy; (void)w; (void)cursor; return 1;
}
int XUndefineCursor(Display *dpy, Window w) {
    (void)dpy; (void)w; return 1;
}

/* -- Query stubs. XQueryPointer is called by Xaw's Tip widget for
 * tooltip placement and by xeyes every 50ms for pupil tracking.
 * Read the latest canvas pointer position from the JS host; child
 * hit-testing isn't wired yet, so child_return stays None. */

Bool XQueryPointer(Display *dpy, Window w, Window *root_return,
                   Window *child_return, int *root_x_return, int *root_y_return,
                   int *win_x_return, int *win_y_return,
                   unsigned int *mask_return) {
    (void)w;
    int px = 0, py = 0;
    emx11_js_pointer_xy(&px, &py);
    if (root_return)     *root_return     = dpy->screens[0].root;
    if (child_return)    *child_return    = None;
    if (root_x_return)   *root_x_return   = px;
    if (root_y_return)   *root_y_return   = py;
    /* No per-window translation yet: hand back the root-relative pair
     * as the window-relative pair too. xeyes uses XTranslateCoordinates
     * afterward to map to its own widget, so the compounded offset
     * still works out. */
    if (win_x_return)    *win_x_return    = px;
    if (win_y_return)    *win_y_return    = py;
    if (mask_return)     *mask_return     = 0;
    return True;
}

int XGetFontProperty(XFontStruct *fs, Atom atom, unsigned long *value_return) {
    if (!fs || !fs->properties || fs->n_properties <= 0) {
        if (value_return) *value_return = 0;
        return False;
    }
    for (int i = 0; i < fs->n_properties; i++) {
        if (fs->properties[i].name == atom) {
            if (value_return) *value_return = fs->properties[i].card32;
            return True;
        }
    }
    if (value_return) *value_return = 0;
    return False;                               /* "property not present" */
}

/* -- XIM (input method) stubs.
 * No XIM support; Xaw asks for one at widget init and falls back to
 * the non-XIM code paths when XOpenIM returns NULL. The remaining
 * functions are defensively present -- Xaw checks `if (im != NULL)`
 * in most places but the linker wants every reference resolved. */

XIM XOpenIM(Display *dpy, struct _XrmHashBucketRec *rdb,
            char *res_name, char *res_class) {
    (void)dpy; (void)rdb; (void)res_name; (void)res_class;
    return NULL;
}
Status XCloseIM(XIM im) {
    (void)im; return 1;
}
Display *XDisplayOfIM(XIM im) {
    (void)im; return NULL;
}
char *XGetIMValues(XIM im, ...) {
    (void)im; return NULL;
}

XIC XCreateIC(XIM im, ...) {
    (void)im; return NULL;
}
void XDestroyIC(XIC ic) {
    (void)ic;
}
void XSetICFocus(XIC ic) {
    (void)ic;
}
void XUnsetICFocus(XIC ic) {
    (void)ic;
}
char *XGetICValues(XIC ic, ...) {
    (void)ic; return NULL;
}
char *XSetICValues(XIC ic, ...) {
    (void)ic; return NULL;
}

XVaNestedList XVaCreateNestedList(int unused_dummy, ...) {
    (void)unused_dummy;
    /* Xaw calls this to build arg lists for XCreateIC; since we
     * never have an XIM, the returned list is never dereferenced.
     * Return a unique non-NULL pointer so callers checking for
     * allocation failure do not early-out. */
    static char sentinel;
    return (XVaNestedList)&sentinel;
}

/* -- Display-level metadata. XDefault* are macros in upstream Xlib.h
 * but our header declares them as functions (see the extern decls
 * near line 1775). Provide the implementations here. */

Visual *XDefaultVisual(Display *dpy, int screen_number) {
    (void)screen_number;
    return dpy ? dpy->screens[0].root_visual : NULL;
}
Colormap XDefaultColormap(Display *dpy, int screen_number) {
    (void)screen_number;
    return dpy ? dpy->screens[0].cmap : 0;
}
int XDefaultDepth(Display *dpy, int screen_number) {
    (void)screen_number;
    return dpy ? dpy->screens[0].root_depth : 24;
}

unsigned long XNextRequest(Display *dpy) {
    (void)dpy; return 1UL;                      /* single-client; no queue */
}

int *XListDepths(Display *dpy, int screen_number, int *count_return) {
    (void)screen_number;
    int *out = malloc(sizeof(int));
    if (!out) {
        if (count_return) *count_return = 0;
        return NULL;
    }
    out[0] = dpy ? dpy->screens[0].root_depth : 24;
    if (count_return) *count_return = 1;
    return out;
}

long XMaxRequestSize(Display *dpy) {
    (void)dpy;
    /* Pick a generous value so Xt never splits a request for size
     * reasons. Real servers are typically 262140 bytes. */
    return 262140;
}

/* -- Server grabs are meaningless in a single-client world. */

int XGrabServer(Display *dpy)   { (void)dpy; return 1; }
int XUngrabServer(Display *dpy) { (void)dpy; return 1; }

/* -- Error handler. Record the hook but never invoke it (we don't
 * generate X protocol errors). */

static XErrorHandler g_error_handler = NULL;

XErrorHandler XSetErrorHandler(XErrorHandler handler) {
    XErrorHandler prev = g_error_handler;
    g_error_handler = handler;
    return prev;
}

/* -- Extension registration. Xmu's CloseHook registers a close
 * callback; we just tell it allocation succeeded with a dummy
 * extension slot. */

XExtCodes *XAddExtension(Display *dpy) {
    (void)dpy;
    XExtCodes *codes = calloc(1, sizeof(*codes));
    if (codes) codes->extension = 0;
    return codes;
}

/* XESetCloseDisplay's typedef is internal to Xlibint.h; declare the
 * minimal signature we need here. Xmu calls it to register a cleanup
 * hook; we never call displays closed, so accept the registration and
 * hand back NULL (meaning "no previous handler"). */

typedef int (*emx11_close_display_proc)(Display *, XExtCodes *);

emx11_close_display_proc XESetCloseDisplay(Display *dpy, int extension,
                                           emx11_close_display_proc proc) {
    (void)dpy; (void)extension; (void)proc;
    return NULL;
}

/* -- Image stubs. XPutImage lives in drawing.c; XCreateImage still stays
 * here because it's pure housekeeping -- the wasm side allocates the
 * XImage header and data buffer; XPutImage is what copies to a Drawable. */

XImage *XCreateImage(Display *dpy, Visual *visual, unsigned int depth,
                     int format, int offset, char *data,
                     unsigned int width, unsigned int height,
                     int bitmap_pad, int bytes_per_line) {
    (void)dpy; (void)visual; (void)format; (void)offset;
    XImage *img = calloc(1, sizeof(*img));
    if (!img) return NULL;
    img->width          = (int)width;
    img->height         = (int)height;
    img->depth          = (int)depth;
    img->data           = data;
    img->xoffset        = 0;
    img->bitmap_pad     = bitmap_pad;
    img->bytes_per_line = bytes_per_line ? bytes_per_line : (int)(width * 4);
    img->bits_per_pixel = 32;
    return img;
}

/* -- More cursor variants. Same story as XDefineCursor: we ignore
 * cursor art, so these just mint unique ids. */

static XID g_cursor_next2 = 0x40000001;

Cursor XCreatePixmapCursor(Display *dpy, Pixmap src, Pixmap mask,
                           XColor *fg, XColor *bg, unsigned int x, unsigned int y) {
    (void)dpy; (void)src; (void)mask; (void)fg; (void)bg; (void)x; (void)y;
    return (Cursor)(g_cursor_next2++);
}

Cursor XCreateGlyphCursor(Display *dpy, Font src_font, Font mask_font,
                          unsigned int src_ch, unsigned int mask_ch,
                          _Xconst XColor *fg, _Xconst XColor *bg) {
    (void)dpy; (void)src_font; (void)mask_font;
    (void)src_ch; (void)mask_ch; (void)fg; (void)bg;
    return (Cursor)(g_cursor_next2++);
}

int XRecolorCursor(Display *dpy, Cursor cursor, XColor *fg, XColor *bg) {
    (void)dpy; (void)cursor; (void)fg; (void)bg; return 1;
}

/* -- Text-property conversion. Xt's Vendor.c uses this when decoding
 * WM_CLASS / WM_NAME. Minimal implementation: copy the text-prop's
 * value verbatim into a single list entry, assume STRING encoding. */

int XmbTextPropertyToTextList(Display *dpy, const XTextProperty *tp,
                              char ***list_return, int *count_return) {
    (void)dpy;
    if (!tp || !list_return || !count_return) return XNoMemory;
    char **list = calloc(2, sizeof(char *));
    if (!list) return XNoMemory;
    list[0] = tp->value ? strdup((const char *)tp->value) : strdup("");
    list[1] = NULL;
    *list_return = list;
    *count_return = 1;
    return 0;                                   /* Success */
}

/* -- xeyes-specific stubs -------------------------------------------------- */

/* XCreateBitmapFromData is the single-plane sibling of
 * XCreatePixmapFromBitmapData (defined further up). xeyes uses it to mint
 * icon/mask bitmaps; since em-x11 has no pixmap backend yet, we mint a
 * unique id and return it unchanged -- the shape path that would actually
 * decode these bits is a future Pixmap milestone. */

extern Pixmap XCreatePixmapFromBitmapData(Display *, Drawable, char *,
                                          unsigned int, unsigned int,
                                          unsigned long, unsigned long,
                                          unsigned int);

Pixmap XCreateBitmapFromData(Display *dpy, Drawable d, _Xconst char *data,
                             unsigned int width, unsigned int height) {
    return XCreatePixmapFromBitmapData(dpy, d, (char *)data, width, height,
                                       1, 0, 1);
}

/* Audible bell -- no sound output in browser-land. xeyes rings it when an
 * unexpected ClientMessage reaches its quit action. */
int XBell(Display *dpy, int percent) {
    (void)dpy; (void)percent;
    return 1;
}

/* XInput2 version negotiation. xeyes probes for XI2 and, if present, uses
 * XIRawMotion on the root window to track the pointer without polling.
 * We do not implement XI2; returning BadRequest routes xeyes to the
 * XtAppAddTimeOut polling path, which we do support. */
Status XIQueryVersion(Display *dpy, int *major_version_inout,
                      int *minor_version_inout) {
    (void)dpy; (void)major_version_inout; (void)minor_version_inout;
    return BadRequest;
}

/* Never reached at runtime (has_xi2() returns 0 above), but xeyes calls
 * it from select_xi2_events() which is linked even when never invoked. */
int XISelectEvents(Display *dpy, Window win, XIEventMask *masks,
                   int num_masks) {
    (void)dpy; (void)win; (void)masks; (void)num_masks;
    return Success;
}

/* -- Locale -- libXt's Initialize.c calls these during XtAppInitialize.
 * Our font path is UTF-8 via canvas.fillText regardless of locale. We
 * report "locale supported" (returns True) because claiming otherwise
 * makes Xt's _XtDefaultLanguageProc warn "locale not supported by Xlib,
 * locale set to C" on every startup -- pure noise for us, since text
 * rendering doesn't care what locale is active. XSetLocaleModifiers
 * likewise accepts any modifier string verbatim. */

Bool XSupportsLocale(void) {
    return True;
}

char *XSetLocaleModifiers(_Xconst char *modifier_list) {
    (void)modifier_list;
    return (char *)"";
}

/* -- X Sync extension. twm uses it for per-window scheduling priorities
 * (XSyncSetPriority/XSyncGetPriority) and probes the extension once at
 * startup. em-x11 has a single-threaded event loop with no scheduler to
 * influence; returning False from QueryExtension tells twm there is no
 * sync support, and the setters/getters collapse to noops. */

Status XSyncQueryExtension(Display *dpy, int *event_base_return,
                           int *error_base_return) {
    (void)dpy;
    if (event_base_return) *event_base_return = 0;
    if (error_base_return) *error_base_return = 0;
    return False;
}

int XSyncSetPriority(Display *dpy, XID client_resource_id, int priority) {
    (void)dpy; (void)client_resource_id; (void)priority;
    return 0;
}

int XSyncGetPriority(Display *dpy, XID client_resource_id,
                     int *return_priority) {
    (void)dpy; (void)client_resource_id;
    if (return_priority) *return_priority = 0;
    return 0;
}

/* -- Window-manager Xlib surface used by twm ------------------------------
 *
 * Phase 0: the goal is to link and start twm's main loop. Everything
 * below has a cheap implementation (track state in the EmxWindow table)
 * where doing so costs nothing, and a pure-stub implementation otherwise.
 * Real semantics -- substructure redirect, focus management, grab
 * pointer routing -- arrive in Phase 1 and Phase 2 when twm starts
 * actually observing clients.
 *
 * None of these rely on ICCCM properties being fully wired. Where a
 * meaningful value exists and we know where to find it, we return it;
 * where not, we return "nothing" cleanly so twm's defensive paths run. */

/* XReparentWindow lives in window.c; it needs to forward to the Host
 * bridge and stays cross-connection-safe. */

/* Save set: X keeps track of windows that should revert to root if the
 * controlling client dies. One-client world -> no-op. */
int XAddToSaveSet(Display *dpy, Window w) { (void)dpy; (void)w; return 1; }
int XRemoveFromSaveSet(Display *dpy, Window w) { (void)dpy; (void)w; return 1; }

/* Subwindow circulation: rotate z-order. No z-order here yet. */
int XCirculateSubwindowsDown(Display *dpy, Window w) { (void)dpy; (void)w; return 1; }
int XCirculateSubwindowsUp(Display *dpy, Window w)   { (void)dpy; (void)w; return 1; }

/* Kill client: twm offers this as a menu action for unresponsive
 * windows. We don't model separate clients; ignore. */
int XKillClient(Display *dpy, XID resource) { (void)dpy; (void)resource; return 1; }

/* Pointer and key grabs. twm uses grabs to intercept drags (resize,
 * move, menu pop) and hot-key bindings. We always accept the grab so
 * the caller's drag loop proceeds; events still go through the normal
 * hit-test path, which is wrong but close enough for Phase 0. */
int XGrabPointer(Display *dpy, Window grab_window, Bool owner_events,
                 unsigned int event_mask, int pointer_mode, int keyboard_mode,
                 Window confine_to, Cursor cursor, Time t) {
    (void)dpy; (void)grab_window; (void)owner_events; (void)event_mask;
    (void)pointer_mode; (void)keyboard_mode; (void)confine_to;
    (void)cursor; (void)t;
    return GrabSuccess;
}

int XWarpPointer(Display *dpy, Window src_w, Window dest_w,
                 int src_x, int src_y, unsigned int src_width, unsigned int src_height,
                 int dest_x, int dest_y) {
    (void)dpy; (void)src_w; (void)dest_w;
    (void)src_x; (void)src_y; (void)src_width; (void)src_height;
    (void)dest_x; (void)dest_y;
    /* Can't move the OS cursor from a browser. Phase 2 can cheat by
     * updating our cached pointer position to match. */
    return 1;
}

int XUngrabKey(Display *dpy, int keycode, unsigned int modifiers,
               Window grab_window) {
    (void)dpy; (void)keycode; (void)modifiers; (void)grab_window;
    return 1;
}

/* Event queue scanners. twm uses these from drag loops (XMaskEvent in
 * MenuMapped) and to coalesce queued events (XCheckTypedWindowEvent in
 * HandleExpose). Implemented as non-blocking peek+pop for now; the
 * blocking XMaskEvent will spin the asyncify yield loop until an event
 * matches. */

Bool XCheckMaskEvent(Display *dpy, long event_mask, XEvent *ev) {
    return emx11_event_queue_peek_match(dpy, event_mask, ev) ? True : False;
}

Bool XCheckTypedWindowEvent(Display *dpy, Window w, int event_type, XEvent *ev) {
    return emx11_event_queue_peek_typed(dpy, w, event_type, ev) ? True : False;
}

int XMaskEvent(Display *dpy, long event_mask, XEvent *ev) {
    for (;;) {
        if (emx11_event_queue_peek_match(dpy, event_mask, ev)) return 1;
        emscripten_sleep(10);
    }
}

/* ICCCM / WM hint readers. None of these are wired to the property
 * subsystem yet; return "nothing" so twm falls back to defaults. */

Status XFetchName(Display *dpy, Window w, char **name_return) {
    if (name_return) *name_return = NULL;
    if (!dpy) return 0;
    /* Traditional (pre-ICCCM) title accessor: reads WM_NAME as an 8-bit
     * STRING and hands back a fresh Xlib-allocated C string. twm's
     * tmgr.c calls this once per managed client to fill the title bar.
     * Without it, TWM's `namelen == 0` path paints the title background
     * and no text -- hence the "solid teal rectangle" symptom. */
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long nitems = 0, bytes_after = 0;
    unsigned char *data = NULL;
    int rc = XGetWindowProperty(dpy, w, XA_WM_NAME, 0, 65536, False,
                                XA_STRING, &actual_type, &actual_format,
                                &nitems, &bytes_after, &data);
    if (rc != Success || actual_type != XA_STRING || actual_format != 8 ||
        !data || nitems == 0) {
        if (data) XFree(data);
        return 0;
    }
    /* XGetWindowProperty already appended a NUL byte past nitems; just
     * hand the buffer back. Client frees via XFree. */
    if (name_return) *name_return = (char *)data;
    else             XFree(data);
    return 1;
}

Status XGetWMIconName(Display *dpy, Window w, XTextProperty *text_prop) {
    (void)dpy; (void)w;
    if (text_prop) memset(text_prop, 0, sizeof(*text_prop));
    return 0;
}

Status XGetTransientForHint(Display *dpy, Window w, Window *prop_window_return) {
    (void)dpy; (void)w;
    if (prop_window_return) *prop_window_return = None;
    return 0;
}

Status XGetWMColormapWindows(Display *dpy, Window w,
                             Window **windows_return, int *count_return) {
    (void)dpy; (void)w;
    if (windows_return) *windows_return = NULL;
    if (count_return)   *count_return   = 0;
    return 0;
}

Status XGetRGBColormaps(Display *dpy, Window w,
                        XStandardColormap **stdcmaps, int *count, Atom property) {
    (void)dpy; (void)w; (void)property;
    if (stdcmaps) *stdcmaps = NULL;
    if (count)    *count    = 0;
    return 0;
}

int XInstallColormap(Display *dpy, Colormap cmap) {
    (void)dpy; (void)cmap;
    /* Single-visual world; no per-window colormap switching needed. */
    return 1;
}

/* Cut buffers. Legacy inter-client clipboard from X10 days, still used
 * by a few programs (twm's F_CUTFILE, xterm's middle-click). Not worth
 * persisting across wasm reloads. */

char *XFetchBytes(Display *dpy, int *nbytes_return) {
    (void)dpy;
    if (nbytes_return) *nbytes_return = 0;
    return NULL;
}

int XStoreBytes(Display *dpy, _Xconst char *bytes, int nbytes) {
    (void)dpy; (void)bytes; (void)nbytes;
    return 1;
}

/* Parse "WxH[+/-X[+/-Y]]" geometry strings. Used by twm for default
 * icon manager placement. Returns a bitmask of which of XValue/YValue/
 * WidthValue/HeightValue fields are populated. Accepting a degenerate
 * input returns 0 (no fields). */
int XParseGeometry(_Xconst char *geom, int *x, int *y,
                   unsigned int *width, unsigned int *height) {
    if (!geom || !*geom) return 0;
    int mask = 0;
    const char *p = geom;
    unsigned int uval;
    int sval;

    /* [W x H] */
    if (*p >= '0' && *p <= '9') {
        uval = 0;
        while (*p >= '0' && *p <= '9') { uval = uval * 10 + (unsigned)(*p++ - '0'); }
        if (*p == 'x' || *p == 'X') {
            if (width) *width = uval;
            mask |= WidthValue;
            p++;
            uval = 0;
            while (*p >= '0' && *p <= '9') { uval = uval * 10 + (unsigned)(*p++ - '0'); }
            if (height) *height = uval;
            mask |= HeightValue;
        }
    }
    /* [+/-X+/-Y] */
    for (int axis = 0; axis < 2; axis++) {
        int sign = 0;
        if (*p == '+') { sign = 1; p++; }
        else if (*p == '-') { sign = -1; p++; }
        else break;
        sval = 0;
        while (*p >= '0' && *p <= '9') { sval = sval * 10 + (*p++ - '0'); }
        if (sign < 0) sval = -sval;
        if (axis == 0) { if (x) *x = sval; mask |= XValue; if (sign < 0) mask |= XNegative; }
        else           { if (y) *y = sval; mask |= YValue; if (sign < 0) mask |= YNegative; }
    }
    return mask;
}

int XSetWindowBorderWidth(Display *dpy, Window w, unsigned int width) {
    EmxWindow *win = emx11_window_find(dpy, w);
    if (!win) return 0;
    win->border_width = width;
    return 1;
}

/* Error text: Xlib historically renders numeric error codes into human
 * strings via its resource database. We don't ship one; write a bland
 * placeholder so callers (twm's error handler in particular) have
 * something to print. */
int XGetErrorText(Display *dpy, int code, char *buffer_return, int length) {
    (void)dpy;
    if (!buffer_return || length <= 0) return 0;
    snprintf(buffer_return, (size_t)length, "X error %d", code);
    return 0;
}

int XGetErrorDatabaseText(Display *dpy, _Xconst char *name, _Xconst char *message,
                          _Xconst char *default_string, char *buffer_return, int length) {
    (void)dpy; (void)name; (void)message;
    if (!buffer_return || length <= 0) return 0;
    const char *src = default_string ? default_string : "";
    size_t n = strlen(src);
    if (n >= (size_t)length) n = (size_t)length - 1;
    memcpy(buffer_return, src, n);
    buffer_return[n] = '\0';
    return 0;
}

/* Multibyte image string. Route to the 8-bit XDrawImageString; our
 * canvas-fillText font path is UTF-8 internally either way, and the
 * XFontSet argument is already a thin wrapper around a loaded CSS font. */
void XmbDrawImageString(Display *dpy, Drawable d, XFontSet fontset, GC gc,
                        int x, int y, _Xconst char *text, int length) {
    (void)fontset;
    XDrawImageString(dpy, d, gc, x, y, text, length);
}
