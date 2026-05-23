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
#include "emx11_meta_layout.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XInput2.h>
#include <emscripten.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

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

/* -- Xmb / Xwc / Xutf8 text family -----------------------------------
 *
 * xt_stubs.c hands back a real (non-NULL) XFontSet whose first font is
 * the one we want to render with. The Xmb/Xutf8 variants route the
 * UTF-8 bytes straight through XDrawString — canvas.fillText speaks
 * UTF-8 natively. The Xwc variants encode wchar_t (UCS-4 in Emscripten)
 * into UTF-8 first, then take the same path.
 *
 * All three families temporarily install the fontset's font into the
 * GC so widgets that build a separate fontset per render-table tag get
 * the font they asked for, not whatever font is sitting in the GC.
 */

extern XFontStruct *emx11_fontset_font(XFontSet font_set);

static int wc_to_utf8_one(unsigned int cp, unsigned char *out) {
    if (cp < 0x80) { out[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (unsigned char)(0xC0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (unsigned char)(0xE0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp < 0x110000) {
        out[0] = (unsigned char)(0xF0 | (cp >> 18));
        out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (unsigned char)(0x80 | (cp & 0x3F));
        return 4;
    }
    out[0] = '?';
    return 1;
}

/* Encode a wchar_t span into a malloc'd UTF-8 buffer (NUL-terminated).
 * Returns byte length via *out_bytes. Caller frees the buffer. */
static unsigned char *wcs_to_utf8(const wchar_t *ws, int nw, int *out_bytes) {
    if (!ws || nw <= 0) {
        unsigned char *empty = malloc(1);
        if (empty) empty[0] = 0;
        if (out_bytes) *out_bytes = 0;
        return empty;
    }
    size_t cap = (size_t)nw * 4 + 1;
    unsigned char *buf = malloc(cap);
    if (!buf) { if (out_bytes) *out_bytes = 0; return NULL; }
    int used = 0;
    for (int i = 0; i < nw; i++) {
        used += wc_to_utf8_one((unsigned int)ws[i], buf + used);
    }
    buf[used] = 0;
    if (out_bytes) *out_bytes = used;
    return buf;
}

/* Count UTF-8 characters (not bytes) in [text, text+bytes). Treats
 * malformed leads as one char each so we never under-count. */
static int utf8_char_count(const char *text, int bytes) {
    int n = 0;
    for (int i = 0; i < bytes; ) {
        unsigned char c = (unsigned char)text[i];
        int step = (c < 0x80) ? 1 :
                   ((c & 0xE0) == 0xC0) ? 2 :
                   ((c & 0xF0) == 0xE0) ? 3 :
                   ((c & 0xF8) == 0xF0) ? 4 : 1;
        if (i + step > bytes) step = 1;
        i += step;
        n++;
    }
    return n;
}

static void draw_with_fontset(Display *dpy, Drawable d, XFontSet font_set,
                              GC gc, int x, int y,
                              const char *text, int bytes, int image_mode) {
    if (!gc || !text || bytes <= 0) return;
    XFontStruct *fs = emx11_fontset_font(font_set);
    Font saved = gc->font;
    if (fs) gc->font = fs->fid;
    if (image_mode) XDrawImageString(dpy, d, gc, x, y, text, bytes);
    else            XDrawString(dpy, d, gc, x, y, text, bytes);
    gc->font = saved;
}

void XmbDrawString(Display *dpy, Drawable d, XFontSet font_set, GC gc,
                   int x, int y, _Xconst char *text, int bytes) {
    draw_with_fontset(dpy, d, font_set, gc, x, y, text, bytes, 0);
}

void Xutf8DrawString(Display *dpy, Drawable d, XFontSet font_set, GC gc,
                     int x, int y, _Xconst char *text, int bytes) {
    draw_with_fontset(dpy, d, font_set, gc, x, y, text, bytes, 0);
}

void XwcDrawString(Display *dpy, Drawable d, XFontSet font_set, GC gc,
                   int x, int y, _Xconst wchar_t *text, int num_wchars) {
    int bytes = 0;
    unsigned char *u8 = wcs_to_utf8(text, num_wchars, &bytes);
    if (u8) {
        draw_with_fontset(dpy, d, font_set, gc, x, y,
                          (const char *)u8, bytes, 0);
        free(u8);
    }
}

void XmbDrawImageString(Display *dpy, Drawable d, XFontSet font_set, GC gc,
                        int x, int y, _Xconst char *text, int bytes) {
    draw_with_fontset(dpy, d, font_set, gc, x, y, text, bytes, 1);
}

void Xutf8DrawImageString(Display *dpy, Drawable d, XFontSet font_set, GC gc,
                          int x, int y, _Xconst char *text, int bytes) {
    draw_with_fontset(dpy, d, font_set, gc, x, y, text, bytes, 1);
}

void XwcDrawImageString(Display *dpy, Drawable d, XFontSet font_set, GC gc,
                        int x, int y, _Xconst wchar_t *text, int num_wchars) {
    int bytes = 0;
    unsigned char *u8 = wcs_to_utf8(text, num_wchars, &bytes);
    if (u8) {
        draw_with_fontset(dpy, d, font_set, gc, x, y,
                          (const char *)u8, bytes, 1);
        free(u8);
    }
}

static int escapement_with_fontset(XFontSet font_set, const char *text, int bytes) {
    if (!text || bytes <= 0) return 0;
    XFontStruct *fs = emx11_fontset_font(font_set);
    if (fs) return XTextWidth(fs, text, bytes);
    return bytes * 7;
}

int XmbTextEscapement(XFontSet font_set, _Xconst char *text, int bytes) {
    return escapement_with_fontset(font_set, text, bytes);
}

int Xutf8TextEscapement(XFontSet font_set, _Xconst char *text, int bytes) {
    return escapement_with_fontset(font_set, text, bytes);
}

int XwcTextEscapement(XFontSet font_set, _Xconst wchar_t *text, int num_wchars) {
    int bytes = 0;
    unsigned char *u8 = wcs_to_utf8(text, num_wchars, &bytes);
    if (!u8) return 0;
    int w = escapement_with_fontset(font_set, (const char *)u8, bytes);
    free(u8);
    return w;
}

static int extents_with_fontset(XFontSet font_set, const char *text, int bytes,
                                XRectangle *ink, XRectangle *logical) {
    XFontStruct *fs = emx11_fontset_font(font_set);
    int width   = fs ? XTextWidth(fs, text, bytes) : bytes * 7;
    int ascent  = fs ? fs->ascent  : 10;
    int descent = fs ? fs->descent : 2;
    XRectangle r = {0, (short)-ascent, (unsigned short)width,
                    (unsigned short)(ascent + descent)};
    if (ink)     *ink     = r;
    if (logical) *logical = r;
    return width;
}

int XmbTextExtents(XFontSet font_set, _Xconst char *text, int nbytes,
                   XRectangle *ink, XRectangle *logical) {
    return extents_with_fontset(font_set, text, nbytes, ink, logical);
}

int Xutf8TextExtents(XFontSet font_set, _Xconst char *text, int nbytes,
                     XRectangle *ink, XRectangle *logical) {
    return extents_with_fontset(font_set, text, nbytes, ink, logical);
}

int XwcTextExtents(XFontSet font_set, _Xconst wchar_t *text, int num_wchars,
                   XRectangle *ink, XRectangle *logical) {
    int bytes = 0;
    unsigned char *u8 = wcs_to_utf8(text, num_wchars, &bytes);
    if (!u8) return 0;
    int w = extents_with_fontset(font_set, (const char *)u8, bytes, ink, logical);
    free(u8);
    return w;
}

/* PerCharExtents: walk one UTF-8 character at a time, measure width
 * incrementally, fill ink/logical arrays with per-glyph rects. ink ==
 * logical here because canvas.measureText gives us no ink bounds.
 * Used by Motif XmText for cursor placement and click-to-position. */
static Status percharextents_utf8(XFontSet font_set, const char *text, int bytes,
                                  XRectangle *ink_buf, XRectangle *log_buf,
                                  int buf_size, int *num_chars,
                                  XRectangle *overall_ink,
                                  XRectangle *overall_log) {
    int total_chars = utf8_char_count(text, bytes);
    if (num_chars) *num_chars = total_chars;
    if (total_chars > buf_size) return 0;

    XFontStruct *fs = emx11_fontset_font(font_set);
    int ascent  = fs ? fs->ascent  : 10;
    int descent = fs ? fs->descent : 2;
    int prev_w  = 0;
    int idx     = 0;

    for (int i = 0; i < bytes; ) {
        unsigned char c = (unsigned char)text[i];
        int step = (c < 0x80) ? 1 :
                   ((c & 0xE0) == 0xC0) ? 2 :
                   ((c & 0xF0) == 0xE0) ? 3 :
                   ((c & 0xF8) == 0xF0) ? 4 : 1;
        if (i + step > bytes) step = bytes - i;
        int run_w = fs ? XTextWidth(fs, text, i + step) : (i + step) * 7;
        int adv   = run_w - prev_w;
        XRectangle r = {(short)prev_w, (short)-ascent,
                        (unsigned short)(adv > 0 ? adv : 0),
                        (unsigned short)(ascent + descent)};
        if (ink_buf) ink_buf[idx] = r;
        if (log_buf) log_buf[idx] = r;
        prev_w = run_w;
        idx++;
        i += step;
    }
    XRectangle overall = {0, (short)-ascent,
                          (unsigned short)prev_w,
                          (unsigned short)(ascent + descent)};
    if (overall_ink) *overall_ink = overall;
    if (overall_log) *overall_log = overall;
    return 1;
}

Status XmbTextPerCharExtents(XFontSet font_set, _Xconst char *text, int bytes,
                             XRectangle *ink_buf, XRectangle *log_buf,
                             int buf_size, int *num_chars,
                             XRectangle *overall_ink, XRectangle *overall_log) {
    return percharextents_utf8(font_set, text, bytes, ink_buf, log_buf,
                               buf_size, num_chars, overall_ink, overall_log);
}

Status Xutf8TextPerCharExtents(XFontSet font_set, _Xconst char *text, int bytes,
                               XRectangle *ink_buf, XRectangle *log_buf,
                               int buf_size, int *num_chars,
                               XRectangle *overall_ink, XRectangle *overall_log) {
    return percharextents_utf8(font_set, text, bytes, ink_buf, log_buf,
                               buf_size, num_chars, overall_ink, overall_log);
}

Status XwcTextPerCharExtents(XFontSet font_set, _Xconst wchar_t *text, int num_wchars,
                             XRectangle *ink_buf, XRectangle *log_buf,
                             int buf_size, int *num_chars,
                             XRectangle *overall_ink, XRectangle *overall_log) {
    int bytes = 0;
    unsigned char *u8 = wcs_to_utf8(text, num_wchars, &bytes);
    if (!u8) return 0;
    Status s = percharextents_utf8(font_set, (const char *)u8, bytes,
                                   ink_buf, log_buf, buf_size, num_chars,
                                   overall_ink, overall_log);
    free(u8);
    /* num_chars is in wchar_t units, which equals codepoints == utf8 chars */
    return s;
}

/* DrawText: array of (fontset, string, delta) segments. delta is an
 * x-offset applied before each segment after the first. */
void XmbDrawText(Display *dpy, Drawable d, GC gc, int x, int y,
                 XmbTextItem *items, int nitems) {
    if (!items) return;
    for (int i = 0; i < nitems; i++) {
        if (i > 0) x += items[i].delta;
        XmbDrawString(dpy, d, items[i].font_set, gc, x, y,
                      items[i].chars, items[i].nchars);
        x += XmbTextEscapement(items[i].font_set, items[i].chars, items[i].nchars);
    }
}

void Xutf8DrawText(Display *dpy, Drawable d, GC gc, int x, int y,
                   XmbTextItem *items, int nitems) {
    if (!items) return;
    for (int i = 0; i < nitems; i++) {
        if (i > 0) x += items[i].delta;
        Xutf8DrawString(dpy, d, items[i].font_set, gc, x, y,
                        items[i].chars, items[i].nchars);
        x += Xutf8TextEscapement(items[i].font_set, items[i].chars, items[i].nchars);
    }
}

void XwcDrawText(Display *dpy, Drawable d, GC gc, int x, int y,
                 XwcTextItem *items, int nitems) {
    if (!items) return;
    for (int i = 0; i < nitems; i++) {
        if (i > 0) x += items[i].delta;
        XwcDrawString(dpy, d, items[i].font_set, gc, x, y,
                      items[i].chars, items[i].nchars);
        x += XwcTextEscapement(items[i].font_set, items[i].chars, items[i].nchars);
    }
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
 * XCopyArea / XCopyPlane / XPutImage live in drawing.c. XReadBitmapFile
 * still no-ops (X11 bitmap file format decoder unimplemented; nothing
 * we run reads bitmaps from disk). */

Pixmap XCreatePixmapFromBitmapData(Display *dpy, Drawable d, char *data,
                                   unsigned int w, unsigned int h,
                                   unsigned long fg, unsigned long bg,
                                   unsigned int depth) {
    /* Mint a real pixmap, then expand the 1-bit input into it via the
     * XYBitmap put_image path: set bits -> fg, unset -> bg. twm uses
     * this to bake its hilite/menu stipple into a colored tile that
     * XSetWindowBackgroundPixmap then references; without the actual
     * bits, the tile is a transparent canvas and the title-bar hilite
     * paints as a black rectangle. */
    Pixmap pm = XCreatePixmap(dpy, d, w, h, depth);
    if (pm == None || !data || w == 0 || h == 0) return pm;
    int bpl = (int)((w + 7u) / 8u);
    int data_len = bpl * (int)h;
    emx11_js_put_image(pm, 0, 0, w, h,
                       XYBitmap, 1, bpl,
                       (const unsigned char *)data, data_len,
                       fg, bg);
    return pm;
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

/* -- Window border setters wire through to Host so the compositor can
 * repaint the border ring. XSetWindowBorderPixmap is still ignored
 * (no pixmap-tiled borders modelled). */

int XSetWindowBorder(Display *dpy, Window w, unsigned long border) {
    EmxWindow *win = emx11_window_find(dpy, w);
    if (!win) return 0;
    win->border_pixel = border;
    emx11_js_window_set_border(w, win->border_width, win->border_pixel);
    return 1;
}
int XSetWindowBorderPixmap(Display *dpy, Window w, Pixmap pixmap) {
    (void)dpy; (void)w; (void)pixmap; return 1;
}
int XDefineCursor(Display *dpy, Window w, Cursor cursor) {
    dpy->request++;
    emx11_js_window_set_cursor(w, (unsigned int)cursor);
    return 1;
}
int XUndefineCursor(Display *dpy, Window w) {
    dpy->request++;
    emx11_js_window_set_cursor(w, 0);
    return 1;
}

/* -- Query stubs. XQueryPointer is called by Xaw's Tip widget for
 * tooltip placement, by xeyes every 50ms for pupil tracking, and by
 * twm's menu loop (menus.c:509) on every MotionNotify to decide which
 * menu entry the pointer is over. Read the latest canvas pointer
 * position from the JS host, then translate into the requested
 * window's local coordinate system. Without the translation, twm's
 * menu code sees root-space x and rejects every hover whose root-x
 * is >= menu->width -- symptom: menu items only highlight when the
 * pointer is to the LEFT of the visible menu by an amount equal to
 * the menu's root-space origin. */

Bool XQueryPointer(Display *dpy, Window w, Window *root_return,
                   Window *child_return, int *root_x_return, int *root_y_return,
                   int *win_x_return, int *win_y_return,
                   unsigned int *mask_return) {
    int px = 0, py = 0;
    emx11_js_pointer_xy(&px, &py);
    /* Window-local coords: root coords minus the target window's
     * absolute origin. Ask the JS host (authoritative for every window,
     * including ones this connection doesn't own) to avoid duplicating
     * the parent-chain walk -- same bridge event.c uses for cross-conn
     * reparented windows. If the lookup fails (shouldn't happen for a
     * window the caller just passed us), fall back to root coords so
     * the pre-fix behaviour is preserved. */
    int wx = px, wy = py;
    if (w != None) {
        int origin[EMX11_ABS_ORIGIN_SIZE] = {0};
        emx11_js_get_window_abs_origin(w, origin);
        if (origin[EMX11_ABS_ORIGIN_PRESENT]) {
            wx = px - origin[EMX11_ABS_ORIGIN_AX];
            wy = py - origin[EMX11_ABS_ORIGIN_AY];
        }
    }
    EM_ASM({
        var d = globalThis.emX11 && globalThis.emX11._debug;
        if (d && d.traceQp) {
            console.log('[c-qp] conn=' + $0 + ' win=' + $1 +
                        ' root=(' + $2 + ',' + $3 + ')' +
                        ' local=(' + $4 + ',' + $5 + ')');
        }
    }, dpy->conn_id, w, px, py, wx, wy);
    if (root_return)     *root_return     = dpy->screens[0].root;
    if (child_return)    *child_return    = None;
    if (root_x_return)   *root_x_return   = px;
    if (root_y_return)   *root_y_return   = py;
    if (win_x_return)    *win_x_return    = wx;
    if (win_y_return)    *win_y_return    = wy;
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

/* -- XIM stubs that don't deserve their own file.
 * Real XOpenIM / XCreateIC / XSetICFocus / XSetICValues / XGetICValues
 * / XGetIMValues / XDestroyIC / XCloseIM live in xim.c. The two helpers
 * below stay here because they have no Tier A behaviour: XDisplayOfIM
 * isn't called by Tk (Xaw used to ask), and XVaCreateNestedList just
 * needs to return a non-NULL pointer so XCreateIC's varargs caller
 * doesn't early-out. */

Display *XDisplayOfIM(XIM im) {
    (void)im; return NULL;
}

/* XVaCreateNestedList captures (name, value) pairs into a heap-allocated
 * sentinel-terminated array. xim.c decodes it on demand when it sees the
 * outer XNPreeditAttributes / XNStatusAttributes attribute. The first
 * slot is a magic header so xim.c can recognise our payload (Tk passes
 * arbitrary pointers through XCreateIC's varargs and we mustn't deref
 * a non-list value as one).
 *
 * Layout: [magic][count][n1][v1][n2][v2]...[NULL]
 * All slots are void*; names are const char*, values are stored as-is. */

static const char EMX11_NESTED_LIST_MAGIC[] = "emx11-nested-list";

XVaNestedList XVaCreateNestedList(int unused_dummy, ...) {
    (void)unused_dummy;
    /* Walk once to count, then again to copy. */
    va_list ap;
    int n = 0;
    va_start(ap, unused_dummy);
    for (;;) {
        const char *name = va_arg(ap, const char *);
        if (!name) break;
        (void)va_arg(ap, void *);
        n++;
    }
    va_end(ap);

    /* +1 magic +1 count +2 per entry +1 terminator. */
    void **buf = (void **)calloc(2 + 2 * n + 1, sizeof(void *));
    if (!buf) return NULL;
    buf[0] = (void *)EMX11_NESTED_LIST_MAGIC;
    buf[1] = (void *)(uintptr_t)n;
    int o = 2;
    va_start(ap, unused_dummy);
    for (int i = 0; i < n; i++) {
        const char *name = va_arg(ap, const char *);
        void *value      = va_arg(ap, void *);
        buf[o++] = (void *)name;
        buf[o++] = value;
    }
    va_end(ap);
    buf[o] = NULL;
    return (XVaNestedList)buf;
}

/* xim.c reads the captured list. Returns 1 + writes (count, name_array,
 * value_array) when the pointer looks valid, 0 otherwise. */
int emx11_nested_list_decode(void *list, int *count_out,
                             const char ***names_out, void ***values_out) {
    if (!list) return 0;
    void **slots = (void **)list;
    if (slots[0] != (void *)EMX11_NESTED_LIST_MAGIC) return 0;
    int n = (int)(uintptr_t)slots[1];
    static const char *name_buf[16];
    static void       *val_buf[16];
    if (n > 16) n = 16;
    for (int i = 0; i < n; i++) {
        name_buf[i] = (const char *)slots[2 + 2 * i + 0];
        val_buf[i]  = slots[2 + 2 * i + 1];
    }
    if (count_out)  *count_out  = n;
    if (names_out)  *names_out  = name_buf;
    if (values_out) *values_out = val_buf;
    return 1;
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

/* -- Image lifecycle: XCreateImage, XGetImage, pixel accessors, and
 * XInitImage / XDestroyImage.
 *
 * XPutImage lives in drawing.c (it bridges drawable data to the host);
 * the pixel accessors and XGetImage are here alongside XCreateImage so the
 * whole XImage pipeline stays in one place.
 *
 * Pixel format: em-x11's single screen is 24-bit TrueColor with 32bpp
 * (B,G,R,A in wasm memory, matching the PutImage BGRA path). The pixel
 * accessors below encode/decode 0x00RRGGBB ↔ BGRA bytes. */

#define ROUNDUP(nbytes, pad) (((((nbytes) - 1) + (pad)) / (pad)) * (pad))

static int _emx11_bits_per_pixel(Display *dpy, int depth) {
    ScreenFormat *fmt = dpy->pixmap_format;
    for (int i = dpy->nformats; i > 0; i--, fmt++) {
        if (fmt->depth == depth) return fmt->bits_per_pixel;
    }
    if (depth <= 1)  return 1;
    if (depth <= 4)  return 4;
    if (depth <= 8)  return 8;
    if (depth <= 16) return 16;
    return 32;
}

int _XInitImageFuncPtrs(XImage *image);

XImage *XCreateImage(Display *dpy, Visual *visual, unsigned int depth,
                     int format, int offset, char *data,
                     unsigned int width, unsigned int height,
                     int bitmap_pad, int bytes_per_line) {
    if (depth == 0 || depth > 32) return NULL;
    if (format != XYBitmap && format != XYPixmap && format != ZPixmap)
        return NULL;
    if (format == XYBitmap && depth != 1) return NULL;
    if (bitmap_pad != 8 && bitmap_pad != 16 && bitmap_pad != 32) return NULL;
    if (offset < 0) return NULL;

    XImage *img = calloc(1, sizeof(*img));
    if (!img) return NULL;

    img->width       = (int)width;
    img->height      = (int)height;
    img->format      = format;
    img->depth       = (int)depth;
    img->data        = data;
    img->xoffset     = offset;
    img->bitmap_pad  = bitmap_pad;

    img->byte_order       = dpy->byte_order;
    img->bitmap_unit      = dpy->bitmap_unit;
    img->bitmap_bit_order = dpy->bitmap_bit_order;

    if (visual) {
        img->red_mask   = visual->red_mask;
        img->green_mask = visual->green_mask;
        img->blue_mask  = visual->blue_mask;
    }

    int bpp = (format == ZPixmap) ? _emx11_bits_per_pixel(dpy, (int)depth) : 1;
    img->bits_per_pixel = bpp;

    int min_bpl;
    if (format == ZPixmap)
        min_bpl = ROUNDUP(bpp * (int)width, bitmap_pad);
    else
        min_bpl = ROUNDUP((int)width + offset, bitmap_pad);

    if (bytes_per_line == 0)
        img->bytes_per_line = min_bpl;
    else if (bytes_per_line < min_bpl) {
        free(img);
        return NULL;
    } else
        img->bytes_per_line = bytes_per_line;

    _XInitImageFuncPtrs(img);
    return img;
}

/* -- XImage pixel accessors (32bpp ZPixmap BGRA) ---------------------------
 * Visual masks (TrueColor): red=0xff0000 green=0xff00 blue=0xff.
 * Pixel values from Tk_GetColorByValue are 0x00RRGGBB.
 * The data buffer is BGRA byte order matching the PutImage path. */

static unsigned long _emx11_get_pixel(XImage *img, int x, int y) {
    unsigned char *p = (unsigned char *)img->data + y * img->bytes_per_line + x * 4;
    return ((unsigned long)p[2] << 16) | ((unsigned long)p[1] << 8) | (unsigned long)p[0];
}

static int _emx11_put_pixel(XImage *img, int x, int y, unsigned long pixel) {
    unsigned char *p = (unsigned char *)img->data + y * img->bytes_per_line + x * 4;
    p[0] = (unsigned char)(pixel & 0xff);
    p[1] = (unsigned char)((pixel >> 8) & 0xff);
    p[2] = (unsigned char)((pixel >> 16) & 0xff);
    p[3] = 0xff;
    return 1;
}

static int _emx11_destroy_image(XImage *img) {
    free(img->data);
    img->data = NULL;
    free(img);
    return 1;
}

/* -- InitImageFuncPtrs: wire the 32bpp BGRA accessors (the only pixel
 * format em-x11 surfaces use). Callers that create depth-1 XYBitmap images
 * bypass XPutPixel and write raw bitmap bytes directly, so a single set of
 * accessors suffices for every XImage in the process. */

int _XInitImageFuncPtrs(XImage *image) {
    if (!image) return 0;
    image->f.get_pixel      = _emx11_get_pixel;
    image->f.put_pixel      = _emx11_put_pixel;
    image->f.destroy_image  = _emx11_destroy_image;
    return 1;
}

/* -- XGetImage — does NOT read back from the browser compositor (that
 * would require an async GPU round-trip). Instead it returns a
 * zero-filled XImage that callers can write to via XPutPixel and then
 * commit with XPutImage. This is sufficient for the "create blank
 * buffer, stamp pixels, blit" pattern that Tk's checkbutton/radiobutton
 * indicator drawing and ttk theme element building rely on.
 *
 * Real readback for photo-image capture and postscript export can be
 * added later via a Host method that returns RGBA bytes. */

XImage *XGetImage(Display *dpy, Drawable d, int x, int y,
                  unsigned int w, unsigned int h,
                  unsigned long plane_mask, int format) {
    (void)dpy; (void)d; (void)x; (void)y; (void)plane_mask;
    if (w == 0 || h == 0) return NULL;

    /* Derive the correct depth for the requested format so XCreateImage
     * produces a consistent XImage: ZPixmap → 24, XYBitmap → 1. */
    int depth = (format == XYBitmap) ? 1 : (int)dpy->screens[0].root_depth;
    XImage *img = XCreateImage(dpy, NULL, (unsigned int)depth, format, 0,
                               NULL, w, h, dpy->bitmap_pad, 0);
    if (!img) return NULL;

    int data_size = img->bytes_per_line * (int)h;
    img->data = calloc(1, (size_t)data_size);
    if (!img->data) {
        free(img);
        return NULL;
    }
    return img;
}

/* -- More cursor variants. Same story as XDefineCursor: we ignore
 * cursor art, so these just mint unique ids. */

static XID g_cursor_next2 = 0x40000001;
#define EMX11_FONT_CURSOR_TAG 0x70000000u

Cursor XCreatePixmapCursor(Display *dpy, Pixmap src, Pixmap mask,
                           XColor *fg, XColor *bg, unsigned int x, unsigned int y) {
    (void)dpy; (void)src; (void)mask; (void)fg; (void)bg; (void)x; (void)y;
    return (Cursor)(g_cursor_next2++);
}

/* XCreateGlyphCursor encodes src_ch (the glyph index in the cursor font,
 * which is the XC_* shape constant) with EMX11_FONT_CURSOR_TAG so the JS
 * host can map it to a CSS cursor keyword. Without this, Tk's
 * TkGetCursorByName path (XCreateGlyphCursor, not XCreateFontCursor)
 * produced pixmap-cursor ids that all resolved to 'default'. */
Cursor XCreateGlyphCursor(Display *dpy, Font src_font, Font mask_font,
                          unsigned int src_ch, unsigned int mask_ch,
                          _Xconst XColor *fg, _Xconst XColor *bg) {
    (void)dpy; (void)src_font; (void)mask_font;
    (void)mask_ch; (void)fg; (void)bg;
    return (Cursor)(EMX11_FONT_CURSOR_TAG | (src_ch & 0xFFFu));
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
    (void)event_mask; (void)pointer_mode; (void)keyboard_mode;
    (void)confine_to; (void)t;
    dpy->request++;
    /* Reset the C-side implicit grab before installing the active one.
     * The ButtonPress that triggered this popup left grab_window pointing
     * at the original button; without clearing it, subsequent ButtonRelease
     * events on popup entries (MenuButton items, ComboBox list) get
     * delivered to the stale grab_window instead of the entry. */
    emx11_reset_implicit_grab();
    /* Honor the cursor argument: while a grab is active the canvas
     * pointer should display this cursor everywhere, overriding
     * per-window XDefineCursor. twm uses MoveCursor/ResizeCursor here. */
    emx11_js_set_grab_cursor((unsigned int)cursor);
    /* Active grab: redirect every subsequent button/motion event to the
     * calling client until XUngrabPointer. Required for twm's
     * DeferExecution (menus.c:2205) -- f.iconify, f.move, f.resize,
     * f.focus, f.delete, etc. when invoked from a root menu install
     * an active grab on Scr->Root and expect the next button press
     * anywhere on the screen to come back to twm so it can apply the
     * deferred function to the clicked window. Without this, the
     * follow-up click goes to the clicked client's queue and the
     * menu item silently does nothing. */
    emx11_js_grab_pointer((unsigned int)dpy->conn_id,
                          grab_window, owner_events ? 1 : 0);
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
    emx11_js_window_set_border(w, win->border_width, win->border_pixel);
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

/* -- Keyboard grab — same rationale as XGrabPointer: we have no real
 * input routing that would steal events from other windows, so reporting
 * GrabSuccess is truthful. */

int XGrabKeyboard(Display *dpy, Window grab_window, Bool owner_events,
                  int pointer_mode, int keyboard_mode, Time t) {
    (void)dpy; (void)grab_window; (void)owner_events;
    (void)pointer_mode; (void)keyboard_mode; (void)t;
    return GrabSuccess;
}

/* -- XNoOp: a server round-trip with no side effect. In em-x11 there is
 * no server, so this is genuinely a no-op. */

int XNoOp(Display *dpy) {
    (void)dpy;
    return 1;
}

/* -- XListHosts: browser has no access-control list. Return empty list. */

XHostAddress *XListHosts(Display *dpy, int *nhosts_return, Bool *state_return) {
    (void)dpy;
    if (nhosts_return) *nhosts_return = 0;
    if (state_return)  *state_return  = False;
    return NULL;
}
