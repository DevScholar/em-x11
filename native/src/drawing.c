#include "emx11_internal.h"

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

int XClearWindow(Display *display, Window w) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    emx11_js_fill_rect(w, 0, 0, win->width, win->height, win->background_pixel);
    return 1;
}
