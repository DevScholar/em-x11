#include "emx11_internal.h"

#include <stdlib.h>
#include <string.h>

static void apply_values(GC gc, unsigned long valuemask, XGCValues *values) {
    if (!gc || !values) return;
    if (valuemask & GCFunction)     gc->function = values->function;
    if (valuemask & GCPlaneMask)   { /* ignored */ }
    if (valuemask & GCForeground)   gc->foreground = values->foreground;
    if (valuemask & GCBackground)   gc->background = values->background;
    if (valuemask & GCLineWidth)    gc->line_width = values->line_width;
    if (valuemask & GCLineStyle)    gc->line_style = values->line_style;
    if (valuemask & GCFillStyle)    gc->fill_style = values->fill_style;
    if (valuemask & GCFont)         gc->font       = values->font;
}

GC XCreateGC(Display *display, Drawable d,
             unsigned long valuemask, XGCValues *values) {
    (void)display;
    (void)d;

    struct _XGC *gc = calloc(1, sizeof(struct _XGC));
    if (!gc) {
        return NULL;
    }
    gc->foreground = 0x00000000UL;
    gc->background = 0x00FFFFFFUL;
    gc->line_width = 0;
    gc->line_style = LineSolid;
    gc->fill_style = FillSolid;
    gc->function   = GXcopy;
    gc->font       = None;

    apply_values(gc, valuemask, values);
    return gc;
}

int XChangeGC(Display *display, GC gc, unsigned long valuemask, XGCValues *values) {
    (void)display;
    apply_values(gc, valuemask, values);
    return 1;
}

int XCopyGC(Display *display, GC src, unsigned long valuemask, GC dst) {
    (void)display;
    if (!src || !dst) return 0;
    XGCValues v;
    v.foreground = src->foreground;
    v.background = src->background;
    v.line_width = src->line_width;
    v.line_style = src->line_style;
    v.fill_style = src->fill_style;
    v.function   = src->function;
    v.font       = src->font;
    apply_values(dst, valuemask, &v);
    return 1;
}

int XGetGCValues(Display *display, GC gc,
                 unsigned long valuemask, XGCValues *values_return) {
    (void)display;
    if (!gc || !values_return) return 0;
    if (valuemask & GCForeground) values_return->foreground = gc->foreground;
    if (valuemask & GCBackground) values_return->background = gc->background;
    if (valuemask & GCLineWidth)  values_return->line_width = gc->line_width;
    if (valuemask & GCLineStyle)  values_return->line_style = gc->line_style;
    if (valuemask & GCFillStyle)  values_return->fill_style = gc->fill_style;
    if (valuemask & GCFunction)   values_return->function   = gc->function;
    if (valuemask & GCFont)       values_return->font       = gc->font;
    return 1;
}

int XFreeGC(Display *display, GC gc) {
    (void)display;
    free(gc);
    return 1;
}

int XSetForeground(Display *display, GC gc, unsigned long foreground) {
    (void)display;
    if (!gc) return 0;
    gc->foreground = foreground;
    return 1;
}

int XSetBackground(Display *display, GC gc, unsigned long background) {
    (void)display;
    if (!gc) return 0;
    gc->background = background;
    return 1;
}

int XSetLineAttributes(Display *display, GC gc,
                       unsigned int line_width, int line_style,
                       int cap_style, int join_style) {
    (void)display; (void)cap_style; (void)join_style;
    if (!gc) return 0;
    gc->line_width = (int)line_width;
    gc->line_style = line_style;
    return 1;
}

int XSetFillStyle(Display *display, GC gc, int fill_style) {
    (void)display;
    if (!gc) return 0;
    gc->fill_style = fill_style;
    return 1;
}

int XSetFunction(Display *display, GC gc, int function) {
    (void)display;
    if (!gc) return 0;
    /* Canvas 2D has no logical-op concept beyond GXcopy. We track the
     * value so drawing primitives can short-circuit non-copy modes
     * (XOR rubber-band etc.) rather than overdraw destructively. See
     * drawing.c::gc_draw_disabled. */
    gc->function = function;
    return 1;
}

/* --- Clipping stubs ---
 *
 * Clip regions aren't yet wired into the compositor. Accepting the calls
 * and remembering nothing lets Xt/Xaw progress; widget clipping just
 * degenerates to the window bounds until we plumb this through. */

int XSetClipMask(Display *display, GC gc, Pixmap pixmap) {
    (void)display; (void)gc; (void)pixmap;
    return 1;
}

int XSetClipOrigin(Display *display, GC gc, int clip_x_origin, int clip_y_origin) {
    (void)display; (void)gc; (void)clip_x_origin; (void)clip_y_origin;
    return 1;
}

int XSetClipRectangles(Display *display, GC gc,
                       int clip_x_origin, int clip_y_origin,
                       XRectangle *rectangles, int n, int ordering) {
    (void)display; (void)gc;
    (void)clip_x_origin; (void)clip_y_origin;
    (void)rectangles; (void)n; (void)ordering;
    return 1;
}
