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

/* Full XCreateWindow with attributes -- Xt calls this directly from
 * XtRealizeWidget rather than the simple form. The attributes struct
 * lets the caller override background, border, event mask, and
 * override_redirect; visual/depth we ignore (single-visual world). */
Window XCreateWindow(Display *display, Window parent,
                     int x, int y,
                     unsigned int width, unsigned int height,
                     unsigned int border_width,
                     int depth, unsigned int class_,
                     Visual *visual, unsigned long valuemask,
                     XSetWindowAttributes *attrs) {
    (void)depth; (void)class_; (void)visual;

    unsigned long bg = 0x00000000UL;
    unsigned long bd = 0x00000000UL;
    if (attrs && (valuemask & CWBackPixel))   bg = attrs->background_pixel;
    if (attrs && (valuemask & CWBorderPixel)) bd = attrs->border_pixel;

    Window w = XCreateSimpleWindow(display, parent, x, y, width, height,
                                   border_width, bd, bg);
    if (w == None || !attrs) return w;

    /* Apply any remaining attribute bits via the common setter. */
    XChangeWindowAttributes(display, w, valuemask, attrs);
    return w;
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
    emx11_window_free_properties(win);

    win->in_use = false;
    emx11_js_window_destroy(w);
    return 1;
}

/* -- Geometry changes. The JS-side compositor needs to learn about
 *    every size/position change so it can redraw, so we push through
 *    emx11_js_window_create when a window is logically "re-set". */

static void notify_js_reconfigure(EmxWindow *win) {
    /* No dedicated "reconfigure" bridge yet; the compositor's addWindow
     * already replaces any previous entry by id, so we reuse it. */
    emx11_js_window_create(win->id, win->x, win->y,
                           win->width, win->height,
                           win->background_pixel);
    if (win->mapped) emx11_js_window_map(win->id);
}

int XMoveWindow(Display *display, Window w, int x, int y) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    win->x = x;
    win->y = y;
    notify_js_reconfigure(win);
    return 1;
}

int XResizeWindow(Display *display, Window w,
                  unsigned int width, unsigned int height) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    win->width  = width;
    win->height = height;
    notify_js_reconfigure(win);
    return 1;
}

int XMoveResizeWindow(Display *display, Window w, int x, int y,
                      unsigned int width, unsigned int height) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    win->x = x;
    win->y = y;
    win->width  = width;
    win->height = height;
    notify_js_reconfigure(win);
    return 1;
}

int XConfigureWindow(Display *display, Window w,
                     unsigned int valuemask, XWindowChanges *values) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win || !values) return 0;
    if (valuemask & CWX)           win->x = values->x;
    if (valuemask & CWY)           win->y = values->y;
    if (valuemask & CWWidth)       win->width  = (unsigned int)values->width;
    if (valuemask & CWHeight)      win->height = (unsigned int)values->height;
    if (valuemask & CWBorderWidth) win->border_width = (unsigned int)values->border_width;
    /* CWStackMode / CWSibling: z-order management not yet implemented. */
    notify_js_reconfigure(win);
    return 1;
}

int XRaiseWindow(Display *display, Window w) {
    (void)display; (void)w;
    /* Z-order: no-op. v1's compositor paints in insertion order. */
    return 1;
}

int XLowerWindow(Display *display, Window w) {
    (void)display; (void)w;
    return 1;
}

int XChangeWindowAttributes(Display *display, Window w,
                            unsigned long valuemask,
                            XSetWindowAttributes *attrs) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win || !attrs) return 0;
    if (valuemask & CWBackPixel)        win->background_pixel = attrs->background_pixel;
    if (valuemask & CWBorderPixel)      win->border_pixel     = attrs->border_pixel;
    if (valuemask & CWEventMask)        win->event_mask       = attrs->event_mask;
    if (valuemask & CWOverrideRedirect) win->override_redirect = attrs->override_redirect;
    /* Ignored: CWBackPixmap, CWBorderPixmap, CWCursor, ... until Pixmap lands. */
    return 1;
}

int XSetWindowBackground(Display *display, Window w, unsigned long background) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    win->background_pixel = background;
    return 1;
}

Status XGetWindowAttributes(Display *display, Window w,
                            XWindowAttributes *attrs_return) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win || !attrs_return) return 0;
    memset(attrs_return, 0, sizeof(*attrs_return));
    attrs_return->x                = win->x;
    attrs_return->y                = win->y;
    attrs_return->width            = (int)win->width;
    attrs_return->height           = (int)win->height;
    attrs_return->border_width     = (int)win->border_width;
    attrs_return->depth            = display->screens[0].root_depth;
    attrs_return->visual           = display->screens[0].root_visual;
    attrs_return->root             = display->screens[0].root;
    attrs_return->class            = InputOutput;
    attrs_return->bit_gravity      = ForgetGravity;
    attrs_return->win_gravity      = NorthWestGravity;
    attrs_return->backing_store    = NotUseful;
    attrs_return->save_under       = False;
    attrs_return->colormap         = display->screens[0].cmap;
    attrs_return->map_installed    = True;
    attrs_return->map_state        = win->mapped ? IsViewable : IsUnmapped;
    attrs_return->all_event_masks  = win->event_mask;
    attrs_return->your_event_mask  = win->event_mask;
    attrs_return->do_not_propagate_mask = 0;
    attrs_return->override_redirect = win->override_redirect;
    attrs_return->screen           = &display->screens[0];
    return 1;
}

Status XGetGeometry(Display *display, Drawable d,
                    Window *root_return, int *x_return, int *y_return,
                    unsigned int *width_return, unsigned int *height_return,
                    unsigned int *border_width_return,
                    unsigned int *depth_return) {
    EmxWindow *win = emx11_window_find(display, (Window)d);
    if (!win) return 0;
    if (root_return)         *root_return         = display->screens[0].root;
    if (x_return)            *x_return            = win->x;
    if (y_return)            *y_return            = win->y;
    if (width_return)        *width_return        = win->width;
    if (height_return)       *height_return       = win->height;
    if (border_width_return) *border_width_return = win->border_width;
    if (depth_return)        *depth_return        = (unsigned int)display->screens[0].root_depth;
    return 1;
}

Bool XTranslateCoordinates(Display *display, Window src_w, Window dest_w,
                           int src_x, int src_y,
                           int *dest_x_return, int *dest_y_return,
                           Window *child_return) {
    EmxWindow *src = emx11_window_find(display, src_w);
    EmxWindow *dst = emx11_window_find(display, dest_w);
    if (!src || !dst) return False;
    /* Windows are children of the root in our flat model: adding src pos
     * and subtracting dst pos is enough. */
    if (dest_x_return) *dest_x_return = src->x + src_x - dst->x;
    if (dest_y_return) *dest_y_return = src->y + src_y - dst->y;
    if (child_return)  *child_return  = None;
    return True;
}

Status XQueryTree(Display *display, Window w,
                  Window *root_return, Window *parent_return,
                  Window **children_return, unsigned int *nchildren_return) {
    (void)w;
    if (root_return)      *root_return      = display->screens[0].root;
    if (parent_return)    *parent_return    = None;
    if (children_return)  *children_return  = NULL;
    if (nchildren_return) *nchildren_return = 0;
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
