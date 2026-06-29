/*
 * Input dispatch — button / motion / key event entry points, grab
 * lifecycle, and cross-connection notification delivery.
 *
 * Rewritten 2026-06-27 to faithfully replicate xserver/dix/events.c
 * (DeliverGrabbedEvent, ActivateImplicitGrab, DeliverEventsToWindow)
 * + dix/enterleave.c (DoEnterLeaveEvents, spriteTrace).
 *
 * Single-pointer / single-seat simplifications vs xorg:
 *   - No MPX, no XI2, no touch, no panoramix
 *   - No sync-grab freeze/thaw (all async)
 *   - TS host provides z-order-correct deepest window hint via
 *     findWindowAt; C side uses it for spriteTrace + delivery
 *   - Implicit-grab start/end bridged to TS for module routing
 */

#include "em_x11_internal.h"
#include "em_x11_meta_layout.h"
#include "enterleave.h"

#include <X11/extensions/XInput2.h>
#include <X11/extensions/shape.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

/* ================================================================== */
/*  Grab state (single source of truth)                                */
/* ================================================================== */

static struct {
  bool active;
  Window window;
  Bool ownerEvents;
  unsigned int eventMask;
  Cursor cursor;
  bool tsSynced; /* XGrabPointer called em_x11_js_grab_pointer */
} activeGrab;

static struct {
  Window window;
  unsigned int buttonCount;
} implicitGrab;

/* Mirrors xorg's device->button->buttonsDown: tracks pressed buttons
 * independently of grab state so we can detect "all buttons released"
 * and deactivate the pointer grab (DeactivatePointerGrab).  Without
 * this, XGrabPointer clears implicitGrab on activation, so the
 * ButtonRelease cleanup at line 261 never fires and activeGrab leaks. */
static unsigned int buttonsDown = 0;

/* Accessors for enterleave.c */
bool active_grab_active(void) { return activeGrab.active; }
unsigned int active_grab_event_mask_get(void) { return activeGrab.eventMask; }
Window active_grab_window_get(void) { return activeGrab.window; }
bool active_grab_owner_events_get(void) { return activeGrab.ownerEvents; }

/* ================================================================== */
/*  Key modifier state (simplified — no XKB)                           */
/* ================================================================== */

static unsigned int modifier_state = 0;

unsigned int key_modifier_state(void) { return modifier_state; }

/* Browser autorepeat-suppression for modifier keys. */
static unsigned char down_keycodes[32];

static int is_modifier_keysym(KeySym ks) {
  return (ks >= 0xFFE1 && ks <= 0xFFEE);
}

/* ================================================================== */
/*  Helpers                                                            */
/* ================================================================== */

static unsigned long event_serial = 1;

static void event_set_serial(XEvent* ev) { ev->xany.serial = event_serial++; }

static Time event_now(void) {
  return (Time)(unsigned long)emscripten_get_now();
}

/* Absolute origin of a local window.  Shared with enterleave.c which
 * has its own copy (they can't share a static easily without a header
 * or merging TUs). */
static void local_window_abs_origin(
  Display* dpy, EmX11Window* w, int* ax_out, int* ay_out, int* depth_out) {
  int ax = 0, ay = 0, depth = 0;
  EmX11Window* cur = w;
  for (int guard = 0; cur && guard < dpy->window_count; guard++, depth++) {
    ax += cur->x;
    ay += cur->y;
    if (cur->parent == None || cur->parent == cur->id)
      break;
    EmX11Window* parent = em_x11_window_find(dpy, cur->parent);
    if (!parent) {
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

/* Push a button event onto the queue.  Extracted so both the normal
 * and grabbed-delivery paths use the same event shape. */
static void push_button_xevent(Display* dpy,
                               int type,
                               Window win,
                               int lx,
                               int ly,
                               int rx,
                               int ry,
                               unsigned int button,
                               unsigned int state,
                               Time now) {
  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xbutton.type = type;
  ev.xany.serial = ++dpy->request;
  ev.xbutton.display = dpy;
  ev.xbutton.window = win;
  ev.xbutton.x = lx;
  ev.xbutton.y = ly;
  ev.xbutton.x_root = rx;
  ev.xbutton.y_root = ry;
  ev.xbutton.root = dpy->screens[0].root;
  ev.xbutton.button = button;
  ev.xbutton.state = state;
  ev.xbutton.same_screen = True;
  ev.xbutton.time = now;
  em_x11_event_queue_push(dpy, &ev);
}

/* Walk spriteTrace bottom-up, find first window whose effective mask
 * contains `need_mask`.  Returns the window ID and fills local coords.
 * If nothing in the trace selects the mask, returns spriteWin as
 * fallback (so the event is never silently dropped). */
static Window deliver_window_from_sprite(
  Display* dpy, long need_mask, int rx, int ry, int* lx_out, int* ly_out) {
  int good = em_x11_sprite_trace_good();
  for (int i = good - 1; i >= 0; i--) {
    Window w = em_x11_sprite_trace_get(i);
    EmX11Window* ew = em_x11_window_find(dpy, w);
    if (!ew)
      continue;
    unsigned int eff = ew->event_mask;
    if (activeGrab.active && activeGrab.ownerEvents)
      eff |= activeGrab.eventMask;
    if (eff & need_mask) {
      int ax, ay, depth;
      local_window_abs_origin(dpy, ew, &ax, &ay, &depth);
      *lx_out = rx - ax;
      *ly_out = ry - ay;
      return w;
    }
  }
  Window fb = em_x11_sprite_win();
  if (fb != None) {
    EmX11Window* ew = em_x11_window_find(dpy, fb);
    if (ew) {
      int ax, ay, depth;
      local_window_abs_origin(dpy, ew, &ax, &ay, &depth);
      *lx_out = rx - ax;
      *ly_out = ry - ay;
      return fb;
    }
  }
  return None;
}

/* ================================================================== */
/*  DeliverGrabbedEvent — mirrors xorg events.c:4419                   */
/*                                                                    */
/*  owner_events=True: deliver normally to the window under the        */
/*    pointer via sprite-trace walk.  The sprite walk's mask check     */
/*    ORs the grab mask so the grabbing client's windows are visible.  */
/*  owner_events=False or no window took the event: deliver ONLY to    */
/*    grab->window (xorg DeliverOneGrabbedEvent).                      */
/*                                                                    */
/*  xorg never delivers to BOTH the pointer window AND the grab        */
/*  window for the same event (no "dual delivery").                    */
/* ================================================================== */

static void DeliverGrabbedEvent(Display* dpy,
                                int type,
                                Window deepest_hint,
                                int ts_lx,
                                int ts_ly,
                                int rx,
                                int ry,
                                unsigned int button,
                                unsigned int state,
                                Time now) {
  long need_mask = (type == ButtonPress) ? ButtonPressMask : ButtonReleaseMask;
  int lx = 0, ly = 0;
  Window normal_target = None;

  if (activeGrab.ownerEvents) {
    normal_target =
      deliver_window_from_sprite(dpy, need_mask, rx, ry, &lx, &ly);
    if (normal_target != None) {
      if (normal_target == deepest_hint) {
        lx = ts_lx;
        ly = ts_ly;
      }
      push_button_xevent(
        dpy, type, normal_target, lx, ly, rx, ry, button, state, now);
    }
  }

  if (normal_target == None) {
    EmX11Window* gw = em_x11_window_find(dpy, activeGrab.window);
    if (gw) {
      int ax, ay, depth;
      local_window_abs_origin(dpy, gw, &ax, &ay, &depth);
      int flx, fly;
      if (activeGrab.window == deepest_hint) {
        flx = ts_lx;
        fly = ts_ly;
      } else {
        flx = rx - ax;
        fly = ry - ay;
      }
      push_button_xevent(
        dpy, type, activeGrab.window, flx, fly, rx, ry, button, state, now);
    }
  }
}

/* ================================================================== */
/*  em_x11_push_button_event — main button entry point                 */
/* ================================================================== */

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
  Time now = event_now();
  int lx = 0, ly = 0;
  Window delivery_win = None;

  /* 1. Track pressed buttons independently of grab state.
   *    Mirrors xorg's device->button->buttonsDown. */
  if (type == ButtonPress)
    buttonsDown++;
  else if (type == ButtonRelease && buttonsDown > 0)
    buttonsDown--;

  /* 2. Update sprite and fire crossing events.
   *    Mirrors xorg's CheckMotion: NotifyGrab during active grab,
   *    NotifyNormal otherwise.  NotifyUngrab is only for the crossing
   *    events fired at grab deactivation time (XUngrabPointer). */
  int mode = activeGrab.active ? NotifyGrab : NotifyNormal;
  em_x11_update_sprite(dpy, window, x_root, y_root, mode);

  /* 3. If any grab is active (passive, explicit, or implicit),
   *    DeliverGrabbedEvent handles routing.  xorg: implicit grab IS the
   *    device grab — all events during grab go through DeliverGrabbedEvent. */
  if (activeGrab.active) {
    DeliverGrabbedEvent(
      dpy, type, window, x, y, x_root, y_root, button, state, now);

    /* Implicit grab deactivation on last ButtonRelease. */
    if (type == ButtonRelease && implicitGrab.window != None) {
      if (implicitGrab.buttonCount > 0)
        implicitGrab.buttonCount--;
      if (implicitGrab.buttonCount == 0) {
        implicitGrab.window = None;
        em_x11_js_implicit_grab_end((unsigned int)dpy->conn_id);
      }
    }

    /* Deactivate any pointer grab when all buttons are released.
     * Mirrors xorg's DeactivatePointerGrab.  This is load-bearing:
     * XGrabPointer (called by Xt passive-grab activation) clears
     * implicitGrab, so the ButtonRelease cleanup above won't catch
     * it.  Without this, activeGrab leaks and crossing events use
     * NotifyGrab mode forever, which breaks posted-menu hover
     * (Motif RowColumn's <EnterWindow>Normal:MenuEnter only matches
     * NotifyNormal events). */
    if (type == ButtonRelease && buttonsDown == 0) {
      /* C-side activeGrab may have been set by XGrabPointer (explicit) or
       * implicit grab (ButtonPress).  Only the explicit path has a TS-side
       * activePointerGrab mirror (set by em_x11_js_grab_pointer).  If we
       * don't sync here, XUngrabPointer's later call will see active==false
       * and return early, leaking the TS grab across menu interactions and
       * misrouting the next click's keyboard focus. */
      if (activeGrab.tsSynced) {
        em_x11_js_ungrab_pointer();
        activeGrab.tsSynced = false;
      }
      activeGrab.active = false;
      activeGrab.window = None;
      activeGrab.ownerEvents = False;
      activeGrab.eventMask = 0;
    }
    return;
  }

  /* 3. Normal delivery: walk spriteTrace for mask match.
   *    The sprite walk determines the correct delivery window (mask gate).
   *    For local coordinates, trust the TS-provided x,y when the sprite
   *    walk agrees with the hint — the TS side has correct stacking-aware
   *    window positions; the EmX11Window chain may have stale coordinates. */
  long need_mask = (type == ButtonPress) ? ButtonPressMask : ButtonReleaseMask;
  delivery_win =
    deliver_window_from_sprite(dpy, need_mask, x_root, y_root, &lx, &ly);
  if (delivery_win == None) {
    return;
  }
  if (delivery_win == window) {
    lx = x;
    ly = y;
  }

  /* 4. Implicit grab activation on ButtonPress.
   *    xorg: ActivateImplicitGrab sets deviceGrab.grab so subsequent
   *    events route via DeliverGrabbedEvent.  We mirror this by setting
   *    activeGrab here, which makes step 2 catch subsequent events. */
  if (type == ButtonPress) {
    if (implicitGrab.buttonCount == 0) {
      EmX11Window* dw = em_x11_window_find(dpy, delivery_win);
      implicitGrab.window = delivery_win;
      activeGrab.active = true;
      activeGrab.window = delivery_win;
      activeGrab.ownerEvents =
        (dw && (dw->event_mask & OwnerGrabButtonMask)) ? True : False;
      activeGrab.eventMask = dw ? dw->event_mask : 0;
      activeGrab.cursor = None;
      activeGrab.tsSynced = false;
      ;
      em_x11_js_implicit_grab_start((unsigned int)dpy->conn_id, delivery_win);
    }
    implicitGrab.buttonCount++;
  }

  /* 5. Push the event */
  push_button_xevent(
    dpy, type, delivery_win, lx, ly, x_root, y_root, button, state, now);
}

/* ================================================================== */
/*  em_x11_push_motion_event — main motion entry point                 */
/* ================================================================== */

EMSCRIPTEN_KEEPALIVE
void em_x11_push_motion_event(
  Window window, int x, int y, int x_root, int y_root, unsigned int state) {
  Display* dpy = em_x11_get_display();

  /* 1. Update sprite — NotifyGrab during active grab, NotifyNormal otherwise
   *    (mirrors xorg CheckMotion which uses grab ? NotifyGrab : NotifyNormal)
   */
  int mode = activeGrab.active ? NotifyGrab : NotifyNormal;
  em_x11_update_sprite(dpy, window, x_root, y_root, mode);

  /* 2. Determine delivery window */
  Window delivery_win = None;
  int lx = 0, ly = 0;

  if (implicitGrab.window != None) {
    EmX11Window* gw = em_x11_window_find(dpy, implicitGrab.window);
    if (gw && gw->mapped) {
      delivery_win = implicitGrab.window;
      int ax, ay, depth;
      local_window_abs_origin(dpy, gw, &ax, &ay, &depth);
      lx = x_root - ax;
      ly = y_root - ay;
    }
  } else if (activeGrab.active) {
    long mmask = PointerMotionMask | ButtonMotionMask;
    delivery_win =
      deliver_window_from_sprite(dpy, mmask, x_root, y_root, &lx, &ly);
    if (delivery_win == None && !activeGrab.ownerEvents) {
      EmX11Window* gw = em_x11_window_find(dpy, activeGrab.window);
      if (gw) {
        delivery_win = activeGrab.window;
        int ax, ay, depth;
        local_window_abs_origin(dpy, gw, &ax, &ay, &depth);
        lx = x_root - ax;
        ly = y_root - ay;
      }
    }
  } else {
    long mmask = PointerMotionMask | ButtonMotionMask;
    delivery_win =
      deliver_window_from_sprite(dpy, mmask, x_root, y_root, &lx, &ly);
  }

  if (delivery_win == None)
    return;

  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.xmotion.type = MotionNotify;
  ev.xmotion.display = dpy;
  ev.xmotion.window = delivery_win;
  ev.xmotion.x = lx;
  ev.xmotion.y = ly;
  ev.xmotion.x_root = x_root;
  ev.xmotion.y_root = y_root;
  ev.xmotion.state = state;
  ev.xmotion.root = dpy->screens[0].root;
  ev.xmotion.is_hint = NotifyNormal;
  ev.xmotion.same_screen = True;
  ev.xmotion.time = event_now();
  em_x11_event_queue_push(dpy, &ev);
}

/* ================================================================== */
/*  Key events (largely unchanged)                                     */
/* ================================================================== */

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
  Display* dpy = em_x11_get_display();
  KeyCode kc = em_x11_keysym_to_keycode(dpy, (KeySym)keysym);
  em_x11_push_key_event_kc(type, window, (unsigned int)kc, keysym, state, x, y);
}

EMSCRIPTEN_KEEPALIVE
/* Called from JS when ButtonPress-driven focus changes, so the C-side
 * focus_window override doesn't redirect keys away from the widget the
 * user clicked on. Without this, init-time XSetInputFocus on the
 * toplevel permanently captures all key events. */
void em_x11_set_focus_window(Window w) {
  Display* dpy = em_x11_get_display();
  if (dpy)
    dpy->focus_window = w;
}

void em_x11_push_key_event_kc(int type,
                              Window window,
                              unsigned int keycode,
                              unsigned int keysym,
                              unsigned int state,
                              int x,
                              int y) {
  Display* dpy = em_x11_get_display();

  /* Update modifier state tracking */
  if (type == KeyPress) {
    modifier_state = state;
  }

  /* Honour XSetInputFocus: if the server-side focus window was set to
   * a real window (not None/PointerRoot), route key events there.
   * Motif calls XSetInputFocus on the MenuShell when a posted menu opens;
   * the TS-side press-driven focus still points at the menu bar cascade
   * button, which has no arrow-key translations.  Without this override,
   * keyboard navigation in posted menus is dead. */
  if (dpy->focus_window != None && dpy->focus_window != PointerRoot) {
    window = dpy->focus_window;
  }

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

  if (keycode > 0 && keycode < 256 && keysym != 0)
    dpy->keysym_table[keycode] = (KeySym)keysym;

  unsigned int slot = dpy->event_tail;
  if (em_x11_event_queue_push(dpy, &ev)) {
    em_x11_xim_capture_key_text(dpy, slot);
  } else {
    dpy->pending_key_text[0] = '\0';
    dpy->pending_key_text_len = 0;
  }
}

/* ================================================================== */
/*  Visibility / Expose (unchanged)                                    */
/* ================================================================== */

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

/* ================================================================== */
/*  Substructure-redirect / cross-connection dispatch (largely        */
/*  unchanged from old events.c)                                       */
/* ================================================================== */

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

EMSCRIPTEN_KEEPALIVE
void em_x11_push_reparent_notify(Window window, Window parent, int x, int y) {
  Display* dpy = em_x11_get_display();
  EmX11Window* win = em_x11_window_find(dpy, window);
  if (win) {
    win->parent = parent;
    win->x = x;
    win->y = y;
  }

  bool wants = false;
  if (win && (win->event_mask & StructureNotifyMask))
    wants = true;
  if (!wants && parent != None) {
    EmX11Window* p = em_x11_window_find(dpy, parent);
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

EMSCRIPTEN_KEEPALIVE
void em_x11_push_configure_notify(
  Window window, int x, int y, int width, int height, int border_width) {
  Display* dpy = em_x11_get_display();
  EmX11Window* win = em_x11_window_find(dpy, window);
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
    EmX11Window* p = em_x11_window_find(dpy, win->parent);
    if (p && (p->event_mask & SubstructureNotifyMask))
      wants = true;
  }
  if (!wants)
    return;

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

EMSCRIPTEN_KEEPALIVE
void em_x11_push_destroy_notify(Window window, Window event_window) {
  Display* dpy = em_x11_get_display();
  EmX11Window* evw = em_x11_window_find(dpy, event_window);
  if (!evw) {
    if (event_window == window) {
      /* fall through to push */
    } else {
      return;
    }
  } else {
    if (event_window == window) {
      if (!(evw->event_mask & StructureNotifyMask))
        return;
    } else {
      if (!(evw->event_mask & SubstructureNotifyMask))
        return;
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

EMSCRIPTEN_KEEPALIVE
void em_x11_push_shape_notify(Window window,
                              int kind,
                              int x,
                              int y,
                              unsigned int width,
                              unsigned int height,
                              Bool shaped) {
  Display* dpy = em_x11_get_display();
  EmX11Window* win = em_x11_window_find(dpy, window);
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

/* ================================================================== */
/*  Repoll (delegates to enterleave)                                   */
/* ================================================================== */

void em_x11_repoll_pointer_window(Display* dpy) {
  if (!dpy || !dpy->screens)
    return;
  int px = 0, py = 0;
  em_x11_js_pointer_xy(&px, &py);
  em_x11_update_sprite(dpy, None, px, py, NotifyNormal);
}

void em_x11_repoll_pointer_window_hint(Display* dpy, Window cur_hint) {
  if (!dpy)
    return;
  int px = 0, py = 0;
  em_x11_js_pointer_xy(&px, &py);
  em_x11_update_sprite(dpy, cur_hint, px, py, NotifyNormal);
}

/* ================================================================== */
/*  Passive input grabs (unchanged)                                    */
/* ================================================================== */

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

/* ================================================================== */
/*  Active pointer grab — rewritten with crossing events               */
/* ================================================================== */

int XUngrabPointer(Display* dpy, Time t) {
  (void)t;

  if (!activeGrab.active)
    return 1;

  Window old_grab_window = activeGrab.window;

  /* Clear implicit grab */
  if (implicitGrab.buttonCount > 0) {
    implicitGrab.window = None;
    implicitGrab.buttonCount = 0;
    em_x11_js_implicit_grab_end((unsigned int)dpy->conn_id);
  }

  activeGrab.active = false;
  activeGrab.window = None;
  activeGrab.eventMask = 0;
  activeGrab.ownerEvents = false;
  dpy->request++;

  /* Fire crossing events: grab window → current sprite */
  {
    int px = 0, py = 0;
    em_x11_js_pointer_xy(&px, &py);
    Window cur = em_x11_sprite_win();
    em_x11_do_enter_leave_events(
      dpy, old_grab_window, cur, NotifyUngrab, px, py);
  }

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

  /* Motif calls XGrabPointer twice during menu-bar cascade (MenuBarSelect
   * pre-grab + ChangeManaged post-grab). Skip bridge calls when the grab
   * is already active on the same window with the same parameters. */
  if (activeGrab.active && activeGrab.window == grab_window &&
      activeGrab.ownerEvents == (Bool)(owner_events != 0) &&
      activeGrab.eventMask == event_mask) {
    return GrabSuccess;
  }

  /* Reset implicit grab (mirrors xorg's ActivatePointerGrab) */
  if (implicitGrab.buttonCount > 0) {
    implicitGrab.window = None;
    implicitGrab.buttonCount = 0;
    em_x11_js_implicit_grab_end((unsigned int)dpy->conn_id);
  }

  activeGrab.active = true;
  activeGrab.window = grab_window;
  activeGrab.ownerEvents = (owner_events != 0);
  activeGrab.eventMask = event_mask;
  activeGrab.cursor = cursor;
  activeGrab.tsSynced = true;
  dpy->request++;

  /* Fire crossing events: current sprite → grab window (NotifyGrab) */
  {
    int px = 0, py = 0;
    em_x11_js_pointer_xy(&px, &py);
    Window cur = em_x11_sprite_win();
    em_x11_do_enter_leave_events(dpy, cur, grab_window, NotifyGrab, px, py);
  }

  em_x11_js_set_grab_cursor((unsigned int)cursor);
  em_x11_js_grab_pointer(
    (unsigned int)dpy->conn_id, grab_window, owner_events ? 1 : 0);
  return GrabSuccess;
}

/* ================================================================== */
/*  Stubs (unchanged)                                                  */
/* ================================================================== */

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

/* XI2 stubs */
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
