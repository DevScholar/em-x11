/*
 * Sprite tracking and Enter/Leave event synthesis, faithfully replicating
 * xserver/dix/enterleave.c + the crossing portions of dix/events.c
 * (CoreEnterLeaveEvent, DoEnterLeaveEvents, CheckMotion).
 *
 * Single-pointer / single-seat simplifications vs xorg:
 *   - No MPX suppression (HasPointer / FirstPointerChild)
 *   - No DeviceEnterLeaveEvents / XI2 path
 *   - No XKB state in crossing events (simple modifier mask)
 *   - No enter/focus grabs
 *
 * Cross-connection awareness: em-x11 runs each X11 client in a separate
 * wasm module whose Display only knows its own windows.  The TS host
 * owns the full window tree.  Sprite-trace building and ancestor checks
 * delegate to JS bridges when the local EmxWindow table doesn't have the
 * answer.
 */

#include "enterleave.h"
#include "em_x11_internal.h"
#include "em_x11_meta_layout.h"
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Sprite state                                                       */
/* ------------------------------------------------------------------ */

#define MAX_SPRITE_TRACE 32

static Window spriteTrace[MAX_SPRITE_TRACE];
static int spriteTraceGood; /* number of valid entries          */
static Window spriteWin;    /* == spriteTrace[spriteTraceGood-1] */

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static Time event_now(void) {
  return (Time)(unsigned long)emscripten_get_now();
}

/* Absolute origin of a *local* window (must be in dpy->windows[]).
 * Mirrors the same-named static in events.c. */
static void window_abs_origin(
  Display* dpy, EmxWindow* w, int* ax_out, int* ay_out, int* depth_out) {
  int ax = 0, ay = 0, depth = 0;
  EmxWindow* cur = w;
  for (int guard = 0; cur && guard < dpy->window_count; guard++, depth++) {
    ax += cur->x;
    ay += cur->y;
    if (cur->parent == None || cur->parent == cur->id)
      break;
    EmxWindow* parent = em_x11_window_find(dpy, cur->parent);
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

/* Is `ancestor` in the parent chain of `descendant`?
 * Checks local EmxWindow table first; falls back to the JS host for
 * cross-connection relationships. */
int win_is_inferior_of(Display* dpy, Window descendant, Window ancestor) {
  if (descendant == None || ancestor == None || descendant == ancestor)
    return false;

  /* Try local table first */
  EmxWindow* cur = em_x11_window_find(dpy, descendant);
  while (cur) {
    if (cur->parent == ancestor)
      return true;
    if (cur->parent == None || cur->parent == cur->id)
      break;
    cur = em_x11_window_find(dpy, cur->parent);
  }

  /* Cross-connection fallback */
  return em_x11_js_is_ancestor(ancestor, descendant);
}

/* Nearest common ancestor of two windows.
 * Walks up from `b`, checking ancestry against `a` at each step. */
static Window common_ancestor(Display* dpy, Window a, Window b) {
  if (a == None || b == None)
    return None;
  if (a == b)
    return a;
  if (win_is_inferior_of(dpy, b, a))
    return a;
  if (win_is_inferior_of(dpy, a, b))
    return b;

  /* Walk up from b */
  Window cur = b;
  for (int guard = 0; guard < 128; guard++) {
    Window parent = em_x11_js_get_parent(cur);
    if (parent == None || parent == cur)
      break;
    if (win_is_inferior_of(dpy, a, parent))
      return parent;
    cur = parent;
  }
  return em_x11_js_common_ancestor(a, b);
}

/* ------------------------------------------------------------------ */
/*  Sprite-trace construction                                          */
/* ------------------------------------------------------------------ */

/* Build spriteTrace[0..good-1] where [0]==root and [good-1]==deepest.
 * Uses the TS-provided `deepest_hint` (from findWindowAt) as the
 * starting point.  For foreign-connection windows on the path, queries
 * the JS host for parent linkage.
 *
 * If `deepest_hint` is None or the chain can't be resolved, falls back
 * to the depth-based hit_test in events.c (via a repoll call). */
void em_x11_fill_sprite_trace(Display* dpy,
                              Window deepest_hint,
                              int rx,
                              int ry) {
  /* If hint is None, try a depth-based fallback */
  if (deepest_hint == None) {
    /* Walk dpy->windows[] for deepest mapped window at (rx,ry) */
    EmxWindow* best = NULL;
    int best_depth = -1;
    for (int i = 0; i < dpy->window_count; i++) {
      EmxWindow* w = &dpy->windows[i];
      if (!w->in_use || !w->mapped)
        continue;
      int ax, ay, depth;
      window_abs_origin(dpy, w, &ax, &ay, &depth);
      if (rx < ax || ry < ay || rx >= ax + (int)w->width ||
          ry >= ay + (int)w->height)
        continue;
      if (depth > best_depth) {
        best = w;
        best_depth = depth;
      }
    }
    deepest_hint = best ? best->id : dpy->screens[0].root;
  }

  /* Walk up from deepest_hint, collecting window IDs */
  Window chain[MAX_SPRITE_TRACE];
  int count = 0;
  Window cur = deepest_hint;
  for (int guard = 0; guard < MAX_SPRITE_TRACE; guard++) {
    if (cur == None)
      break;
    chain[count++] = cur;
    if (cur == dpy->screens[0].root)
      break; /* reached root */

    /* Try local table for parent */
    EmxWindow* ew = em_x11_window_find(dpy, cur);
    if (ew && ew->parent != None && ew->parent != cur) {
      cur = ew->parent;
      continue;
    }
    /* Cross-connection: ask JS host */
    Window p = em_x11_js_get_parent(cur);
    if (p == None || p == cur) {
      /* Can't continue — root not reached. Fallback: insert root at end. */
      if (cur != dpy->screens[0].root) {
        chain[count++] = dpy->screens[0].root;
      }
      break;
    }
    cur = p;
  }

  /* Reverse to get root→deepest order */
  spriteTraceGood = count;
  for (int i = 0; i < count; i++)
    spriteTrace[i] = chain[count - 1 - i];
  spriteWin = spriteTrace[spriteTraceGood - 1];

  EM_ASM(
    {
      var d = Module['emX11Debug'];
      if (d && d.traceSprite) {
        var ids = [];
        for (var i = 0; i < $0; i++) {
          ids.push((HEAPU32[($1 >> 2) + i]) >>> 0);
        }
        console.log('[sprite] trace=' + ids.join(' > ') + ' win=' +
                    (ids[ids.length - 1] >>> 0));
      }
    },
    spriteTraceGood,
    (int)spriteTrace);
}

/* convenience accessors for events.c */
int em_x11_sprite_trace_good(void) { return spriteTraceGood; }
Window em_x11_sprite_trace_get(int i) {
  return (i >= 0 && i < spriteTraceGood) ? spriteTrace[i] : None;
}
Window em_x11_sprite_win(void) { return spriteWin; }

/* ------------------------------------------------------------------ */
/*  Core crossing-event emitter                                        */
/* ------------------------------------------------------------------ */

/* Push a single EnterNotify or LeaveNotify event.
 * Mirrors xorg's CoreEnterLeaveEvent (events.c:4730), simplified for
 * single-pointer / single-seat:
 *   - mode is passed through (caller decides NotifyGrab vs NotifyNormal)
 *   - state from simple modifier tracking, no XKB
 *   - child is only set for Virtual crossings (passed by caller)
 *   - mask check: window's event_mask, plus active grab's mask if grabbed */
static void CoreEnterLeaveEvent(Display* dpy,
                                int type,
                                int mode,
                                int detail,
                                Window win,
                                Window child,
                                int rx,
                                int ry,
                                unsigned int state) {
  EmxWindow* ew = em_x11_window_find(dpy, win);
  if (!ew)
    return;

  long need = (type == EnterNotify) ? EnterWindowMask : LeaveWindowMask;

  /* Mirror xorg events.c:4748-4755:
   *   if (grab) {
   *       mask = (pWin == grab->window) ? grab->eventMask : 0;
   *       if (grab->ownerEvents)
   *           mask |= EventMaskForClient(pWin, rClient(grab));
   *   } else {
   *       mask = pWin->eventMask | wOtherEventMasks(pWin);
   *   }
   * em-x11 simplifications: wOtherEventMasks is always 0 (single
   * client per wasm module); EventMaskForClient == ew->event_mask. */
  unsigned int eff_mask;
  if (active_grab_active()) {
    eff_mask =
      (win == active_grab_window_get()) ? active_grab_event_mask_get() : 0;
    if (active_grab_owner_events_get())
      eff_mask |= ew->event_mask;
  } else {
    eff_mask = ew->event_mask;
  }

  if (!(eff_mask & need))
    return;

  int ax, ay, depth;
  window_abs_origin(dpy, ew, &ax, &ay, &depth);

  XEvent ev;
  memset(&ev, 0, sizeof(ev));
  ev.type = type;
  ev.xcrossing.display = dpy;
  ev.xcrossing.window = win;
  ev.xcrossing.root = dpy->screens[0].root;
  ev.xcrossing.x = rx - ax;
  ev.xcrossing.y = ry - ay;
  ev.xcrossing.x_root = rx;
  ev.xcrossing.y_root = ry;
  ev.xcrossing.mode = mode;
  ev.xcrossing.detail = detail;
  ev.xcrossing.same_screen = True;
  ev.xcrossing.focus = (win == dpy->focus_window);
  ev.xcrossing.state = state;
  ev.xcrossing.time = event_now();
  ev.xcrossing.subwindow = child;
  em_x11_event_queue_push(dpy, &ev);
}

/* ------------------------------------------------------------------ */
/*  Virtual event synthesis (CoreEnterNotifies / CoreLeaveNotifies)    */
/* ------------------------------------------------------------------ */

/* Emit EnterNotify on each window on the path from `ancestor`'s child
 * down to (but not including) `descendant`, with `virtual_detail`. */
static void CoreEnterNotifies(Display* dpy,
                              Window ancestor,
                              Window descendant,
                              int mode,
                              int virtual_detail,
                              int rx,
                              int ry,
                              unsigned int state) {
  if (ancestor == None || descendant == None)
    return;
  if (ancestor == descendant)
    return;

  /* Walk down from ancestor to descendant.  We reconstruct the path by
   * walking up from descendant and collecting windows, then emitting
   * in reverse. */
  Window path[MAX_SPRITE_TRACE];
  int count = 0;
  Window cur = descendant;
  while (cur != None && cur != ancestor) {
    path[count++] = cur;
    if (count >= MAX_SPRITE_TRACE)
      break;
    EmxWindow* ew = em_x11_window_find(dpy, cur);
    Window parent = None;
    if (ew && ew->parent != None && ew->parent != cur)
      parent = ew->parent;
    else
      parent = em_x11_js_get_parent(cur);
    if (parent == None || parent == cur)
      break;
    cur = parent;
  }

  /* Emit in top-down order (ancestor's child first).
   * The window that gets the EnterNotify is the one whose CHILD the
   * pointer just entered.  For virtual crossings, child is set. */
  for (int i = count - 1; i >= 1; i--) {
    Window w = path[i];     /* the window receiving the event */
    Window c = path[i - 1]; /* the child that contains the pointer */
    CoreEnterLeaveEvent(
      dpy, EnterNotify, mode, virtual_detail, w, c, rx, ry, state);
  }
}

/* Emit LeaveNotify on each window on the path from `descendant` up to
 * (but not including) `ancestor`, with `virtual_detail`. */
static void CoreLeaveNotifies(Display* dpy,
                              Window descendant,
                              Window ancestor,
                              int mode,
                              int virtual_detail,
                              int rx,
                              int ry,
                              unsigned int state) {
  if (descendant == None || ancestor == None)
    return;
  if (descendant == ancestor)
    return;

  Window cur = descendant;
  EmxWindow* ew = em_x11_window_find(dpy, cur);
  while (cur != None && cur != ancestor) {
    Window parent;
    if (ew && ew->parent != None && ew->parent != cur)
      parent = ew->parent;
    else
      parent = em_x11_js_get_parent(cur);
    if (parent == None || parent == cur)
      break;

    /* Emit LeaveNotify on `cur` — the window the pointer is leaving.
     * child = None for the immediate-leave case; virtual detail as given. */
    CoreEnterLeaveEvent(
      dpy, LeaveNotify, mode, virtual_detail, cur, None, rx, ry, state);
    cur = parent;
    ew = em_x11_window_find(dpy, cur);
  }
}

/* ------------------------------------------------------------------ */
/*  The four canonical crossing shapes (xorg enterleave.c)             */
/* ------------------------------------------------------------------ */

/* Pointer moved from ancestor A down to descendant B.
 * LeaveNotify(Inferior) on A, virtual EnterNotifies on intermediates,
 * EnterNotify(Ancestor) on B. */
static void CoreEnterLeaveToDescendant(Display* dpy,
                                       Window from,
                                       Window to,
                                       int mode,
                                       int rx,
                                       int ry,
                                       unsigned int state) {
  CoreEnterLeaveEvent(
    dpy, LeaveNotify, mode, NotifyInferior, from, None, rx, ry, state);
  CoreEnterNotifies(dpy, from, to, mode, NotifyVirtual, rx, ry, state);
  CoreEnterLeaveEvent(
    dpy, EnterNotify, mode, NotifyAncestor, to, None, rx, ry, state);
}

/* Pointer moved from descendant A up to ancestor B.
 * LeaveNotify(Ancestor) on A, virtual LeaveNotifies on intermediates,
 * EnterNotify(Inferior) on B. */
static void CoreEnterLeaveToAncestor(Display* dpy,
                                     Window from,
                                     Window to,
                                     int mode,
                                     int rx,
                                     int ry,
                                     unsigned int state) {
  CoreEnterLeaveEvent(
    dpy, LeaveNotify, mode, NotifyAncestor, from, None, rx, ry, state);
  CoreLeaveNotifies(dpy, from, to, mode, NotifyVirtual, rx, ry, state);
  CoreEnterLeaveEvent(
    dpy, EnterNotify, mode, NotifyInferior, to, None, rx, ry, state);
}

/* Pointer moved between unrelated windows (siblings, or different branches).
 * LeaveNotify(Nonlinear) on A, then virtual LeaveNotifies A→LCA,
 * then virtual EnterNotifies LCA→B, then EnterNotify(Nonlinear) on B. */
static void CoreEnterLeaveNonLinear(Display* dpy,
                                    Window from,
                                    Window to,
                                    int mode,
                                    int rx,
                                    int ry,
                                    unsigned int state) {
  Window lca = common_ancestor(dpy, from, to);

  CoreEnterLeaveEvent(
    dpy, LeaveNotify, mode, NotifyNonlinear, from, None, rx, ry, state);
  if (lca != None) {
    CoreLeaveNotifies(
      dpy, from, lca, mode, NotifyNonlinearVirtual, rx, ry, state);
    CoreEnterNotifies(
      dpy, lca, to, mode, NotifyNonlinearVirtual, rx, ry, state);
  }
  CoreEnterLeaveEvent(
    dpy, EnterNotify, mode, NotifyNonlinear, to, None, rx, ry, state);
}

/* Top-level dispatcher: determine the relationship between `from` and
 * `to`, then emit the correct crossing-event sequence.
 * Mirrors xorg's CoreEnterLeaveEvents (enterleave.c:537). */
static void CoreEnterLeaveEvents(Display* dpy,
                                 Window from,
                                 Window to,
                                 int mode,
                                 int rx,
                                 int ry,
                                 unsigned int state) {
  if (from == to)
    return;

  if (win_is_inferior_of(dpy, to, from))
    CoreEnterLeaveToDescendant(dpy, from, to, mode, rx, ry, state);
  else if (win_is_inferior_of(dpy, from, to))
    CoreEnterLeaveToAncestor(dpy, from, to, mode, rx, ry, state);
  else
    CoreEnterLeaveNonLinear(dpy, from, to, mode, rx, ry, state);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

/* The master crossing function.  Computes the crossing-event sequence
 * from `from` to `to` and pushes the events to the queue.
 * Mirrors xorg's DoEnterLeaveEvents (enterleave.c:596). */
void em_x11_do_enter_leave_events(
  Display* dpy, Window from, Window to, int mode, int rx, int ry) {
  if (from == to)
    return;

  /* xorg: when a grab has ownerEvents=True and the pointer is inside the
   * grab window's hierarchy, crossing events use NotifyNormal.  Events
   * outside the hierarchy use NotifyGrab.  But em-x11 doesn't support
   * GrabModeSync (events are never frozen), so the NotifyGrab/NotifyNormal
   * distinction has no delivery-side effect: events always reach the
   * window under the pointer.  However, Motif's RowColumn translation
   * <EnterWindow>Normal: MenuEnter() exclusively matches NotifyNormal
   * events.  During spring-loaded menus this is correct — NotifyGrab
   * suppresses MenuEnter and PushB's Enter() handles arming via
   * _XmGetInDragMode().  During posted menus _XmGetInDragMode is False,
   * so PushB does nothing and highlight depends on MenuEnter() matching.
   *
   * Real xorg makes this work because:
   *   - The grab from _XmMenuGrabKeyboardAndPointer is GrabModeSync
   *   - XAllowEvents replays events, during which crossing modes are
   *     effectively NotifyNormal
   *   - Or XtUngrabPointer fires before the hover phase starts
   *
   * Since em-x11 flattens all grabs to async, the safest approximation
   * is: ownerEvents=True → all crossing events use NotifyNormal.
   * This matches the effective xorg behavior where owner_events means
   * "let events reach the window under the pointer normally." */
  if (mode == NotifyGrab && active_grab_active() &&
      active_grab_owner_events_get()) {
    mode = NotifyNormal;
  }

  /* State is stored by events.c; pulled here for the event payload.
   * In a real X server this comes from the device's button->state and
   * the keyboard's xkbInfo.  We track a simplified modifier field. */
  unsigned int state = key_modifier_state();
  CoreEnterLeaveEvents(dpy, from, to, mode, rx, ry, state);
}

/* Update the sprite: re-fill spriteTrace, and if spriteWin changed,
 * emit crossing events.  Called on every button press and motion. */
void em_x11_update_sprite(
  Display* dpy, Window deepest_hint, int rx, int ry, int mode) {
  Window old = spriteWin;
  em_x11_fill_sprite_trace(dpy, deepest_hint, rx, ry);
  if (spriteWin != old)
    em_x11_do_enter_leave_events(dpy, old, spriteWin, mode, rx, ry);
}
