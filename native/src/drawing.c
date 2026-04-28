#include "emx11_internal.h"

#include <stdlib.h>
#include <string.h>

int XFillRectangle(Display *display, Drawable d, GC gc,
                   int x, int y, unsigned int width, unsigned int height) {
    (void)display;
    if (!gc) return 0;
    emx11_js_fill_rect((Window)d, x, y, width, height, gc->foreground);
    return 1;
}

int XDrawRectangle(Display *display, Drawable d, GC gc,
                   int x, int y, unsigned int width, unsigned int height) {
    (void)display;
    if (!gc) return 0;
    int x2 = x + (int)width;
    int y2 = y + (int)height;
    emx11_js_draw_line((Window)d, x,  y,  x2, y,  gc->foreground, gc->line_width);
    emx11_js_draw_line((Window)d, x2, y,  x2, y2, gc->foreground, gc->line_width);
    emx11_js_draw_line((Window)d, x2, y2, x,  y2, gc->foreground, gc->line_width);
    emx11_js_draw_line((Window)d, x,  y2, x,  y,  gc->foreground, gc->line_width);
    return 1;
}

int XDrawLine(Display *display, Drawable d, GC gc,
              int x1, int y1, int x2, int y2) {
    (void)display;
    if (!gc) return 0;
    emx11_js_draw_line((Window)d, x1, y1, x2, y2, gc->foreground, gc->line_width);
    return 1;
}

int XDrawLines(Display *display, Drawable d, GC gc,
               XPoint *points, int npoints, int mode) {
    (void)display;
    if (!gc || !points || npoints <= 0) return 0;
    int cx = points[0].x;
    int cy = points[0].y;
    for (int i = 1; i < npoints; i++) {
        int nx, ny;
        if (mode == CoordModePrevious) {
            nx = cx + points[i].x;
            ny = cy + points[i].y;
        } else {
            nx = points[i].x;
            ny = points[i].y;
        }
        emx11_js_draw_line((Window)d, cx, cy, nx, ny,
                           gc->foreground, gc->line_width);
        cx = nx;
        cy = ny;
    }
    return 1;
}

int XDrawSegments(Display *display, Drawable d, GC gc,
                  XSegment *segments, int nsegments) {
    (void)display;
    if (!gc || !segments) return 0;
    for (int i = 0; i < nsegments; i++) {
        emx11_js_draw_line((Window)d,
                           segments[i].x1, segments[i].y1,
                           segments[i].x2, segments[i].y2,
                           gc->foreground, gc->line_width);
    }
    return 1;
}

int XDrawPoint(Display *display, Drawable d, GC gc, int x, int y) {
    (void)display;
    if (!gc) return 0;
    /* A single point is a 1x1 fill in Xlib semantics. */
    emx11_js_fill_rect((Window)d, x, y, 1, 1, gc->foreground);
    return 1;
}

/* Serialize XPoint[] (with mode resolution) into a flat int[] and push. */
static int *flatten_points(XPoint *points, int npoints, int mode,
                           int *out_count) {
    if (npoints <= 0 || !points) {
        *out_count = 0;
        return NULL;
    }
    int *flat = malloc(sizeof(int) * 2 * (size_t)npoints);
    if (!flat) {
        *out_count = 0;
        return NULL;
    }
    int cx = 0, cy = 0;
    for (int i = 0; i < npoints; i++) {
        if (mode == CoordModePrevious && i > 0) {
            cx += points[i].x;
            cy += points[i].y;
        } else {
            cx = points[i].x;
            cy = points[i].y;
        }
        flat[i * 2 + 0] = cx;
        flat[i * 2 + 1] = cy;
    }
    *out_count = npoints;
    return flat;
}

int XDrawPoints(Display *display, Drawable d, GC gc,
                XPoint *points, int npoints, int mode) {
    (void)display;
    if (!gc) return 0;
    int count = 0;
    int *flat = flatten_points(points, npoints, mode, &count);
    if (!flat) return 0;
    emx11_js_draw_points((Window)d, flat, count,
                         CoordModeOrigin, gc->foreground);
    free(flat);
    return 1;
}

int XDrawArc(Display *display, Drawable d, GC gc,
             int x, int y, unsigned int width, unsigned int height,
             int angle1, int angle2) {
    (void)display;
    if (!gc) return 0;
    emx11_js_draw_arc((Window)d, x, y, width, height,
                      angle1, angle2, gc->foreground, gc->line_width);
    return 1;
}

int XFillArc(Display *display, Drawable d, GC gc,
             int x, int y, unsigned int width, unsigned int height,
             int angle1, int angle2) {
    (void)display;
    if (!gc) return 0;
    emx11_js_fill_arc((Window)d, x, y, width, height,
                      angle1, angle2, gc->foreground);
    return 1;
}

int XFillPolygon(Display *display, Drawable d, GC gc,
                 XPoint *points, int npoints, int shape, int mode) {
    (void)display;
    if (!gc) return 0;
    int count = 0;
    int *flat = flatten_points(points, npoints, mode, &count);
    if (!flat) return 0;
    emx11_js_fill_polygon((Window)d, flat, count, shape, CoordModeOrigin,
                          gc->foreground);
    free(flat);
    return 1;
}

int XClearWindow(Display *display, Window w) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    /* Routed through a bg-aware bridge so the compositor paints with the
     * window's background_pixmap (if any) instead of the solid pixel.
     * The pixel remains the fallback when no pixmap is bound. */
    emx11_js_clear_area(w, 0, 0, win->width, win->height);
    return 1;
}

int XClearArea(Display *display, Window w,
               int x, int y, unsigned int width, unsigned int height,
               Bool exposures) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    if (width == 0)  width  = win->width  - (unsigned int)x;
    if (height == 0) height = win->height - (unsigned int)y;
    emx11_js_clear_area(w, x, y, width, height);
    /* x11protocol.txt §1064 / XClearArea(3): when `exposures` is True,
     * the server generates Expose events for the cleared region. Xt's
     * SetValues.c:441 relies on this (XClearArea(..., TRUE)) to get a
     * redisplay of any widget whose resources changed in-place -- e.g.
     * XawCommandToggle swapping fg/bg on button press, or XCalc setting
     * the LCD label string. Without the Expose, the widget's bg is
     * refreshed to the new colour but its content (label text, border)
     * is never repainted until some other event (Leave, Map, ...)
     * happens to trigger its expose proc. */
    if (exposures) {
        XEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type             = Expose;
        ev.xexpose.display  = display;
        ev.xexpose.window   = w;
        ev.xexpose.x        = x;
        ev.xexpose.y        = y;
        ev.xexpose.width    = (int)width;
        ev.xexpose.height   = (int)height;
        ev.xexpose.count    = 0;
        emx11_event_queue_push(display, &ev);
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/*  Drawable-to-drawable copy: XCopyArea / XCopyPlane / XPutImage.           */
/*                                                                           */
/*  Routing happens JS-side in Host.onCopy* / onPutImage -- it decides       */
/*  "is this id a pixmap or a window" for each endpoint. The C side just     */
/*  flattens Xlib args across the bridge. GC fields we use are foreground,   */
/*  background (XCopyPlane, XPutImage bitmap path). gc->function and         */
/*  plane_mask aren't plumbed through our simplified GC struct; we behave    */
/*  as if they're GXcopy + AllPlanes, matching what Xaw/Tk expect.           */
/* ------------------------------------------------------------------------- */

int XCopyArea(Display *dpy, Drawable src, Drawable dst, GC gc,
              int src_x, int src_y, unsigned int width, unsigned int height,
              int dst_x, int dst_y) {
    (void)dpy; (void)gc;
    if (width == 0 || height == 0) return 1;
    emx11_js_copy_area(src, dst, src_x, src_y, width, height, dst_x, dst_y);
    return 1;
}

int XCopyPlane(Display *dpy, Drawable src, Drawable dst, GC gc,
               int src_x, int src_y, unsigned int width, unsigned int height,
               int dst_x, int dst_y, unsigned long plane) {
    (void)dpy;
    if (!gc || width == 0 || height == 0) return 1;
    /* apply_bg: "paint unset bits with gc->background". In X semantics
     * this depends on gc->function (GXcopy) and whether the client wants
     * opaque stipple behaviour. Without gc->function in our struct, we
     * default to opaque: Xaw's Label + Tk's bitmap images both want the
     * bg painted. Clients that need transparent overlays use GXand,
     * which we don't currently expose. */
    emx11_js_copy_plane(src, dst, src_x, src_y, width, height,
                        dst_x, dst_y, plane,
                        gc->foreground, gc->background, 1);
    return 1;
}

int XPutImage(Display *dpy, Drawable d, GC gc, XImage *image,
              int src_x, int src_y, int dst_x, int dst_y,
              unsigned int w, unsigned int h) {
    (void)dpy;
    if (!gc || !image || !image->data || w == 0 || h == 0) return 1;
    /* Slice a (src_x, src_y, w, h) sub-rectangle out of the XImage and
     * hand the raw bytes to Host. We pass the full scanline stride so
     * Host can step source rows without additional copying; Host uses
     * (src_x, src_y) == (0, 0) implicitly by pointing at the right
     * offset. For simplicity here we just bump the pointer. */
    int bpl = image->bytes_per_line;
    int bpp = image->bits_per_pixel;
    const unsigned char *base = (const unsigned char *)image->data;
    int byte_offset;
    if (image->format == XYBitmap || image->depth == 1) {
        /* 1 bit per pixel; src_x may not be byte-aligned. We push the
         * whole scanline starting at byte-aligned src_x_floor and let
         * Host pick bits from column (src_x % 8) onward. For the common
         * case src_x == 0 the math collapses to "start of row". */
        byte_offset = src_y * bpl + (src_x >> 3);
    } else {
        byte_offset = src_y * bpl + src_x * (bpp >> 3);
    }
    int data_len = (int)h * bpl;
    emx11_js_put_image(d, dst_x, dst_y, w, h, image->format, image->depth,
                       bpl, base + byte_offset, data_len,
                       gc->foreground, gc->background);
    return 1;
}
