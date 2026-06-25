/*
 * Input dispatch: hit-testing, pointer-window tracking, and the C entry
 * points the JS host calls (em_x11_push_button_event / motion / key /
 * expose / map_request / reparent_notify). Queue plumbing lives in
 * event_queue.c; XSendEvent + focus in event_send.c; keysyms in
 * event_keysym.c.
 */

#include "em_x11_internal.h"
#include "em_x11_meta_layout.h"

#include <X11/extensions/XInput2.h>
#include <X11/extensions/shape.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

/* -- Window hit testing -------------------------------------------------- */

/* Walk the parent chain to compute the window's origin in root-relative
 * coordinates, plus its depth in the tree (root = 0, shell = 1, ...).
 * Cycles in `parent` links (should never happen) are defended against
 * with a hard iteration limit. */
static void window_abs_origin(
  Display* dpy, EmxWindow* w, int* ax_out, int* ay_out, int* depth_out) {
  int ax = 0, ay = 0, depth = 0;
  EmxWindow* cur = w;
  for (int guard = 0; cur && guard < EM_X11_MAX_WINDOWS; guard++, depth++) {
    ax += cur->x;
    ay += cur->y;
    if (cur->parent == None || cur->parent == cur->id)
      break;
    EmxWindow* parent = em_x11_window_find(dpy, cur->parent);
    if (!parent) {
      /* Parent is owned by a different connection (e.g. xcalc
       * shell reparented under a twm-owned frame). Our local table
       * doesn't have it, so the cumulative origin needs to come
       * from Host's authoritative tree. Without this fallback the
       * walk terminates with cur->parent's offset missing, every
       * input event lands at canvas-absolute - frame.position
       * inside the conn's coordinate system, and Xaw widgets
       * highlight the wrong button on hover. */
      int buf[EM_X11_ABS_ORIGIN_SIZE] = {0};
      em_x11_js_get_window_abs_origin(cur->parent, buf);
      if (buf[EM_X11_ABS_ORIGIN_PRESENT]) {
        ax += buf[EM_X11_ABS_ORIGIN_AX];
        ay += buf[EM_X11_ABS_ORIGIN_AY];
      }
      break;
    }
    cur = parent;
  }
  if (ax_out)
    *ax_out = ax;
  if (ay_out)
    *ay_out = ay;
  if (depth_out)
    *depth_out = depth;
}

/* Given a root-relative point, find the deepest mapped window that
 * (a) contains the point and (b) has at least one of `need_mask` bits
 * in its event_mask. If the deepest containing window doesn't select
 * for the event, propagate up the parent chain (INCLUDING root) to the
 * first ancestor that does -- matching xserver/dix/events.c's
 * DeliverEventsToWindow semantics. Returns NULL only when literally
 * nothing in the table or in the ancestor chain wants the event.
 *
 * Root IS a valid hit and a valid delivery target. Twm selects on root
 * for the bare-button menus and for SubstructureRedirect; any
 * "single-client world" pruning here breaks the WM. xserver's
 * miSpriteTrace starts from root and hands deliveries to root too.
 *
 * `lx`/`ly` are filled with the winning window's local coordinates. */
static EmxWindow* hit_test(
  Display* dpy, int rx, int ry, long need_mask, int* lx_out, int* ly_out) {
  EmxWindow* best = NULL;
  int best_depth = -1;
  int best_ax = 0, best_ay = 0;

  for (int i = 0; i < EM_X11_MAX_WINDOWS; i++) {
    EmxWindow* w = &dpy->windows[i];
    if (!w->in_use || !w->mapped)
      continue;

    int ax, ay, depth;
    window_abs_origin(dpy, w, &ax, &ay, &depth);
    if (rx < ax || ry < ay || rx >= ax + (int)w->width ||
        ry >= ay + (int)w->height) {
      continue;
    }
    /* depth tie-break by stack-ish order doesn't apply here -- we keep
     * deepest-by-tree-depth which matches xorg's "child wins over
     * parent at same point" semantics. Same depth means siblings,
     * which our tree doesn't disambiguate; first-found wins. */
    if (depth > best_depth) {
      best = w;
      best_depth = depth;
      best_ax = ax;
      best_ay = ay;
    }
  }
  if (!best)
    return NULL;

  /* Propagate up to AND including root. We keep updating (ax, ay) so
   * local coords stay correct wherever the chain terminates. */
  EmxWindow* cur = best;
  int ax = best_ax, ay = best_ay;
  while (cur) {
    if (cur->event_mask & need_mask) {
      if (lx_out)
        *lx_out = rx - ax;
      if (ly_out)
        *ly_out = ry - ay;
      return cur;
    }
    if (cur->parent == None || cur->parent == cur->id)
      break;
    EmxWindow* p = em_x11_window_find(dpy, cur->parent);
    if (!p)
      break;      /* parent in another conn's table */
    ax -= cur->x; /* un-offset into parent frame */
    ay -= cur->y;
    cur = p;
  }
  /* Nothing along the chain selected for this event. Fall back to the
   * deepest hit anyway so the event isn't lost -- matches the shape
   * of the old "always dispatch somewhere" behavior while we build out
   * the rest of the dispatch logic. */
  if (lx_out)
    *lx_out = rx - best_ax;
  if (ly_out)
    *ly_out = ry - best_ay;
  return best;
}

/* Mirrors xserver/dix/events.c DeliverDeviceEvents (line 2888): given
 * the z-order-correct deepest window from the host (whose JS findWindowAt
 * is equivalent to xorg's XYToWindow → miSpriteTrace), walk up the parent
 * chain in this Display's table to find the first window whose event_mask
 * contains `need_mask`. Returns that window with coordinates relative to
 * it. Falls back to the starting window if no ancestor matches.
 *
 * This hybrid approach fixes both tcldide and twm:
 * - tcldide: host hint is a Tk widget in the same Display → widget has
 *   the mask → delivered directly (z-order correct, mask correct)
 * - twm: host hint may be a cross-connection shadow (in this Display's
 *   EmxWindow table from reparent but mapped=false / mask=0) → walk up
 *   skips it, finds frame/root with the right mask → correct
 * - twm active grab: host hint is a foreign window not in this Display
 *   → caller falls back to hit_test → finds twm window → correct */
static EmxWindow* walk_up_for_mask(Display* dpy,
                                   EmxWindow* start,
                                   int rx,
                                   int ry,
                                   long need_mask,
                                   int* lx_out,
                                   int* ly_out) {
  int base_ax, base_ay, depth_unused;
  window_abs_origin(dpy, start, &base_ax, &base_ay, &depth_unused);

  EmxWindow* cur = start;
  int ax = base_ax, ay = base_ay;
  while (cur) {
    if (cur->event_mask & need_mask) {
      if (lx_out)
        *lx_out = rx - ax;
      if (ly_out)
        *ly_out = ry - ay;
      return cur;
    }
    if (cur->parent == None || cur->parent == cur->id)
      break;
    EmxWindow* p = em_x11_window_find(dpy, cur->parent);
    if (!p)
      break;
    ax -= cur->x;
    ay -= cur->y;
    cur = p;
  }
  /* Nothing along the chain selected for this event. Deliver to the
   * starting window anyway so the event isn't lost. */
  if (lx_out)
    *lx_out = rx - base_ax;
  if (ly_out)
    *ly_out = ry - base_ay;
  return start;
}

/* -- JS -> C event bridges ------------------------------------------------- */

/* Implicit pointer grab state (x11protocol.txt §523).
 * A ButtonPress initiates a grab: subsequent ButtonRelease and MotionNotify
 * events are routed to the grab window regardless of current pointer position.
 * The grab is released when the last simultaneously-held button is released. */
static Window grab_window = None;
static unsigned int grab_button_count = 0;

/* Active pointer grab (XGrabPointer). Tracked separately from the implicit
 * grab so crossing events carry mode=NotifyGrab (emit_crossing) and the
 * motion_target fallback (em_x11_push_motion_event) can route to the grab
 * window when hit_test finds nothing. XGrabPointer also resets the stale
 * implicit grab so ButtonRelease routes to the window under the pointer
 * instead of the original press window -- needed for MenuButton/ComboBox
 * popup entries where Tk posts a transient menu and calls XGrabPointer. */
static bool active_grab = false;
static Window active_grab_window = None;
static unsigned int active_grab_event_mask = 0;

/* Popup menu windows registered for dual-delivery. Motif posts menu
 * shells as OverrideRedirect top-levels — siblings, not children, of
 * the grab window. During outside-click dual-delivery we must also
 * send a copy to each registered popup so MenuShell's
 * <BtnUp>:MenuShellPopdownDone() translation can dismiss. */
static Window popup_windows[8];
static int popup_window_count = 0;

void em_x11_register_popup(Window w) {
  for (int i = 0; i < popup_window_count; i++)
    if (popup_windows[i] == w)
      return;
  if (popup_window_count < 8)
    popup_windows[popup_window_count++] = w;
}

void em_x11_unregister_popup(Window w) {
  for (int i = 0; i < popup_window_count; i++) {
    if (popup_windows[i] == w) {
      popup_windows[i] = popup_windows[--popup_window_count];
      return;
    }
  }
}

/* Monotonic millisecond timestamp for xbutton/xmotion/xkey/xcrossing `time`
 * fields. Some WMs (twm's ConstrainedMove in particular: menus.c:1500) compare
 * `event.time - last_click_time` against a timeout to detect rapid successive
 * clicks. Leaving time=0 on every event makes the delta always 0, which trips
 * the < 400ms gate on *every* press -- twm then enters ConstMove and freezes
 * one axis of the drag. emscripten_get_now is a double of ms since epoch, so
 * unsigned-casting it is fine for the 32-bit Time field (wraps every ~49 days
 * like a real X server does). */
static Time event_now(void) {
  return (Time)(unsigned long)emscripten_get_now();
}

/* Browser autorepeat-suppression for modifier keys. Real X servers never
 * autorepeat modifier keys (Shift/Control/Alt/Meta/Super/Hyper), but browsers
 * fire keydown repeatedly for every key. Each repeated Control_L KeyPress would
 * otherwise flow through the Xt translation pipeline and match catch-all <Key>
 * translations (self-insert), flooding the event loop. Track pressed keycodes
 * and suppress KeyPress for modifier-range keysyms when the keycode is already
 * down. */
static unsigned char down_keycodes[32]; /* 256 bits for keycodes 0-255 */

static int is_modifier_keysym(KeySym ks) {
  return (ks >= 0xFFE1 && ks <= 0xFFEE);
}

/* Window the pointer is currently over, as of the last motion or press we
 * observed. Real X servers synthesize EnterNotify / LeaveNotify whenever the
 * pointer crosses a window boundary (x11protocol §Window crossing), grab or
 * no grab. The browser DOM only delivers raw mousemove, so we track the
 * pointer window here and emit crossings from update_pointer_window. Without
 * this, Tk's <Enter>/<Leave> bindings never fire on simple hover: e.g.
 * tk::ButtonUp's `$Priv(window) eq $w` check stays false for every widget
 * except ones the user has grabbed-and-dragged through, which looks like
 * "button press/release works visually but -command never fires". */
static Window last_pointer_window = None;

/* Reset the implicit pointer grab. Called from XGrabPointer (which
 * establishes an active server grab) so the stale implicit grab from the
 * ButtonPress that triggered the popup doesn't pin future ButtonReleases to
 * the original press window. Without this, MenuButton and ComboBox popup
 * entries never see ButtonRelease: the C-side grab_window stays on the
 * button that opened the menu, and every subsequent button-up gets delivered
 * there instead of to the menu entry under the pointer. */
void em_x11_reset_implicit_grab(void) {
  grab_window = None;
  grab_button_count = 0;
}

/* Window-relationship classifier for crossing detail computation.
 * Returns 1 if `descendant` is in the parent chain of `ancestor` (so
 * `descendant` is inferior, `ancestor` is ancestor); 0 otherwise.
 * Walks until parent==None / self / not-in-this-conn. */
static int
win_is_inferior_of(Display* dpy, Window descendant, Window ancestor) {
  if (descendant == None || ancestor == None || descendant == ancestor)
    return 0;
  EmxWindow* cur = em_x11_window_find(dpy, descendant);
  EM_ASM(
    {
      var d = Module['emX11Debug'];
      if (d && d.traceCBtn)
        console.log('[c-inferior] desc=' + ($0 >>> 0) + ' anc=' + ($1 >>> 0) +
                    ' start=' + ($2 >>> 0));
    },
    descendant,
    ancestor,
    cur ? cur->id : 0);
  while (cur && cur->parent != None && cur->parent != cur->id) {
    EM_ASM(
      {
        var d = Module['emX11Debug'];
        if (d && d.traceCBtn)
          console.log('[c-inferior]   step id=' + ($0 >>> 0) + ' parent=' +
                      ($1 >>> 0));
      },
      cur->id,
      cur->parent);
    if (cur->parent == ancestor)
      return 1;
    cur = em_x11_window_find(dpy, cur->parent);
  }
  EM_ASM({
    var d = Module['emX11Debug'];
    if (d && d.traceCBtn)
      console.log('[c-inferior]   result=false');
  });
  return 0;
}

/* Compute X11 crossing detail (NotifyAncestor / NotifyInferior /
 * NotifyNonlinear) per x11protocol §10.4 for a Leave(`from`)+Enter(`to`)
 * pair. Real X servers branch on this; twm relies on it to elide
 * Map/Unmap-induced crossings between a frame and its hilite_w child
 * (events.c::HandleLeaveNotify gates XUnmapWindow on
 * `detail != NotifyInferior`). Without proper detail, twm interprets
 * every "we mapped a child window" as "the user left the frame",
 * unmaps the child, repoll fires the inverse pair, twm re-maps,
 * infinite C loop wedges the tab.
 *
 * Returned values per X11:
 *   pointer A→B with B inferior of A: Leave(A,Inferior), Enter(B,Ancestor)
 *   pointer A→B with A inferior of B: Leave(A,Ancestor), Enter(B,Inferior)
 *   otherwise (siblings / unrelated): both Nonlinear. (Virtuals on
 *   intermediate windows are not synthesised here -- they require
 *   per-window-on-the-path emit_crossing calls; see follow-up.) */
static int crossing_detail(Display* dpy, int type, Window from, Window to) {
  if (win_is_inferior_of(dpy, to, from)) {
    return (type == LeaveNotify) ? NotifyInferior : NotifyAncestor;
  }
  if (win_is_inferior_of(dpy, from, to)) {
    return (type == LeaveNotify) ? NotifyAncestor : NotifyInferior;
  }
  return NotifyNonlinear;
}

/* Push an EnterNotify / LeaveNotify on `w`, iff the window selects for that
 * mask. Coords are root-relative; we derive window-local ones from the
 * window's absolute origin. `peer` is the other end of the crossing
 * (the window the pointer is leaving from on EnterNotify, the window
 * it's entering on LeaveNotify) -- used to compute xcrossing.detail. */
static void emit_crossing(Display* dpy,
                          int type,
                          Window w,
                          Window peer,
                          int x_root,
                          int y_root,
                          unsigned int state) {
  EmxWindow* win = em_x11_window_find(dpy, w);
  if (!win)
    return;
  long mask = (type == EnterNotify) ? EnterWindowMask : LeaveWindowMask;
  /* xorg CoreEnterLeaveEvent: when a grab is active, mask =
   * grab->eventMask | EventMaskForClient for EVERY window, not
   * just the grab window. The grab mask supplies event selection
   * that submenu windows didn't explicitly select (LeaveWindowMask
   * for spring-loaded dismissal). */
  unsigned int effective_mask = win->event_mask;
  if (active_grab)
    effective_mask |= active_grab_event_mask;
  if (!(effective_mask & mask))
    return;

  /* crossing_detail wants (from, to) as the pointer's actual travel
   * direction. On LeaveNotify the pointer moved away from `w` toward
   * `peer`; on EnterNotify it moved from `peer` onto `w`. update_pointer_window
   * passes (cur, prev) for Leave and (cur, prev) for Enter (i.e. the
   * caller's `peer` plays opposite roles in the two callsites), so
   * disentangle here based on which crossing this is. */
  bool is_leave = (type == LeaveNotify);
  Window from = is_leave ? w : peer;
  Window to = is_leave ? peer : w;
  int detail = crossing_detail(dpy, type, from, to);

  int ax = 0, ay = 0, depth;
  window_abs_origin(dpy, win, &ax, &ay, &depth);

  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = type;
  ev.xcrossing.display = dpy;
  ev.xcrossing.window = w;
  ev.xcrossing.root = dpy->screens[0].root;
  ev.xcrossing.x = x_root - ax;
  ev.xcrossing.y = y_root - ay;
  ev.xcrossing.x_root = x_root;
  ev.xcrossing.y_root = y_root;
  /* Tk's menubutton dismisses a posted menu on LeaveNotify with
   * mode=NotifyNormal (tkMenuButtonLeave in library/menu.tcl). During
   * an active pointer grab -- set by XGrabPointer when Tk posts the
   * menu -- the X server delivers crossings with mode=NotifyGrab so
   * the menubutton knows the leave was grab-induced and keeps the menu
   * posted. Without this, the repoll after XMapWindow(menu) emits
   * a LeaveNotify with NotifyNormal, Tk's menubutton dismisses the
   * menu immediately, and the "first click pops menu then it vanishes"
   * symptom appears. */
  ev.xcrossing.mode = active_grab ? NotifyGrab : NotifyNormal;
  ev.xcrossing.detail = detail;
  ev.xcrossing.same_screen = True;
  ev.xcrossing.focus = (w == dpy->focus_window);
  ev.xcrossing.state = state;
  ev.xcrossing.time = event_now();
  em_x11_event_queue_push(dpy, &ev);
}

/* Update last_pointer_window, emitting Leave on the outgoing window and
 * Enter on the incoming one. Called on every motion, on ButtonPress
 * (so the "first interaction is a click, no prior mousemove" path still
 * delivers the Enter that Tk's button bindings depend on), and from
 * em_x11_repoll_pointer_window after a map/unmap that may have changed
 * the topmost window under a static pointer (twm root menu pop-up). */
static void update_pointer_window(
  Display* dpy, Window cur, int x_root, int y_root, unsigned int state) {
  if (cur == last_pointer_window)
    return;
  Window prev = last_pointer_window;
  if (prev != None) {
    emit_crossing(dpy, LeaveNotify, prev, cur, x_root, y_root, state);
  }
  if (cur != None) {
    emit_crossing(dpy, EnterNotify, cur, prev, x_root, y_root, state);
  }
  last_pointer_window = cur;
}

/* Re-evaluate which window is under the cursor and fire any Enter/Leave
 * pair that the change implies. Real X servers synthesize crossings
 * whenever a map/unmap/raise/lower changes the topmost window at the
 * sprite position (xserver/dix/events.c::CheckMotion via WindowsRestructured).
 * Without this hook, twm's root menu pops up *under* the cursor: hit_test
 * now resolves to the menu, but last_pointer_window is still root, so the
 * next motion event fires Leave/Enter only after the user crosses the
 * boundary -- twm's UpdateMenu (menus.c:512) gates every hover update on
 * `ActiveMenu->entered`, which is set on EnterNotify, so until that fires
 * no menu item highlights and release lands with ActiveItem=NULL.
 *
 * Called from window.c after XMapWindow / XUnmapWindow on this display.
 * `state` defaults to 0 since we have no fresh modifier sample to attach;
 * Tk and twm don't read xcrossing.state for menu logic. */
void em_x11_repoll_pointer_window(Display* dpy) {
  if (!dpy)
    return;
  int px = 0, py = 0;
  em_x11_js_pointer_xy(&px, &py);
  int lx = 0, ly = 0;
  EmxWindow* cur = hit_test(dpy, px, py, 0, &lx, &ly);
  Window cur_id = cur ? cur->id : None;
  update_pointer_window(dpy, cur_id, px, py, 0);
}

/* Variant called from the host's deferred-repoll path with a cur_id the
 * host already resolved via its (stacking-aware) findWindowAt. The C-side
 * hit_test in this file walks dpy->windows[] by tree-depth + first-found,
 * which gives wrong answers for sibling top-levels whose stack order was
 * changed (twm root menu's shadow XRaiseWindow'd before the menu itself
 * is mapped: shadow created first → first-found returns shadow, but host's
 * stackOrder correctly puts menu on top). Trusting the host avoids that
 * divergence. cur_hint may name a window owned by another connection or
 * may be 0 (None); both are valid -- emit_crossing's mask check handles
 * the foreign / unowned cases by no-op'ing. */
void em_x11_repoll_pointer_window_hint(Display* dpy, Window cur_hint) {
  if (!dpy)
    return;
  int px = 0, py = 0;
  em_x11_js_pointer_xy(&px, &py);
  update_pointer_window(dpy, cur_hint, px, py, 0);
}

EMSCRIPTEN_KEEPALIVE
void em_x11_push_button_event(int type,
                              Window window,
                              int x,
                              int y,
                              int x_root,
                              int y_root,
                              unsigned int button,
                              unsigned int state) {
  Display* dpy = em_x11_get_display();
  int lx = 0, ly = 0;
  EmxWindow* target;
  if (type == ButtonRelease && grab_window != None) {
    /* Implicit grab: deliver ButtonRelease to the grab window even if
     * the pointer has moved elsewhere. Compute local coords from the
     * grab window's current absolute position. */
    target = em_x11_window_find(dpy, grab_window);
    if (target) {
      int ax = 0, ay = 0, depth;
      window_abs_origin(dpy, target, &ax, &ay, &depth);
      lx = x_root - ax;
      ly = y_root - ay;
    }
    if (grab_button_count > 0)
      grab_button_count--;
    if (grab_button_count == 0)
      grab_window = None;
  } else {
    long mask = (type == ButtonPress) ? ButtonPressMask : ButtonReleaseMask;
    target = em_x11_window_find(dpy, window);
    if (target) {
      target = walk_up_for_mask(dpy, target, x_root, y_root, mask, &lx, &ly);
      if (!target && active_grab) {
        /* walk_up_for_mask found no subscriber in the ancestor chain.
         * xorg's DeliverGrabbedEvent falls back to DeliverOneGrabbedEvent
         * → grab->window when normal delivery finds no taker. Without
         * this, clicks on windows that don't select ButtonPressMask
         * (e.g. static labels) are silently dropped and popups never
         * dismiss. */
        target = em_x11_window_find(dpy, active_grab_window);
        if (target) {
          int ax = 0, ay = 0, depth;
          window_abs_origin(dpy, target, &ax, &ay, &depth);
          lx = x_root - ax;
          ly = y_root - ay;
        }
      }
    } else {
      /* Use hit_test to find the actual window under the pointer.
       * When active_grab is set, falling back to active_grab_window
       * (the old behaviour) short-circuits Xt's NULL-widget →
       * LookupSpringLoaded path — the grab window always has a
       * widget (e.g. RowColumn), so _XtDefaultDispatcher never
       * reaches the spring-loaded redispatch that dismisses popups
       * on outside clicks. hit_test may return the root window or a
       * cross-conn window with no widget, which triggers the correct
       * path. */
      target = hit_test(dpy, x_root, y_root, mask, &lx, &ly);
      if (!target && active_grab) {
        /* hit_test failed (unlikely), last-resort fallback. */
        target = em_x11_window_find(dpy, active_grab_window);
        if (target) {
          int ax = 0, ay = 0, depth;
          window_abs_origin(dpy, target, &ax, &ay, &depth);
          lx = x_root - ax;
          ly = y_root - ay;
        }
      }
    }

    if (target && type == ButtonPress) {
      update_pointer_window(dpy, target->id, x_root, y_root, state);
      if (grab_button_count == 0)
        grab_window = target->id;
      grab_button_count++;
    }
  }

  if (!target)
    return;

  {
    Time now = event_now();
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xbutton.type = type;
    ev.xany.serial = ++dpy->request;
    ev.xbutton.display = dpy;
    ev.xbutton.window = target->id;
    ev.xbutton.x = lx;
    ev.xbutton.y = ly;
    ev.xbutton.x_root = x_root;
    ev.xbutton.y_root = y_root;
    ev.xbutton.root = dpy->screens[0].root;
    ev.xbutton.button = button;
    ev.xbutton.state = state;
    ev.xbutton.same_screen = True;
    ev.xbutton.time = now;
    em_x11_event_queue_push(dpy, &ev);

    /* xorg DeliverGrabbedEvent: after normal delivery (above),
     * DeliverOneGrabbedEvent sends a copy to the grab window so it
     * can dismiss the popup. Push a second event to the grab window
     * when the normal target is outside the grab hierarchy. Distinct
     * serial numbers prevent Motif's _XmIsEventUnique from treating
     * the grab-delivery as a duplicate.
     *
     * ButtonPress alone is not enough — Motif's MenuShell translation
     * table triggers PopdownDone on <BtnUp> (ButtonRelease), so we
     * must dual-deliver both press and release for the menu to
     * dismiss on outside clicks. */
    if ((type == ButtonPress || type == ButtonRelease) && active_grab &&
        target->id != active_grab_window &&
        !win_is_inferior_of(dpy, target->id, active_grab_window)) {
      unsigned long need_mask =
        (type == ButtonPress) ? ButtonPressMask : ButtonReleaseMask;
      EmxWindow* grab_win = em_x11_window_find(dpy, active_grab_window);
      /* The grab window may not have the mask via XSelectInput, but the
       * active grab's own event_mask (from XGrabPointer) authorises
       * delivery. Motif's EVENTS includes both press and release. */
      if (grab_win &&
          ((grab_win->event_mask | active_grab_event_mask) & need_mask)) {
        /* Deliver to the grab window's parent (MenuShell) when the grab
         * window is a child (RowColumn). The MenuShell has the
         * <BtnUp>:MenuShellPopdownDone() translation that dismisses. */
        EmxWindow* deliver_win = grab_win;
        if (grab_win->parent != 0) {
          EmxWindow* pw = em_x11_window_find(dpy, grab_win->parent);
          if (pw)
            deliver_win = pw;
        }
        int gax = 0, gay = 0, gdepth;
        window_abs_origin(dpy, deliver_win, &gax, &gay, &gdepth);
        XEvent gev;
        memset(&gev, 0, sizeof(gev));
        gev.xbutton.type = type;
        gev.xany.serial = ++dpy->request;
        gev.xbutton.display = dpy;
        gev.xbutton.window = deliver_win->id;
        gev.xbutton.x = x_root - gax;
        gev.xbutton.y = y_root - gay;
        gev.xbutton.x_root = x_root;
        gev.xbutton.y_root = y_root;
        gev.xbutton.root = dpy->screens[0].root;
        gev.xbutton.button = button;
        gev.xbutton.state = state;
        gev.xbutton.same_screen = True;
        gev.xbutton.time = now;
        em_x11_event_queue_push(dpy, &gev);
      }

      /* Also deliver to registered popup windows (MenuShell OverrideRedirect
       * popups) so their <BtnUp>:MenuShellPopdownDone() translations fire.
       * These popups are siblings, not children, of the grab window — the
       * parent-chain delivery above misses them. */
      for (int pi = 0; pi < popup_window_count; pi++) {
        Window pw_id = popup_windows[pi];
        EmxWindow* pw = em_x11_window_find(dpy, pw_id);
        if (!pw)
          continue;
        int pax = 0, pay = 0, pdepth;
        window_abs_origin(dpy, pw, &pax, &pay, &pdepth);
        XEvent pev;
        memset(&pev, 0, sizeof(pev));
        pev.xbutton.type = type;
        pev.xany.serial = ++dpy->request;
        pev.xbutton.display = dpy;
        pev.xbutton.window = pw_id;
        pev.xbutton.x = x_root - pax;
        pev.xbutton.y = y_root - pay;
        pev.xbutton.x_root = x_root;
        pev.xbutton.y_root = y_root;
        pev.xbutton.root = dpy->screens[0].root;
        pev.xbutton.button = button;
        pev.xbutton.state = state;
        pev.xbutton.same_screen = True;
        pev.xbutton.time = now;
        em_x11_event_queue_push(dpy, &pev);
      }
    }
  }
}

EMSCRIPTEN_KEEPALIVE
void em_x11_push_motion_event(
  Window window, int x, int y, int x_root, int y_root, unsigned int state) {
  Display* dpy = em_x11_get_display();

  /* Pointer-window tracking: JS host's findWindowAt is z-order-
   * aware. Trust it first; hit_test is depth-based and can't
   * distinguish overlapping siblings (popup menu above toplevel).
   * Fall back to hit_test only when the host hint is None or names
   * a foreign window not in this Display's table.
   *
   * xorg's CheckMotion always updates the sprite window and fires
   * DoEnterLeaveEvents when the pointer crosses a window boundary,
   * regardless of grab state (dix/events.c:3239-3253). The grab
   * only affects which client receives the events via
   * CoreEnterLeaveEvent's TryClientEvents path, not whether they
   * are generated. In em-x11's single-connection-per-process model
   * all events go to the same client anyway, so we mirror xorg:
   * always run update_pointer_window, always with NotifyNormal
   * during implicit grab / NotifyGrab during active grab
   * (emit_crossing checks active_grab). */
  {
    Window cur_pw = window;
    if (cur_pw == None || !em_x11_window_find(dpy, cur_pw)) {
      /* xorg's CheckMotion always updates the sprite window and fires
       * DoEnterLeaveEvents regardless of grab state. The grab only
       * affects which client receives the events, not whether they
       * are generated. Always use hit_test so that LeaveNotify fires
       * when the pointer leaves the grab window — Motif's spring-
       * loaded menu dismissal depends on this signal. */
      int lx_fb = 0, ly_fb = 0;
      EmxWindow* pt = hit_test(dpy, x_root, y_root, 0, &lx_fb, &ly_fb);
      cur_pw = pt ? pt->id : None;
    }
    update_pointer_window(dpy, cur_pw, x_root, y_root, state);
  }

  /* MotionNotify routing: grab window during a grab, otherwise
   * start from the JS host's z-order-correct window and walk up
   * the C-side parent chain for mask matching (mirrors xorg's
   * XYToWindow → DeliverDeviceEvents). hit_test is only a fallback
   * for cross-connection cases where the host hint doesn't exist
   * in this Display's table. */
  EmxWindow* motion_target;
  bool via_grab = false;
  const char* path_label = "none";
  if (grab_window != None) {
    motion_target = em_x11_window_find(dpy, grab_window);
    if (!motion_target || !motion_target->mapped)
      return;
    via_grab = true;
    path_label = "implicit_grab";
  } else {
    motion_target = em_x11_window_find(dpy, window);
    if (motion_target) {
      int unused_lx = 0, unused_ly = 0;
      motion_target = walk_up_for_mask(dpy,
                                       motion_target,
                                       x_root,
                                       y_root,
                                       PointerMotionMask | ButtonMotionMask,
                                       &unused_lx,
                                       &unused_ly);
      path_label = "hint+walk";
    } else if (active_grab) {
      motion_target = em_x11_window_find(dpy, active_grab_window);
      path_label = motion_target ? "active_grab_fb" : "none";
    } else {
      int lx_fb = 0, ly_fb = 0;
      motion_target = hit_test(dpy,
                               x_root,
                               y_root,
                               PointerMotionMask | ButtonMotionMask,
                               &lx_fb,
                               &ly_fb);
      path_label = motion_target ? "hit_test" : "none";
    }
    if (!motion_target)
      return;
  }
  /* Mask gate removed: while matching xserver/dix/events.c semantics,
   * filtering MotionNotify by PointerMotionMask|ButtonMotionMask broke
   * tcldide standalone demos where ttk widgets (notebook, menubutton)
   * rely on MotionNotify for hover/cursor tracking but select
   * EnterWindowMask|LeaveWindowMask rather than PointerMotionMask.
   * tcl/tk safely ignores MotionNotify it didn't select for, so
   * pushing it unconditionally is harmless. The grab bypass that was
   * paired with this gate (via_grab/active_grab) is also removed --
   * without the gate there's nothing to bypass. */

  /* Decision-path diagnostic trace. Toggled via
   * `Module['emX11Debug'].traceCMot` (set from DevTools). */
  EM_ASM(
    {
      var d = Module['emX11Debug'];
      if (d && d.traceCMot) {
        console.log('[c-mot] conn=' + $0 + ' rx=' + $1 + ' ry=' + $2 +
                    ' hint=' + ($3 >>> 0) + ' path=' +
                    ($4 != null ? UTF8ToString($4) : '?') + ' target=' +
                    ($5 >>> 0) + ' activeGrab=' + ($6 ? 'Y' : 'N') +
                    ' targetMask=0x' + ($7 >>> 0).toString(16) +
                    ' implicitGrab=' + ($8 ? 'Y' : 'N'));
      }
    },
    dpy->conn_id,
    x_root,
    y_root,
    window,
    path_label,
    motion_target->id,
    active_grab ? 1 : 0,
    (unsigned long)motion_target->event_mask,
    via_grab ? 1 : 0);

  int ax = 0, ay = 0, depth;
  window_abs_origin(dpy, motion_target, &ax, &ay, &depth);

  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xmotion.type = MotionNotify;
  ev.xmotion.display = dpy;
  ev.xmotion.window = motion_target->id;
  ev.xmotion.x = x_root - ax;
  ev.xmotion.y = y_root - ay;
  ev.xmotion.x_root = x_root;
  ev.xmotion.y_root = y_root;
  ev.xmotion.state = state;
  ev.xmotion.root = dpy->screens[0].root;
  ev.xmotion.is_hint = NotifyNormal;
  ev.xmotion.same_screen = True;
  ev.xmotion.time = event_now();
  em_x11_event_queue_push(dpy, &ev);
}

EMSCRIPTEN_KEEPALIVE
void em_x11_push_key_event_kc(int type,
                              Window window,
                              unsigned int keycode,
                              unsigned int keysym,
                              unsigned int state,
                              int x,
                              int y);

EMSCRIPTEN_KEEPALIVE
void em_x11_push_key_event(int type,
                           Window window,
                           unsigned int keysym,
                           unsigned int state,
                           int x,
                           int y) {
  /* Legacy entry point: only the keysym is supplied; we derive a
   * synthetic keycode by reverse-looking-up the keysym in
   * keysym_table. Kept so external consumers built against an older
   * em-x11 still link. New host bridges should call
   * em_x11_push_key_event_kc which threads the physical keycode
   * through from the browser's KeyboardEvent.code. */
  Display* dpy = em_x11_get_display();
  KeyCode kc = em_x11_keysym_to_keycode(dpy, (KeySym)keysym);
  em_x11_push_key_event_kc(type, window, (unsigned int)kc, keysym, state, x, y);
}

EMSCRIPTEN_KEEPALIVE
void em_x11_push_key_event_kc(int type,
                              Window window,
                              unsigned int keycode,
                              unsigned int keysym,
                              unsigned int state,
                              int x,
                              int y) {
  Display* dpy = em_x11_get_display();
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xkey.type = type;
  ev.xkey.display = dpy;
  ev.xkey.window = window;
  ev.xkey.x = x;
  ev.xkey.y = y;
  ev.xkey.x_root = x;
  ev.xkey.y_root = y;
  ev.xkey.state = state;
  ev.xkey.keycode = (KeyCode)keycode;
  /* Suppress browser key repeat for modifier keys. Browsers fire
   * keydown repeatedly for all keys including Control/Shift/Alt/etc.,
   * but real X servers never autorepeat modifiers. Without this gate,
   * repeated Control_L KeyPress events match catch-all <Key> translations
   * (self-insert) and flood the event loop. */

  if (type == KeyPress && keycode < 256 && is_modifier_keysym(keysym)) {
    int byte = keycode / 8;
    int bit = keycode % 8;
    if (down_keycodes[byte] & (1 << bit))
      return;
    down_keycodes[byte] |= (unsigned char)(1 << bit);
  }
  if (type == KeyRelease && keycode < 256) {
    int byte = keycode / 8;
    int bit = keycode % 8;
    down_keycodes[byte] &= (unsigned char)~(1 << bit);
  }
  ev.xkey.root = dpy->screens[0].root;
  ev.xkey.same_screen = True;
  ev.xkey.time = event_now();
  /* Make sure keysym_table[kc] reflects the keysym we just dispatched
   * so subsequent XLookupKeysym / XkbKeycodeToKeysym calls for this
   * physical key return the right value -- including layout-specific
   * keysyms patched in after Display init by the host layoutmap. */
  if (keycode > 0 && keycode < 256 && keysym != 0) {
    dpy->keysym_table[keycode] = (KeySym)keysym;
  }
  /* Record the slot BEFORE push -- em_x11_event_queue_push will write
   * to dpy->event_tail then advance it. The text snapshot has to land
   * in the same slot. */
  unsigned int slot = dpy->event_tail;
  if (em_x11_event_queue_push(dpy, &ev)) {
    em_x11_xim_capture_key_text(dpy, slot);
  } else {
    /* Queue full: drop the staged text too so the next successful
     * push doesn't claim text that belonged to the dropped event. */
    dpy->pending_key_text[0] = '\0';
    dpy->pending_key_text_len = 0;
  }
}

EMSCRIPTEN_KEEPALIVE
void em_x11_push_visibility_notify(Window window, int state) {
  Display* dpy = em_x11_get_display();
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xvisibility.type = VisibilityNotify;
  ev.xvisibility.display = dpy;
  ev.xvisibility.window = window;
  ev.xvisibility.state = state;
  em_x11_event_queue_push(dpy, &ev);
}

EMSCRIPTEN_KEEPALIVE
void em_x11_push_expose_event(Window window, int x, int y, int w, int h) {
  Display* dpy = em_x11_get_display();
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xexpose.type = Expose;
  ev.xexpose.display = dpy;
  ev.xexpose.window = window;
  ev.xexpose.x = x;
  ev.xexpose.y = y;
  ev.xexpose.width = w;
  ev.xexpose.height = h;
  em_x11_event_queue_push(dpy, &ev);
}

/* -- Substructure redirect dispatch ---------------------------------------
 *
 * The Host ccall's these on the holder module whenever a redirect would
 * normally have been served by the X server. They construct the canonical
 * XMapRequestEvent (etc.) and push it onto the holder's queue so the WM's
 * normal event loop picks it up via XNextEvent (x11protocol.txt §1592).
 */

EMSCRIPTEN_KEEPALIVE
void em_x11_push_map_request(Window parent, Window window) {
  Display* dpy = em_x11_get_display();
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xmaprequest.type = MapRequest;
  ev.xmaprequest.display = dpy;
  ev.xmaprequest.parent = parent;
  ev.xmaprequest.window = window;
  em_x11_event_queue_push(dpy, &ev);
}

/* Cross-connection ReparentNotify delivery. Called by the Host on the
 * window's *owner* module after a (typically WM-issued) XReparentWindow
 * succeeds. Two jobs in one entry point:
 *
 *   1. Update the local EmxWindow shadow unconditionally. The owner did
 *      not issue the reparent (twm did, on its own display), so its
 *      shadow still has the pre-reparent parent/x/y. Without this fix,
 *      window_abs_origin walks the stale chain to root and reports the
 *      pre-reparent absolute coords; ButtonPress/Motion get translated
 *      with the wrong offset, hover/click hit the wrong widget, and
 *      Tk/Xt's redraw paths (which read x,y from local shadow) draw at
 *      the wrong place too. See project_em_x11_mask_gating.md for the
 *      adjacent crash this enables, and the session notes for why the
 *      shadow fix has to be unconditional rather than mask-gated.
 *
 *   2. Synthesise the ReparentNotify XEvent and push it -- but only if
 *      the owner actually selected StructureNotifyMask on the window
 *      (or SubstructureNotifyMask on the new parent). Matches dix's
 *      DeliverEvents gating; Xt's Shell widget selects StructureNotify
 *      on its shell, so it does receive this. */
EMSCRIPTEN_KEEPALIVE
void em_x11_push_reparent_notify(Window window, Window parent, int x, int y) {
  Display* dpy = em_x11_get_display();
  EmxWindow* win = em_x11_window_find(dpy, window);
  if (win) {
    win->parent = parent;
    win->x = x;
    win->y = y;
  }

  /* Mask gate: same shape as window.c::wants_structure -- StructureNotify
   * on the window itself OR SubstructureNotify on the new parent. */
  bool wants = false;
  if (win && (win->event_mask & StructureNotifyMask))
    wants = true;
  if (!wants && parent != None) {
    EmxWindow* p = em_x11_window_find(dpy, parent);
    if (p && (p->event_mask & SubstructureNotifyMask))
      wants = true;
  }
  if (!wants)
    return;

  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xreparent.type = ReparentNotify;
  ev.xreparent.display = dpy;
  ev.xreparent.event = window;
  ev.xreparent.window = window;
  ev.xreparent.parent = parent;
  ev.xreparent.x = x;
  ev.xreparent.y = y;
  ev.xreparent.override_redirect = win ? win->override_redirect : False;
  em_x11_event_queue_push(dpy, &ev);
}

/* Cross-connection ConfigureNotify delivery. Called by the Host on the
 * window's *owner* module after a (typically WM-issued) XConfigureWindow
 * / XMoveResizeWindow / XResizeWindow / XMoveWindow on the owner's
 * shell. Mirrors em_x11_push_reparent_notify:
 *
 *   1. Update the local EmxWindow shadow unconditionally so subsequent
 *      drawing / hit-testing on the owner uses the correct geometry.
 *      Without this, Tk/Xt/Xlib clients keep painting and propagating
 *      events with their stale pre-resize size.
 *
 *   2. Synthesise the ConfigureNotify XEvent and push it -- mask-gated
 *      on StructureNotifyMask on the window itself, or
 *      SubstructureNotifyMask on the parent (mirrors dix's DeliverEvents).
 *      Xt's Shell widget and Tk's TopLevel both select StructureNotify
 *      on their shells, which is what triggers their re-layout pass. */
EMSCRIPTEN_KEEPALIVE
void em_x11_push_configure_notify(
  Window window, int x, int y, int width, int height, int border_width) {
  Display* dpy = em_x11_get_display();
  EmxWindow* win = em_x11_window_find(dpy, window);
  if (win) {
    win->x = x;
    win->y = y;
    win->width = (unsigned int)width;
    win->height = (unsigned int)height;
    win->border_width = (unsigned int)border_width;
  }

  bool wants = false;
  if (win && (win->event_mask & StructureNotifyMask))
    wants = true;
  if (!wants && win && win->parent != None) {
    EmxWindow* p = em_x11_window_find(dpy, win->parent);
    if (p && (p->event_mask & SubstructureNotifyMask))
      wants = true;
  }
  if (!wants)
    return;

  /* Bump request serial so Tk/Xt's WaitForConfigureNotify accepts the
   * synthetic event on the first poll. The owner did not issue the
   * configure, so its dpy->request hasn't moved; matching what
   * notify_js_reconfigure does for the local-issuer case. */
  dpy->request++;

  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xconfigure.type = ConfigureNotify;
  ev.xconfigure.serial = dpy->request;
  ev.xconfigure.send_event = False;
  ev.xconfigure.display = dpy;
  ev.xconfigure.event = window;
  ev.xconfigure.window = window;
  ev.xconfigure.x = x;
  ev.xconfigure.y = y;
  ev.xconfigure.width = width;
  ev.xconfigure.height = height;
  ev.xconfigure.border_width = border_width;
  ev.xconfigure.above = None;
  ev.xconfigure.override_redirect = win ? win->override_redirect : False;
  em_x11_event_queue_push(dpy, &ev);
}

/* Cross-connection DestroyNotify dispatch. Called by the Host when a
 * client connection closes (process exit, XCloseDisplay) so the WM
 * watching that client's parent (typically twm holding SubstructureNotify
 * on its frames) tears down its TwmWindow record + frame. Without this,
 * the dead client's frame lingers as a transparent outline -- twm has
 * no way to learn the inner window is gone otherwise.
 *
 * `event_window` is the parent (the observer); `window` is the dying
 * window. Mask gate matches dix's DeliverEvents: SubstructureNotify
 * on the parent. We don't model the StructureNotify-on-window path
 * here because by the time this fires, the window's own owner is
 * already gone -- nobody on this side cares. */
EMSCRIPTEN_KEEPALIVE
void em_x11_push_destroy_notify(Window window, Window event_window) {

  Display* dpy = em_x11_get_display();
  EmxWindow* evw = em_x11_window_find(dpy, event_window);
  /* When the event_window is the destroyed window itself (StructureNotify
   * path) and the window is a foreign cross-conn window that isn't in our
   * local EmxWindow table, trust the host — it only calls us when this
   * connection subscribed to StructureNotifyMask.  Push unconditionally.
   * For the parent path (SubstructureNotify), we need local shadow to
   * verify the mask, so bail if not found. */
  if (!evw) {
    if (event_window == window) {

      /* fall through to push */
    } else {

      return;
    }
  } else {
    /* Real X11 semantics: DestroyNotify is sent to the window itself if it
     * selected StructureNotifyMask, AND to the parent if it selected
     * SubstructureNotifyMask.  twm selects StructureNotifyMask on the
     * client window (not SubstructureNotifyMask on its frame). */
    if (event_window == window) {
      if (!(evw->event_mask & StructureNotifyMask)) {

        return;
      }
    } else {
      if (!(evw->event_mask & SubstructureNotifyMask)) {

        return;
      }
    }
  }

  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xdestroywindow.type = DestroyNotify;
  ev.xdestroywindow.display = dpy;
  ev.xdestroywindow.event = event_window;
  ev.xdestroywindow.window = window;
  em_x11_event_queue_push(dpy, &ev);
}

/* Cross-connection ShapeNotify dispatch. Called by the Host when a window's
 * shape changes and another connection has selected ShapeNotifyMask on it.
 * Mirrors the ShapeNotify event layout in shapeproto.h: eventBase (64) +
 * ShapeNotify (0) = type 64. */
EMSCRIPTEN_KEEPALIVE
void em_x11_push_shape_notify(Window window,
                              int kind,
                              int x,
                              int y,
                              unsigned int width,
                              unsigned int height,
                              Bool shaped) {
  Display* dpy = em_x11_get_display();
  EmxWindow* win = em_x11_window_find(dpy, window);
  /* Gate when the window is local: only deliver if the owner selected
   * ShapeNotifyMask. When win==NULL the window is foreign and the Host
   * already filtered subscribers before ccalling us. */
  if (win && !(win->shape_event_mask & ShapeNotifyMask))
    return;

  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  XShapeEvent* sev = (XShapeEvent*)&ev;
  sev->type = 64 + ShapeNotify;
  sev->serial = dpy->request;
  sev->send_event = False;
  sev->display = dpy;
  sev->window = window;
  sev->kind = kind;
  sev->x = x;
  sev->y = y;
  sev->width = width;
  sev->height = height;
  sev->time = event_now();
  sev->shaped = shaped;
  em_x11_event_queue_push(dpy, &ev);
}

/* -- Passive input grabs -- */

int XGrabButton(Display* dpy,
                unsigned int button,
                unsigned int modifiers,
                Window grab_window,
                Bool owner_events,
                unsigned int event_mask,
                int pointer_mode,
                int keyboard_mode,
                Window confine_to,
                Cursor cursor) {
  (void)dpy;
  em_x11_js_grab_button(grab_window,
                        button,
                        modifiers,
                        owner_events ? 1 : 0,
                        event_mask,
                        pointer_mode,
                        keyboard_mode,
                        confine_to,
                        cursor);
  return 1;
}

int XUngrabButton(Display* dpy,
                  unsigned int button,
                  unsigned int modifiers,
                  Window grab_window) {
  (void)dpy;
  em_x11_js_ungrab_button(grab_window, button, modifiers);
  return 1;
}

int XGrabKey(
  Display* a, int b, unsigned int c, Window d, Bool e, int f, int g) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)e;
  (void)f;
  (void)g;
  return 1;
}

int XUngrabKeyboard(Display* dpy, Time t) {
  (void)dpy;
  (void)t;
  return 1;
}

int XUngrabPointer(Display* dpy, Time t) {
  (void)t;
  dpy->request++;
  em_x11_reset_implicit_grab();
  active_grab = false;
  active_grab_window = None;
  active_grab_event_mask = 0;
  em_x11_js_set_grab_cursor(0);
  em_x11_js_ungrab_pointer();
  return 1;
}

int XGrabPointer(Display* dpy,
                 Window grab_window,
                 Bool owner_events,
                 unsigned int event_mask,
                 int pointer_mode,
                 int keyboard_mode,
                 Window confine_to,
                 Cursor cursor,
                 Time t) {
  (void)pointer_mode;
  (void)keyboard_mode;
  (void)confine_to;
  (void)t;
  active_grab_event_mask = event_mask;
  EM_ASM(
    {
      var d = Module['emX11Debug'];
      if (d && d.traceGrab) {
        console.log('[c-grab] XGrabPointer conn=' + $0 + ' grab_win=' +
                    ($1 >>> 0) + ' owner_events=' + $2 + ' cursor=' + $3);
      }
    },
    dpy->conn_id,
    grab_window,
    owner_events ? 1 : 0,
    (unsigned int)cursor);
  dpy->request++;
  em_x11_reset_implicit_grab();
  active_grab = true;
  active_grab_window = grab_window;
  em_x11_js_set_grab_cursor((unsigned int)cursor);
  em_x11_js_grab_pointer(
    (unsigned int)dpy->conn_id, grab_window, owner_events ? 1 : 0);
  return GrabSuccess;
}

int XWarpPointer(Display* dpy,
                 Window src_w,
                 Window dest_w,
                 int src_x,
                 int src_y,
                 unsigned int src_width,
                 unsigned int src_height,
                 int dest_x,
                 int dest_y) {
  (void)dpy;
  (void)src_w;
  (void)dest_w;
  (void)src_x;
  (void)src_y;
  (void)src_width;
  (void)src_height;
  (void)dest_x;
  (void)dest_y;
  return 1;
}

int XUngrabKey(Display* dpy,
               int keycode,
               unsigned int modifiers,
               Window grab_window) {
  (void)dpy;
  (void)keycode;
  (void)modifiers;
  (void)grab_window;
  return 1;
}

int XGrabKeyboard(Display* dpy,
                  Window grab_window,
                  Bool owner_events,
                  int pointer_mode,
                  int keyboard_mode,
                  Time t) {
  (void)dpy;
  (void)grab_window;
  (void)owner_events;
  (void)pointer_mode;
  (void)keyboard_mode;
  (void)t;
  return GrabSuccess;
}

/* -- XQueryPointer -- */

Bool XQueryPointer(Display* dpy,
                   Window w,
                   Window* root_return,
                   Window* child_return,
                   int* root_x_return,
                   int* root_y_return,
                   int* win_x_return,
                   int* win_y_return,
                   unsigned int* mask_return) {
  int px = 0, py = 0;
  em_x11_js_pointer_xy(&px, &py);
  int wx = px, wy = py;
  if (w != None) {
    int origin[EM_X11_ABS_ORIGIN_SIZE] = {0};
    em_x11_js_get_window_abs_origin(w, origin);
    if (origin[EM_X11_ABS_ORIGIN_PRESENT]) {
      wx = px - origin[EM_X11_ABS_ORIGIN_AX];
      wy = py - origin[EM_X11_ABS_ORIGIN_AY];
    }
  }
  EM_ASM(
    {
      var d = Module['emX11Debug'];
      if (d && d.traceQp) {
        console.log('[c-qp] conn=' + $0 + ' win=' + $1 + ' root=(' + $2 + ',' +
                    $3 + ')' + ' local=(' + $4 + ',' + $5 + ')');
      }
    },
    dpy->conn_id,
    w,
    px,
    py,
    wx,
    wy);
  if (root_return)
    *root_return = dpy->screens[0].root;
  if (child_return)
    *child_return = None;
  if (root_x_return)
    *root_x_return = px;
  if (root_y_return)
    *root_y_return = py;
  if (win_x_return)
    *win_x_return = wx;
  if (win_y_return)
    *win_y_return = wy;
  if (mask_return)
    *mask_return = 0;
  return True;
}

/* -- XInput2 stubs -- */

Status XIQueryVersion(Display* dpy,
                      int* major_version_inout,
                      int* minor_version_inout) {
  (void)dpy;
  (void)major_version_inout;
  (void)minor_version_inout;
  return BadRequest;
}

int XISelectEvents(Display* dpy,
                   Window win,
                   XIEventMask* masks,
                   int num_masks) {
  (void)dpy;
  (void)win;
  (void)masks;
  (void)num_masks;
  return Success;
}
