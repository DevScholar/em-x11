/*
 * em-x11 SHAPE extension -- client-side implementation.
 *
 * Upstream libXext marshals these calls into X protocol requests and
 * sends them to the server. em-x11 has no server: we store the shape
 * data directly on the EmxWindow record and push it to the JS
 * compositor, which clips rendering to the shape.
 *
 * v1 scope:
 *   - ShapeBounding fully supported via XShapeCombineRectangles (ShapeSet
 *     and ShapeUnion). This is what xeyes needs.
 *   - ShapeClip / ShapeInput accepted but recorded nowhere (TODO).
 *   - XShapeCombineMask is a stub -- depends on Pixmap infrastructure
 *     that em-x11 has not yet introduced.
 */

#include "../emx11_internal.h"

#include <X11/Xutil.h>
#include <X11/extensions/shape.h>

#include <stdlib.h>
#include <string.h>

#define EMX11_SHAPE_EVENT_BASE 64
#define EMX11_SHAPE_ERROR_BASE 128

/* -- Extension query ------------------------------------------------------- */

Bool XShapeQueryExtension(Display *dpy, int *event_base, int *error_base) {
    (void)dpy;
    if (event_base) *event_base = EMX11_SHAPE_EVENT_BASE;
    if (error_base) *error_base = EMX11_SHAPE_ERROR_BASE;
    return True;
}

Status XShapeQueryVersion(Display *dpy, int *major, int *minor) {
    (void)dpy;
    if (major) *major = SHAPE_MAJOR_VERSION;
    if (minor) *minor = SHAPE_MINOR_VERSION;
    return 1;
}

/* -- Helpers --------------------------------------------------------------- */

static void push_shape_to_js(EmxWindow *win) {
    if (!win) return;
    if (win->shape_bounding_count == 0 || !win->shape_bounding) {
        emx11_js_window_shape(win->id, NULL, 0);
        return;
    }

    /* Flatten XRectangle[] into an int[] (x, y, w, h per rect). The JS
     * bridge reads plain ints -- it doesn't know XRectangle's layout. */
    int *flat = malloc(sizeof(int) * 4 * win->shape_bounding_count);
    if (!flat) return;
    for (int i = 0; i < win->shape_bounding_count; i++) {
        flat[i * 4 + 0] = win->shape_bounding[i].x;
        flat[i * 4 + 1] = win->shape_bounding[i].y;
        flat[i * 4 + 2] = win->shape_bounding[i].width;
        flat[i * 4 + 3] = win->shape_bounding[i].height;
    }
    emx11_js_window_shape(win->id, flat, win->shape_bounding_count);
    free(flat);
}

static void clear_shape(EmxWindow *win) {
    if (!win) return;
    free(win->shape_bounding);
    win->shape_bounding = NULL;
    win->shape_bounding_count = 0;
}

static void set_shape(EmxWindow *win, XRectangle *rects, int n) {
    if (!win) return;
    clear_shape(win);
    if (n <= 0 || !rects) return;
    win->shape_bounding = malloc(sizeof(XRectangle) * (size_t)n);
    if (!win->shape_bounding) return;
    memcpy(win->shape_bounding, rects, sizeof(XRectangle) * (size_t)n);
    win->shape_bounding_count = n;
}

static void union_shape(EmxWindow *win, XRectangle *rects, int n) {
    if (!win || n <= 0 || !rects) return;
    int total = win->shape_bounding_count + n;
    XRectangle *merged = malloc(sizeof(XRectangle) * (size_t)total);
    if (!merged) return;
    if (win->shape_bounding_count > 0) {
        memcpy(merged, win->shape_bounding,
               sizeof(XRectangle) * (size_t)win->shape_bounding_count);
    }
    memcpy(merged + win->shape_bounding_count, rects,
           sizeof(XRectangle) * (size_t)n);
    free(win->shape_bounding);
    win->shape_bounding = merged;
    win->shape_bounding_count = total;
}

/* -- Primary entry points -------------------------------------------------- */

void XShapeCombineRectangles(Display *dpy, Window dest, int dest_kind,
                             int x_off, int y_off,
                             XRectangle *rects, int n_rects,
                             int op, int ordering) {
    (void)ordering;

    EmxWindow *win = emx11_window_find(dpy, dest);
    if (!win) return;

    /* ShapeClip and ShapeInput are accepted but not yet wired into rendering
     * or hit testing. xeyes only uses ShapeBounding. */
    if (dest_kind != ShapeBounding) return;

    /* Apply x/y offset by copying the input. */
    XRectangle *offset = NULL;
    if (n_rects > 0 && rects && (x_off != 0 || y_off != 0)) {
        offset = malloc(sizeof(XRectangle) * (size_t)n_rects);
        if (!offset) return;
        for (int i = 0; i < n_rects; i++) {
            offset[i].x      = (short)(rects[i].x + x_off);
            offset[i].y      = (short)(rects[i].y + y_off);
            offset[i].width  = rects[i].width;
            offset[i].height = rects[i].height;
        }
        rects = offset;
    }

    switch (op) {
    case ShapeSet:
        set_shape(win, rects, n_rects);
        break;
    case ShapeUnion:
        union_shape(win, rects, n_rects);
        break;
    case ShapeIntersect:
    case ShapeSubtract:
    case ShapeInvert:
        /* TODO: proper region algebra. For now treat as replace so at
         * least callers don't silently fail. */
        set_shape(win, rects, n_rects);
        break;
    default:
        break;
    }

    free(offset);
    push_shape_to_js(win);
}

void XShapeCombineMask(Display *dpy, Window dest, int dest_kind,
                      int x_off, int y_off, Pixmap src, int op) {
    /* Pixmaps are not yet implemented in em-x11, so we have no way to
     * decode the bitmap into a shape. Accepted as a no-op for now; the
     * window remains rectangular. xeyes typically uses this, so this
     * is the primary TODO blocking xeyes visual correctness. */
    (void)dpy; (void)dest; (void)dest_kind;
    (void)x_off; (void)y_off; (void)src; (void)op;
}

void XShapeCombineShape(Display *dpy, Window dest, int dest_kind,
                       int x_off, int y_off, Window src, int src_kind, int op) {
    /* Copy shape from another window. */
    (void)src_kind;
    EmxWindow *src_win = emx11_window_find(dpy, src);
    if (!src_win || src_win->shape_bounding_count == 0) return;
    XShapeCombineRectangles(dpy, dest, dest_kind, x_off, y_off,
                            src_win->shape_bounding,
                            src_win->shape_bounding_count, op, Unsorted);
}

void XShapeCombineRegion(Display *dpy, Window dest, int dest_kind,
                        int x_off, int y_off, Region region, int op) {
    /* Region is an opaque Xlib type -- its internals live in Xregion.h,
     * which we did not copy. We cannot walk the region without that
     * header. TODO once Region support is added to em-x11. */
    (void)dpy; (void)dest; (void)dest_kind;
    (void)x_off; (void)y_off; (void)region; (void)op;
}

void XShapeOffsetShape(Display *dpy, Window dest, int dest_kind,
                      int x_off, int y_off) {
    EmxWindow *win = emx11_window_find(dpy, dest);
    if (!win || dest_kind != ShapeBounding) return;
    for (int i = 0; i < win->shape_bounding_count; i++) {
        win->shape_bounding[i].x = (short)(win->shape_bounding[i].x + x_off);
        win->shape_bounding[i].y = (short)(win->shape_bounding[i].y + y_off);
    }
    push_shape_to_js(win);
}

Status XShapeQueryExtents(Display *dpy, Window window,
                         Bool *bounding_shaped,
                         int *x_bounding, int *y_bounding,
                         unsigned int *w_bounding, unsigned int *h_bounding,
                         Bool *clip_shaped,
                         int *x_clip, int *y_clip,
                         unsigned int *w_clip, unsigned int *h_clip) {
    EmxWindow *win = emx11_window_find(dpy, window);
    if (!win) return 0;

    if (bounding_shaped) *bounding_shaped = (win->shape_bounding != NULL);
    if (x_bounding) *x_bounding = 0;
    if (y_bounding) *y_bounding = 0;
    if (w_bounding) *w_bounding = win->width;
    if (h_bounding) *h_bounding = win->height;

    if (clip_shaped) *clip_shaped = False;
    if (x_clip) *x_clip = 0;
    if (y_clip) *y_clip = 0;
    if (w_clip) *w_clip = win->width;
    if (h_clip) *h_clip = win->height;
    return 1;
}

void XShapeSelectInput(Display *dpy, Window window, unsigned long mask) {
    /* ShapeNotify events -- not generated by em-x11 yet. Accepted as a
     * no-op; real apps only need this if they observe other clients'
     * shape changes, which our single-client model doesn't surface. */
    (void)dpy; (void)window; (void)mask;
}

unsigned long XShapeInputSelected(Display *dpy, Window window) {
    (void)dpy; (void)window;
    return 0;
}

XRectangle *XShapeGetRectangles(Display *dpy, Window window, int kind,
                               int *count, int *ordering) {
    EmxWindow *win = emx11_window_find(dpy, window);
    if (ordering) *ordering = Unsorted;
    if (!win || kind != ShapeBounding || win->shape_bounding_count == 0) {
        if (count) *count = 0;
        return NULL;
    }
    int n = win->shape_bounding_count;
    XRectangle *copy = malloc(sizeof(XRectangle) * (size_t)n);
    if (!copy) {
        if (count) *count = 0;
        return NULL;
    }
    memcpy(copy, win->shape_bounding, sizeof(XRectangle) * (size_t)n);
    if (count) *count = n;
    return copy;
}
