/*
 * Xlib/Xt stubs needed specifically by libXaw (and its Xmu dependency).
 *
 * Split out from xt_stubs.c so the dependency chain is obvious: libXt
 * needs what's in xt_stubs.c; libXaw needs everything here *plus* what
 * xt_stubs provides. Many of these are "link-time presence, runtime
 * noop" -- Xaw's Label widget references XCopyPlane in its bitmap
 * rendering path, but an ASCII label never actually reaches that
 * branch. The stubs keep the linker happy without pretending to
 * implement machinery we have not yet built.
 *
 * Anything that starts to matter at runtime gets promoted from here
 * to a real implementation file.
 */

#include "emx11_internal.h"

#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

/* -- 16-bit text is the same as 8-bit for our Latin-only font path.
 * Xaw's Label only uses the 16-bit calls when the resource manager
 * declares a two-byte font, which we never do; but the linker still
 * needs the symbols. Fold the byte pairs into low-byte-only strings
 * so a defensive call does not crash. */

static char *xchar2b_to_ascii(_Xconst XChar2b *s, int n, char *stack_buf,
                              size_t stack_cap) {
    char *out = (size_t)n + 1 <= stack_cap ? stack_buf : malloc((size_t)n + 1);
    if (!out) return NULL;
    for (int i = 0; i < n; i++) {
        out[i] = (char)s[i].byte2;              /* assume low byte is enough */
    }
    out[n] = '\0';
    return out;
}

int XDrawString16(Display *dpy, Drawable d, GC gc, int x, int y,
                  _Xconst XChar2b *string, int length) {
    char stack[256], *buf = xchar2b_to_ascii(string, length, stack, sizeof stack);
    if (!buf) return 0;
    int r = XDrawString(dpy, d, gc, x, y, buf, length);
    if (buf != stack) free(buf);
    return r;
}

int XTextWidth16(XFontStruct *fs, _Xconst XChar2b *string, int count) {
    char stack[256], *buf = xchar2b_to_ascii(string, count, stack, sizeof stack);
    if (!buf) return 0;
    int w = XTextWidth(fs, buf, count);
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

/* -- Pixmap stubs.
 * Pixmap is a server-side offscreen drawable. We have no offscreen
 * backend yet -- everything paints directly to the one canvas -- so
 * these are link-time placeholders. Xaw's Label/Command paths only
 * use Pixmaps when the client provides a bitmap resource, which the
 * default ASCII-label path does not. If a demo starts using Pixmaps,
 * these get promoted to real implementations backed by an ImageData
 * or OffscreenCanvas. */

static XID g_pixmap_next = 0x30000001;

Pixmap XCreatePixmap(Display *dpy, Drawable d, unsigned int w, unsigned int h,
                     unsigned int depth) {
    (void)dpy; (void)d; (void)w; (void)h; (void)depth;
    return (Pixmap)(g_pixmap_next++);
}

int XFreePixmap(Display *dpy, Pixmap pixmap) {
    (void)dpy; (void)pixmap; return 1;
}

int XCopyArea(Display *dpy, Drawable src, Drawable dst, GC gc,
              int src_x, int src_y, unsigned int width, unsigned int height,
              int dst_x, int dst_y) {
    (void)dpy; (void)src; (void)dst; (void)gc;
    (void)src_x; (void)src_y; (void)width; (void)height; (void)dst_x; (void)dst_y;
    return 1;
}

int XCopyPlane(Display *dpy, Drawable src, Drawable dst, GC gc,
               int src_x, int src_y, unsigned int width, unsigned int height,
               int dst_x, int dst_y, unsigned long plane) {
    (void)dpy; (void)src; (void)dst; (void)gc;
    (void)src_x; (void)src_y; (void)width; (void)height;
    (void)dst_x; (void)dst_y; (void)plane;
    return 1;
}

Pixmap XCreatePixmapFromBitmapData(Display *dpy, Drawable d, char *data,
                                   unsigned int w, unsigned int h,
                                   unsigned long fg, unsigned long bg,
                                   unsigned int depth) {
    (void)dpy; (void)d; (void)data; (void)w; (void)h;
    (void)fg; (void)bg; (void)depth;
    return (Pixmap)(g_pixmap_next++);
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
 * tooltip placement. Return "pointer at (0,0) with no buttons" -- the
 * tooltip will just render off-screen, which is fine for now. */

Bool XQueryPointer(Display *dpy, Window w, Window *root_return,
                   Window *child_return, int *root_x_return, int *root_y_return,
                   int *win_x_return, int *win_y_return,
                   unsigned int *mask_return) {
    (void)w;
    if (root_return)     *root_return     = dpy->screens[0].root;
    if (child_return)    *child_return    = None;
    if (root_x_return)   *root_x_return   = 0;
    if (root_y_return)   *root_y_return   = 0;
    if (win_x_return)    *win_x_return    = 0;
    if (win_y_return)    *win_y_return    = 0;
    if (mask_return)     *mask_return     = 0;
    return True;
}

int XGetFontProperty(XFontStruct *fs, Atom atom, unsigned long *value_return) {
    (void)fs; (void)atom;
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

/* -- Selection stubs.
 * Xt's Selection.c drags selection APIs in even when no selection
 * conversion is actually requested. Record the owner so that a
 * later XGetSelectionOwner returns the same value consistently. */

static Window g_selection_owner = None;

int XSetSelectionOwner(Display *dpy, Atom selection, Window owner, Time t) {
    (void)dpy; (void)selection; (void)t;
    g_selection_owner = owner;
    return 1;
}

Window XGetSelectionOwner(Display *dpy, Atom selection) {
    (void)dpy; (void)selection;
    return g_selection_owner;
}

int XConvertSelection(Display *dpy, Atom selection, Atom target, Atom property,
                      Window requestor, Time t) {
    (void)dpy; (void)selection; (void)target; (void)property;
    (void)requestor; (void)t;
    return 1;                                   /* pretend we sent it */
}

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

/* -- Image stubs. Real XCreateImage / XPutImage would marshal pixel
 * data; libXpm uses them to upload a decoded XPM. Since we never
 * successfully load a pixmap (XPM loader returns failure), XPutImage
 * never runs -- but the linker still wants it. */

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

int XPutImage(Display *dpy, Drawable d, GC gc, XImage *image,
              int src_x, int src_y, int dst_x, int dst_y,
              unsigned int w, unsigned int h) {
    (void)dpy; (void)d; (void)gc; (void)image;
    (void)src_x; (void)src_y; (void)dst_x; (void)dst_y; (void)w; (void)h;
    return 1;
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
