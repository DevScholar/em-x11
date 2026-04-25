#include "emx11_internal.h"

#include <stdlib.h>

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

    if (values) {
        if (valuemask & GCForeground) gc->foreground = values->foreground;
        if (valuemask & GCBackground) gc->background = values->background;
        if (valuemask & GCLineWidth)  gc->line_width = values->line_width;
        if (valuemask & GCLineStyle)  gc->line_style = values->line_style;
        if (valuemask & GCFillStyle)  gc->fill_style = values->fill_style;
    }
    return gc;
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
