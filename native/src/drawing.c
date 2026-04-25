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
    emx11_js_fill_rect(w, 0, 0, win->width, win->height, win->background_pixel);
    return 1;
}

int XClearArea(Display *display, Window w,
               int x, int y, unsigned int width, unsigned int height,
               Bool exposures) {
    (void)exposures;
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    if (width == 0)  width  = win->width  - (unsigned int)x;
    if (height == 0) height = win->height - (unsigned int)y;
    emx11_js_fill_rect(w, x, y, width, height, win->background_pixel);
    return 1;
}
