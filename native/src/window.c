#include "emx11_internal.h"

#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- StructureNotify synthesis ---------------------------------------- */

/* Real X server delivers MapNotify / UnmapNotify / ConfigureNotify to
 * every client that selected StructureNotifyMask on the window (and
 * SubstructureNotifyMask on its parent). Tk's Tk_MakeWindowExist /
 * TkpWmSetState / TkDoWhenIdle paths all do exactly that -- they
 * latch `ismapped` and the recorded geometry from these events rather
 * than from their own XMap/XConfigure call-site. Without synthesis,
 * Tk's widget tree stays at `ismapped=0, geometry=1x1+0+0` forever,
 * which is the symptom tk-hello was hitting.
 *
 * For single-client demos (tk-hello) the local queue is where the
 * notify has to land. When a WM is involved the cross-connection case
 * still needs solving via Host.onWindowMap / onWindowConfigure (already
 * in place for Expose); we leave those paths alone here. */
static void push_map_notify(Display *dpy, EmxWindow *win, bool mapped) {
    XEvent ev = {0};
    if (mapped) {
        ev.type = MapNotify;
        ev.xmap.serial     = 0;
        ev.xmap.send_event = False;
        ev.xmap.display    = dpy;
        ev.xmap.event      = win->id;
        ev.xmap.window     = win->id;
        ev.xmap.override_redirect = win->override_redirect ? True : False;
    } else {
        ev.type = UnmapNotify;
        ev.xunmap.serial     = 0;
        ev.xunmap.send_event = False;
        ev.xunmap.display    = dpy;
        ev.xunmap.event      = win->id;
        ev.xunmap.window     = win->id;
        ev.xunmap.from_configure = False;
    }
    emx11_event_queue_push(dpy, &ev);
}

static void push_configure_notify(Display *dpy, EmxWindow *win) {
    XEvent ev = {0};
    ev.type = ConfigureNotify;
    ev.xconfigure.serial     = 0;
    ev.xconfigure.send_event = False;
    ev.xconfigure.display    = dpy;
    ev.xconfigure.event      = win->id;
    ev.xconfigure.window     = win->id;
    ev.xconfigure.x          = win->x;
    ev.xconfigure.y          = win->y;
    ev.xconfigure.width      = (int)win->width;
    ev.xconfigure.height     = (int)win->height;
    ev.xconfigure.border_width = (int)win->border_width;
    ev.xconfigure.above      = None;
    ev.xconfigure.override_redirect = win->override_redirect ? True : False;
    emx11_event_queue_push(dpy, &ev);
}

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

    emx11_js_window_create(display->conn_id, w->id, parent,
                           x, y, width, height, background);
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
    /* Cross-connection safe: a window created by another client is legal
     * to map (the WM case -- twm calls XMapWindow on a reparented client
     * shell). We always forward to the Host; local bookkeeping only
     * fires when the caller owns a shadow entry.
     *
     * Expose synthesis lives Host-side (Host.onWindowMap) rather than
     * here so a cross-connection map reaches the actual owner of the
     * window, not the calling connection. The owner's Module ccall is
     * the only way to land an event in the owner's queue; pushing into
     * the caller's queue (what the previous local synthesis did) sent
     * Expose to the WM instead of the client whose shell just mapped.
     * Standalone clients still get their Expose -- Host queues it during
     * the initial XOpenDisplay → XMapWindow burst (when conn.module is
     * still null because launchClient hasn't resolved) and drains the
     * queue after launchClient binds the Module.
     *
     * MapNotify is a different story: it must land in the MAPPING
     * client's queue so its Tk / Xt layer can latch `TK_MAPPED` and
     * advance the realize state machine. In the single-client demo
     * case (tk-hello) the caller and owner are the same connection,
     * so a local push is exactly right. */
    EmxWindow *win = emx11_window_find(display, w);
    if (win) {
        win->mapped = true;
        push_map_notify(display, win, true);
    }
    emx11_js_window_map(display->conn_id, w);
    return 1;
}

int XUnmapWindow(Display *display, Window w) {
    EmxWindow *win = emx11_window_find(display, w);
    if (win) {
        win->mapped = false;
        push_map_notify(display, win, false);
    }
    emx11_js_window_unmap(display->conn_id, w);
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

static void notify_js_reconfigure(Display *dpy, EmxWindow *win) {
    /* Geometry-only path: window already exists on Host; just update
     * its (x, y, w, h). We deliberately avoid re-calling window_create
     * here -- doing so stomped on parent / shape / background_pixmap.
     *
     * Expose synthesis on a configured-while-mapped window lives Host-
     * side (Host.onWindowConfigure) for the same reason as XMapWindow:
     * a WM reconfiguring a managed client's shell is a cross-connection
     * call, and only the Host knows the shell's owner module to ccall.
     *
     * ConfigureNotify, on the other hand, must land in the CONFIGURED
     * client's queue so Tk can update its recorded geometry and mark
     * the widget for paint. Without this, Tk's TopLevel stays at
     * `geometry=1x1+0+0` after `pack` -- which is the tk-hello symptom. */
    emx11_js_window_configure(win->id, win->x, win->y,
                              win->width, win->height);
    push_configure_notify(dpy, win);
}

int XMoveWindow(Display *display, Window w, int x, int y) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    win->x = x;
    win->y = y;
    notify_js_reconfigure(display, win);
    return 1;
}

int XResizeWindow(Display *display, Window w,
                  unsigned int width, unsigned int height) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    win->width  = width;
    win->height = height;
    notify_js_reconfigure(display, win);
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
    notify_js_reconfigure(display, win);
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
    notify_js_reconfigure(display, win);
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
    if (valuemask & CWEventMask) {
        win->event_mask = attrs->event_mask;
        emx11_js_select_input(display->conn_id, w, attrs->event_mask);
    }
    if (valuemask & CWOverrideRedirect) {
        win->override_redirect = attrs->override_redirect;
        emx11_js_set_override_redirect(w, attrs->override_redirect ? 1 : 0);
    }
    if (valuemask & CWBackPixmap) {
        /* X semantics: ParentRelative and None are legal values here,
         * distinct from "a real Pixmap id". We honour only the real-id
         * case; the two sentinels collapse to "no tile, use pixel". */
        Pixmap pm = attrs->background_pixmap;
        if (pm == ParentRelative || pm == None) pm = 0;
        win->background_pixmap = pm;
        emx11_js_window_set_bg_pixmap(w, pm);
    }
    /* Ignored: CWBorderPixmap, CWCursor, ... */
    return 1;
}

int XSetWindowBackground(Display *display, Window w, unsigned long background) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    win->background_pixel = background;
    /* Setting a solid pixel overrides any prior pixmap tile, matching
     * Xlib's documented behaviour. */
    if (win->background_pixmap != 0) {
        win->background_pixmap = 0;
        emx11_js_window_set_bg_pixmap(w, 0);
    }
    return 1;
}

int XSetWindowBackgroundPixmap(Display *display, Window w, Pixmap pm) {
    EmxWindow *win = emx11_window_find(display, w);
    if (!win) return 0;
    if (pm == ParentRelative || pm == None) pm = 0;
    win->background_pixmap = pm;
    emx11_js_window_set_bg_pixmap(w, pm);
    return 1;
}

Status XGetWindowAttributes(Display *display, Window w,
                            XWindowAttributes *attrs_return) {
    if (!attrs_return) return 0;
    EmxWindow *win = emx11_window_find(display, w);
    if (win) {
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

    /* Cross-connection fallback: the caller has no local shadow for this
     * XID (WM case -- twm querying xeyes's shell to size its frame).
     * dix/window.c treats window state as server-authoritative by XID,
     * so we reach through to the Host for the canonical record. */
    int a[8];
    emx11_js_get_window_attrs(w, a);
    if (!a[0]) return 0;
    memset(attrs_return, 0, sizeof(*attrs_return));
    attrs_return->x                = a[1];
    attrs_return->y                = a[2];
    attrs_return->width            = a[3];
    attrs_return->height           = a[4];
    attrs_return->border_width     = a[7];
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
    attrs_return->map_state        = a[5] ? IsViewable : IsUnmapped;
    attrs_return->all_event_masks  = 0;
    attrs_return->your_event_mask  = 0;
    attrs_return->do_not_propagate_mask = 0;
    attrs_return->override_redirect = a[6] ? True : False;
    attrs_return->screen           = &display->screens[0];
    return 1;
}

Status XGetGeometry(Display *display, Drawable d,
                    Window *root_return, int *x_return, int *y_return,
                    unsigned int *width_return, unsigned int *height_return,
                    unsigned int *border_width_return,
                    unsigned int *depth_return) {
    EmxWindow *win = emx11_window_find(display, (Window)d);
    if (win) {
        if (root_return)         *root_return         = display->screens[0].root;
        if (x_return)            *x_return            = win->x;
        if (y_return)            *y_return            = win->y;
        if (width_return)        *width_return        = win->width;
        if (height_return)       *height_return       = win->height;
        if (border_width_return) *border_width_return = win->border_width;
        if (depth_return)        *depth_return        = (unsigned int)display->screens[0].root_depth;
        return 1;
    }

    /* Cross-connection fallback: same story as XGetWindowAttributes --
     * twm.c:845 queries a managed client's shell geometry. */
    int a[8];
    emx11_js_get_window_attrs((Window)d, a);
    if (!a[0]) return 0;
    if (root_return)         *root_return         = display->screens[0].root;
    if (x_return)            *x_return            = a[1];
    if (y_return)            *y_return            = a[2];
    if (width_return)        *width_return        = (unsigned int)a[3];
    if (height_return)       *height_return       = (unsigned int)a[4];
    if (border_width_return) *border_width_return = (unsigned int)a[7];
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
    if (win) win->event_mask = event_mask;
    /* Always forward to Host, even for cross-connection targets (twm
     * must XSelectInput(root, SubstructureRedirectMask) -- root is
     * Host-owned, no local shadow of its event_mask would help). */
    emx11_js_select_input(display->conn_id, w, event_mask);
    return 1;
}

int XStoreName(Display *display, Window w, const char *name) {
    if (!name) return 0;
    EmxWindow *win = emx11_window_find(display, w);
    if (win) {
        strncpy(win->name, name, sizeof(win->name) - 1);
        win->name[sizeof(win->name) - 1] = '\0';
    }
    /* Real Xlib XStoreName is just XChangeProperty(WM_NAME, STRING, 8,
     * Replace, name, len) -- twm and other WMs read the title via
     * XGetWMName / XFetchName which round-trips through that property.
     * The local `win->name` buffer above is a debugging convenience; the
     * canonical store is the Host-side property table, which any
     * connection can read regardless of who wrote it.
     *
     * ICCCM 4.1.2.1: STRING property is NUL-terminated by convention but
     * the property's `nitems` is the byte length WITHOUT the NUL. */
    int n = (int)strlen(name);
    XChangeProperty(display, w, XA_WM_NAME, XA_STRING, 8,
                    PropModeReplace, (const unsigned char *)name, n);
    return 1;
}

/* XReparentWindow: move a window under a new parent. Always forwards to
 * the Host, even if the caller has no local shadow -- twm reparenting
 * xeyes's shell (a conn-2 XID) is the canonical cross-connection case,
 * and twm wouldn't have a local EmxWindow for it. */
int XReparentWindow(Display *display, Window w, Window parent, int x, int y) {
    EmxWindow *win = emx11_window_find(display, w);
    if (win) {
        win->parent = parent;
        win->x = x;
        win->y = y;
    }
    emx11_js_reparent_window(w, parent, x, y);
    return 1;
}
