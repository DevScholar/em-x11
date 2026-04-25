#include "emx11_internal.h"

#include <stdlib.h>
#include <string.h>

Window XCreateSimpleWindow(Display *display, Window parent,
                           int x, int y,
                           unsigned int width, unsigned int height,
                           unsigned int border_width,
                           unsigned long border, unsigned long background) {
    EmxWindow *w = emx11_window_alloc(display);
    if (!w) {
        return None;
    }

    w->id               = emx11_next_xid(display);
    w->parent           = parent;
    w->x                = x;
    w->y                = y;
    w->width            = width;
    w->height           = height;
    w->border_width     = border_width;
    w->border_pixel     = border;
    w->background_pixel = background;
    w->mapped           = false;

    emx11_js_window_create(w->id, x, y, width, height, background);
    return w->id;
}

int XMapWindow(Display *display, Window w) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) {
        return 0;
    }
    win->mapped = true;
    emx11_js_window_map(w);

    /* X semantics: newly-mapped windows receive an Expose event so clients
     * know to paint themselves for the first time. */
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xexpose.type    = Expose;
    ev.xexpose.display = display;
    ev.xexpose.window  = w;
    ev.xexpose.x       = 0;
    ev.xexpose.y       = 0;
    ev.xexpose.width   = (int)win->width;
    ev.xexpose.height  = (int)win->height;
    ev.xexpose.count   = 0;
    emx11_event_queue_push(display, &ev);
    return 1;
}

int XUnmapWindow(Display *display, Window w) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) {
        return 0;
    }
    win->mapped = false;
    emx11_js_window_unmap(w);
    return 1;
}

int XDestroyWindow(Display *display, Window w) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) {
        return 0;
    }
    /* Release SHAPE storage attached to this window. */
    free(win->shape_bounding);
    win->shape_bounding = NULL;
    win->shape_bounding_count = 0;

    win->in_use = false;
    emx11_js_window_destroy(w);
    return 1;
}

int XSelectInput(Display *display, Window w, long event_mask) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) {
        return 0;
    }
    win->event_mask = event_mask;
    return 1;
}

int XStoreName(Display *display, Window w, const char *name) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win || !name) {
        return 0;
    }
    strncpy(win->name, name, sizeof(win->name) - 1);
    win->name[sizeof(win->name) - 1] = '\0';
    return 1;
}
