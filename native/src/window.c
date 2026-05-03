#include "emx11_internal.h"
#include "emx11_meta_layout.h"

#include <X11/Xatom.h>
#include <emscripten.h>
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
 * in place for Expose); we leave those paths alone here.
 *
 * Mask gating (xserver dix/events.c::DeliverEvents): the real server
 * only queues Structure/SubstructureNotify when SOMEONE subscribed.
 * Without this gate, twm sees MapNotify+UnmapNotify on every internal
 * window it touches (e.g. hilite_w which is created → mapped via
 * XMapSubwindows → unmapped immediately in add_window.c:1347). twm's
 * HandleUnmapNotify then locates Tmp_win via TwmContext (hilite_w is
 * registered there via add_window.c:953) and -- because HandleMapNotify
 * just set `Tmp_win->mapped=TRUE` -- interprets the unmap as the
 * client withdrawing, calls HandleDestroyNotify, destroys its own
 * frame. Real twm doesn't trip this because it never selected
 * StructureNotifyMask on hilite_w (nor SubstructureNotifyMask on
 * hilite_w's parent title_w), so the events were never delivered. */
static bool wants_structure(Display *dpy, Window w) {
    EmxWindow *win = emx11_window_find(dpy, w);
    if (!win) return false;
    if (win->event_mask & StructureNotifyMask) return true;
    if (win->parent != None) {
        EmxWindow *p = emx11_window_find(dpy, win->parent);
        if (p && (p->event_mask & SubstructureNotifyMask)) return true;
    }
    return false;
}

static void push_map_notify(Display *dpy, EmxWindow *win, bool mapped) {
    if (!wants_structure(dpy, win->id)) return;
    XEvent ev = {0};
    if (mapped) {
        ev.type = MapNotify;
        /* See push_configure_notify: serial must equal Tk's captured
         * NextRequest() value. The XMap/XUnmapWindow callers bump
         * dpy->request before this fires, so dpy->request matches. */
        ev.xmap.serial     = dpy->request;
        ev.xmap.send_event = False;
        ev.xmap.display    = dpy;
        ev.xmap.event      = win->id;
        ev.xmap.window     = win->id;
        ev.xmap.override_redirect = win->override_redirect ? True : False;
    } else {
        ev.type = UnmapNotify;
        ev.xunmap.serial     = dpy->request;
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
    /* Tk's WaitForConfigureNotify (tkUnixWm.c:5204) computes
     *   diff = event.xconfigure.serial - serial
     * and only accepts the event when diff >= 0. The `serial` Tk
     * captures via NextRequest() before sending XResize/Configure
     * equals the post-send dpy->request value. Using `dpy->request`
     * here matches that exactly; serial=0 (the previous default)
     * left Tk waiting 2s per top-level resize because every
     * comparison underflowed. */
    ev.xconfigure.serial     = dpy->request;
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
                           x, y, width, height,
                           border_width, border,
                           /* XCreateSimpleWindow takes an explicit
                            * background pixel; that fixes state to
                            * BackgroundPixel (= 1). XCreateWindow uses
                            * the bridge directly when CWBackPixel is
                            * absent so it can pass state=None (= 0). */
                           1, background);
    return w->id;
}

/* Full XCreateWindow with attributes -- Xt calls this directly from
 * XtRealizeWidget rather than the simple form. The attributes struct
 * lets the caller override background, border, event mask, and
 * override_redirect; visual/depth we ignore (single-visual world).
 *
 * Background-state semantics (xserver/dix/window.c around line 1180):
 * - CWBackPixel set      -> BackgroundPixel
 * - CWBackPixmap set:
 *     None            -> None
 *     ParentRelative  -> ParentRelative (we treat as None for now)
 *     real Pixmap     -> BackgroundPixmap
 * - neither set          -> None (default)
 * Without this, every Xt-created shell ends up with our old default
 * pixel=0 = solid black, which stomps the application's drawing on
 * every Expose/raise (xeyes regressed this way once z-order paint
 * actually started honouring window bg). */
Window XCreateWindow(Display *display, Window parent,
                     int x, int y,
                     unsigned int width, unsigned int height,
                     unsigned int border_width,
                     int depth, unsigned int class_,
                     Visual *visual, unsigned long valuemask,
                     XSetWindowAttributes *attrs) {
    (void)depth; (void)class_; (void)visual;

    EmxWindow *w = emx11_window_alloc(display);
    if (!w) return None;

    unsigned long bg_pixel = 0x00000000UL;
    unsigned long bd       = 0x00000000UL;
    int bg_type            = 0; /* default: None */
    if (attrs && (valuemask & CWBackPixel)) {
        bg_pixel = attrs->background_pixel;
        bg_type  = 1;
    }
    if (attrs && (valuemask & CWBorderPixel)) bd = attrs->border_pixel;

    w->id               = emx11_next_xid(display);
    w->parent           = parent;
    w->x                = x;
    w->y                = y;
    w->width            = width;
    w->height           = height;
    w->border_width     = border_width;
    w->border_pixel     = bd;
    w->background_pixel = bg_pixel;
    w->mapped           = false;

    emx11_js_window_create(display->conn_id, w->id, parent,
                           x, y, width, height,
                           border_width, bd,
                           bg_type, bg_pixel);

    /* Apply remaining bits (CWBackPixmap, CWEventMask, ...) via the
     * common setter. CWBackPixel/CWBorderPixel were already shipped to
     * Host above so XChangeWindowAttributes' redundant push is fine. */
    if (attrs) XChangeWindowAttributes(display, w->id, valuemask, attrs);
    return w->id;
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
     * so a local push is exactly right.
     *
     * State-change gate (xserver dix/window.c::MapWindow): the real
     * server early-returns when pWin->mapped is already true, generating
     * NO MapNotify. Without this gate, twm sees a spurious MapNotify
     * for windows it never expected to see one for, and conversely the
     * symmetric XUnmapWindow path generates a spurious UnmapNotify
     * which twm interprets via HandleUnmapNotify as the client wanting
     * WithdrawnState -- triggering HandleDestroyNotify which destroys
     * the WM frame. Real twm relies on this no-op-on-already-mapped
     * behaviour around HandleMapNotify line 1387's XUnmapWindow on
     * hilite_w (never mapped -> no event) and the iconmgr setup paths. */
    EmxWindow *win = emx11_window_find(display, w);
    if (win) {
        bool was_mapped = win->mapped;
        win->mapped = true;
        if (!was_mapped) {
            display->request++;  /* match Tk's NextRequest() — see notify_js_reconfigure */
            push_map_notify(display, win, true);
        }

        /* Implicit toplevel focus: without a WM (our world) no one
         * assigns X focus to newly mapped toplevels, and Tk's
         * TkSetFocusWin (tkFocus.c:644) silently no-ops when
         * displayFocusPtr->focusWinPtr is still NULL. That branch only
         * gets populated after a FocusIn lands on a toplevel wrapper,
         * so `focus .some.entry` from user clicks would be a no-op:
         * cursors never blink and keys never route. Auto-focus the
         * first root-child wrapper to map, mirroring the "no WM,
         * server auto-focuses override-redirect window" path in real X. */
        if (!was_mapped &&
            win->parent == display->screens[0].root &&
            display->focus_window == None) {
            XSetInputFocus(display, w, RevertToParent, CurrentTime);
        }
    }
    emx11_js_window_map(display->conn_id, w);
    return 1;
}

int XUnmapWindow(Display *display, Window w) {
    /* State-change gate: see XMapWindow. Real X early-returns on already-
     * unmapped windows; we have to do the same or twm's destructor path
     * fires on phantom UnmapNotifies for never-mapped windows. */
    EmxWindow *win = emx11_window_find(display, w);
    if (win) {
        bool was_mapped = win->mapped;
        win->mapped = false;
        if (was_mapped) {
            display->request++;
            push_map_notify(display, win, false);
        }
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

    win->in_use = false;
    emx11_js_window_destroy(w);
    return 1;
}

/* -- Geometry changes. The JS-side compositor needs to learn about
 *    every size/position change so it can redraw, so we push through
 *    emx11_js_window_create when a window is logically "re-set". */

static void notify_js_reconfigure(Display *dpy, EmxWindow *win) {
    /* Bump dpy->request to model the X protocol "request serial" Xlib
     * tracks per outgoing request. Tk captures `serial = NextRequest()`
     * (= dpy->request + 1) BEFORE calling XResizeWindow / XConfigure-
     * Window, then waits for a ConfigureNotify whose serial >= captured.
     * Without this bump, dpy->request stays at 0 and push_configure_notify
     * pushes serial=0; Tk's diff = 0 - serial < 0 and the wait loops on
     * tkUnixWm.c's hard-coded 2-second deadline (see project_emx11_
     * configure_serial in memory). Incrementing here makes dpy->request
     * equal to the value Tk captured, so the synthetic ConfigureNotify
     * satisfies the wait on the first poll.
     */
    dpy->request++;
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
    EM_ASM({
        if (globalThis.__EMX11_TRACE_MOVE__) {
            console.log('[c-move] conn=' + $0 + ' win=' + ($1 >>> 0) +
                        ' x=' + $2 + ' y=' + $3 +
                        ' old_x=' + $4 + ' old_y=' + $5);
        }
    }, display->conn_id, w, x, y, win->x, win->y);
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
    if (valuemask & CWBorderWidth) {
        emx11_js_window_set_border(win->id, win->border_width, win->border_pixel);
    }
    return 1;
}

int XRaiseWindow(Display *display, Window w) {
    (void)display;
    emx11_js_window_raise(w);
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
    if (valuemask & CWBackPixel) {
        win->background_pixel = attrs->background_pixel;
        emx11_js_window_set_bg(w, 1, win->background_pixel);
    }
    if (valuemask & CWBorderPixel) {
        win->border_pixel = attrs->border_pixel;
        emx11_js_window_set_border(w, win->border_width, win->border_pixel);
    }
    if (valuemask & CWEventMask) {
        win->event_mask = attrs->event_mask;
        emx11_js_select_input(display->conn_id, w, attrs->event_mask);
    }
    if (valuemask & CWOverrideRedirect) {
        win->override_redirect = attrs->override_redirect;
        emx11_js_set_override_redirect(w, attrs->override_redirect ? 1 : 0);
    }
    if (valuemask & CWBackPixmap) {
        /* X semantics (xserver/dix/window.c:1186-1216):
         *   None           -> backgroundState = None (no auto-paint)
         *   ParentRelative -> use parent's tile (we map to None for now;
         *                     proper impl needs parent lookup at paint
         *                     time)
         *   real Pixmap    -> backgroundState = BackgroundPixmap
         * We must NOT collapse None to "use the pixel" -- doing so
         * caused xeyes' shell (default bg = None) to paint solid black
         * over the application's drawing on every Expose/raise. */
        Pixmap pm = attrs->background_pixmap;
        if (pm == None || pm == ParentRelative) {
            win->background_pixmap = 0;
            emx11_js_window_set_bg(w, 0, 0); /* state = None */
        } else {
            win->background_pixmap = pm;
            emx11_js_window_set_bg_pixmap(w, pm);
        }
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
    /* Push the new pixel to the Host so the next XClearArea / Expose
     * actually paints with it. Without this, Xt's XawCommandToggle
     * (swaps fg/bg on click) updates our native struct but the
     * compositor keeps the old colour -- the next clearArea fills
     * with the OLD bg while Label redraws text using the NEW fg
     * (which equals the old bg), rendering invisible text. */
    emx11_js_window_set_bg(w, 1, background);
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
    int a[EMX11_WIN_ATTRS_SIZE];
    emx11_js_get_window_attrs(w, a);
    if (!a[EMX11_WIN_ATTRS_PRESENT]) return 0;
    memset(attrs_return, 0, sizeof(*attrs_return));
    attrs_return->x                = a[EMX11_WIN_ATTRS_X];
    attrs_return->y                = a[EMX11_WIN_ATTRS_Y];
    attrs_return->width            = a[EMX11_WIN_ATTRS_WIDTH];
    attrs_return->height           = a[EMX11_WIN_ATTRS_HEIGHT];
    attrs_return->border_width     = a[EMX11_WIN_ATTRS_BORDER_WIDTH];
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
    attrs_return->map_state        = a[EMX11_WIN_ATTRS_MAPPED] ? IsViewable : IsUnmapped;
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
    int a[EMX11_WIN_ATTRS_SIZE];
    emx11_js_get_window_attrs((Window)d, a);
    if (!a[EMX11_WIN_ATTRS_PRESENT]) return 0;
    if (root_return)         *root_return         = display->screens[0].root;
    if (x_return)            *x_return            = a[EMX11_WIN_ATTRS_X];
    if (y_return)            *y_return            = a[EMX11_WIN_ATTRS_Y];
    if (width_return)        *width_return        = (unsigned int)a[EMX11_WIN_ATTRS_WIDTH];
    if (height_return)       *height_return       = (unsigned int)a[EMX11_WIN_ATTRS_HEIGHT];
    if (border_width_return) *border_width_return = (unsigned int)a[EMX11_WIN_ATTRS_BORDER_WIDTH];
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
    /* Walk src and dst to their root-relative origins, then compute the
     * destination offset. The old implementation assumed a flat tree
     * (all windows children of root) and did `src->x + src_x - dst->x`,
     * which is wrong as soon as src or dst has a non-root parent -- twm
     * frames contain a titlebar child, and F_MOVE's XTranslateCoordinates
     * from titlebar to frame returned `0 + click_x - frame_x` instead
     * of `click_x`, so the drag offset was off by the frame's current
     * position and the window jumped on drag start. */
    int src_ax = 0, src_ay = 0;
    for (EmxWindow *cur = src; cur; ) {
        src_ax += cur->x;
        src_ay += cur->y;
        if (cur->parent == None || cur->parent == cur->id) break;
        EmxWindow *p = emx11_window_find(display, cur->parent);
        if (!p) break;
        cur = p;
    }
    int dst_ax = 0, dst_ay = 0;
    for (EmxWindow *cur = dst; cur; ) {
        dst_ax += cur->x;
        dst_ay += cur->y;
        if (cur->parent == None || cur->parent == cur->id) break;
        EmxWindow *p = emx11_window_find(display, cur->parent);
        if (!p) break;
        cur = p;
    }
    if (dest_x_return) *dest_x_return = src_ax + src_x - dst_ax;
    if (dest_y_return) *dest_y_return = src_ay + src_y - dst_ay;
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
