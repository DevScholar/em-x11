/*
 * em-x11 XRender stubs.
 *
 * Real XRender protocol isn't implemented — em-x11's text path is
 * canvas.fillText, not glyph composition — so anything XRender clients
 * would normally talk to a real RENDER extension for is short-circuited
 * here. Tk doesn't invoke any of these directly (Xft owns the surface),
 * but Xt-side / future consumers that probe for RENDER expect the
 * extension to "exist" and version-report cleanly.
 */

#include "emx11_internal.h"
#include <X11/extensions/Xrender.h>

#include <stdlib.h>
#include <string.h>

Bool XRenderQueryExtension(Display *dpy, int *event_base, int *error_base) {
    (void)dpy;
    if (event_base) *event_base = 64;
    if (error_base) *error_base = 128;
    return True;
}

Status XRenderQueryVersion(Display *dpy, int *major, int *minor) {
    (void)dpy;
    if (major) *major = 0;
    if (minor) *minor = 11;
    return 1;
}

static XRenderPictFormat g_argb32 = {
    /* id     */ 1,
    /* type   */ PictTypeDirect,
    /* depth  */ 32,
    /* direct */ { 16, 0xFF, 8, 0xFF, 0, 0xFF, 24, 0xFF },
    /* cmap   */ 0,
};
static XRenderPictFormat g_rgb24 = {
    2, PictTypeDirect, 24, { 16, 0xFF, 8, 0xFF, 0, 0xFF, 0, 0 }, 0,
};

XRenderPictFormat *XRenderFindVisualFormat(Display *dpy, _Xconst Visual *visual) {
    (void)dpy; (void)visual;
    return &g_rgb24;
}

XRenderPictFormat *XRenderFindStandardFormat(Display *dpy, int format) {
    (void)dpy;
    switch (format) {
    case PictStandardARGB32: return &g_argb32;
    case PictStandardRGB24:  return &g_rgb24;
    default:                 return &g_rgb24;
    }
}

Picture XRenderCreatePicture(Display *dpy, Drawable d,
                             _Xconst XRenderPictFormat *format,
                             unsigned long valuemask,
                             _Xconst XRenderPictureAttributes *attrs) {
    (void)dpy; (void)format; (void)valuemask; (void)attrs;
    /* Picture id == Drawable id keeps composite ops trivially routable
     * through the existing copy-area / fill-rect bridges. */
    return (Picture)d;
}

void XRenderFreePicture(Display *dpy, Picture p) { (void)dpy; (void)p; }

void XRenderComposite(Display *dpy, int op,
                      Picture src, Picture mask, Picture dst,
                      int sx, int sy, int mx, int my,
                      int dx, int dy, unsigned int w, unsigned int h) {
    (void)op; (void)mask; (void)mx; (void)my;
    /* Best-effort fallback to plain XCopyArea when source and dest are
     * both real drawables. Anything fancier (PictOpOver alpha blend,
     * mask compositing) is silently dropped. */
    if (!src || !dst) return;
    GC gc = XCreateGC(dpy, (Drawable)dst, 0, NULL);
    if (gc) {
        XCopyArea(dpy, (Drawable)src, (Drawable)dst, gc, sx, sy, w, h, dx, dy);
        XFreeGC(dpy, gc);
    }
}

void XRenderFillRectangle(Display *dpy, int op, Picture dst,
                          _Xconst XRenderColor *color,
                          int x, int y, unsigned int w, unsigned int h) {
    (void)op;
    if (!dst || !color) return;
    XGCValues vals;
    unsigned int r = (color->red   >> 8) & 0xFF;
    unsigned int g = (color->green >> 8) & 0xFF;
    unsigned int b = (color->blue  >> 8) & 0xFF;
    vals.foreground = (r << 16) | (g << 8) | b;
    GC gc = XCreateGC(dpy, (Drawable)dst, GCForeground, &vals);
    if (gc) {
        XFillRectangle(dpy, (Drawable)dst, gc, x, y, w, h);
        XFreeGC(dpy, gc);
    }
}
