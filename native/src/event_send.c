/*
 * Synthetic event delivery and input focus.
 *
 * XSendEvent: Tk's WM_DELETE_WINDOW, `event generate` virtual events,
 * focus-in/out synthetics. Most pass NoEventMask -- the simple path.
 * XSetInputFocus / XGetInputFocus: focus tracking, paired with the
 * FocusOut/FocusIn synthetics on transition. See event.c for input
 * dispatch (button/motion/key) and event_queue.c for the queue itself.
 */

#include "emx11_internal.h"

#include <string.h>

/* Deliver a synthesized event. In real X this round-trips to the server,
 * which re-dispatches per the event_mask / propagate rules; we deliver
 * directly into our own queue because em-x11 is a single-process "server".
 *
 * Tk uses XSendEvent heavily for self-addressed traffic: WM_DELETE_WINDOW
 * via ClientMessage, `event generate` virtual events, focus-in/out
 * synthetics. Most of those pass event_mask=NoEventMask (= 0), which
 * means "deliver regardless of selection" -- the simple path.
 *
 * Propagation semantics (x11protocol.txt §SendEvent): if propagate=True
 * and no client on `w` selects any bit of event_mask, walk up the parent
 * chain until one does, or until a window has that bit in its
 * do_not_propagate_mask. We do NOT track do_not_propagate_mask -- it has
 * no demo trip-wire today; revisit when a Tk widget actually sets it via
 * XChangeWindowAttributes(CWDontPropagate).
 *
 * PointerWindow / InputFocus as `w`: real Xlib resolves these against
 * current pointer / focus state. InputFocus resolves to
 * display->focus_window (wired below); PointerWindow is NOT resolved
 * (would need a hit test via JS round-trip). Tk's explicit-send paths
 * use real XIDs, not these sentinels, so the gap is unobserved. */
Status XSendEvent(Display *display, Window w, Bool propagate,
                  long event_mask, XEvent *event_send) {
    if (!display || !event_send) return 0;

    /* Resolve InputFocus sentinel to the concrete focus XID before any
     * further bookkeeping -- the rest of this function treats `w` as a
     * real window id. PointerWindow (= 0) coincides numerically with
     * None so we leave it alone; the caller gets delivery on xany.window
     * == 0, which Tk self-filters. */
    if (w == InputFocus) {
        w = display->focus_window;
    }

    /* Clipboard proxy intercept: when a real CLIPBOARD owner responds to
     * our back-channel XConvertSelection by sending SelectionNotify to
     * the proxy, forward the UTF-8 bytes to navigator.clipboard.writeText()
     * and consume the event (nothing on the X side should ever read from
     * the proxy's queue). Returns True only if the event was handled. */
    if (emx11_selection_intercept_send(display, w, event_send)) {
        return 1;
    }

    XEvent ev = *event_send;                    /* caller may reuse buffer */
    ev.xany.display    = display;
    ev.xany.send_event = True;
    ev.xany.window     = w;

    /* NoEventMask or unmaskable event type: deliver to `w` unconditionally. */
    if (event_mask == 0) {
        return emx11_event_queue_push(display, &ev) ? 1 : 0;
    }

    /* Find the closest window up from `w` that selects any bit of
     * event_mask. If propagate=False, only `w` itself is a candidate. */
    EmxWindow *target = (w == PointerWindow || w == None)
                            ? NULL
                            : emx11_window_find(display, w);
    Window root = display->screens[0].root;
    while (target) {
        if (target->event_mask & event_mask) {
            ev.xany.window = target->id;
            return emx11_event_queue_push(display, &ev) ? 1 : 0;
        }
        if (!propagate) break;
        if (target->parent == None || target->parent == target->id) break;
        if (target->id == root) break;
        target = emx11_window_find(display, target->parent);
    }

    /* Nothing along the chain selected for this event. Matches real X's
     * "discard" outcome, but we still queue it on `w` so Tk's direct
     * self-sends (XSendEvent to its own shell with a mask the shell
     * happens not to select) don't silently disappear. Revisit once
     * event_mask selection is well-exercised by real Tk traffic. */
    ev.xany.window = w;
    return emx11_event_queue_push(display, &ev) ? 1 : 0;
}

/* Push a FocusIn or FocusOut event onto the queue. `mode` / `detail` are
 * the X protocol classifications; NotifyNormal + NotifyNonlinear is the
 * standard pair for an explicit XSetInputFocus between unrelated
 * windows. PointerRoot and None don't get synthetic events -- there's
 * no target window to address. */
static void push_focus_change(Display *dpy, int type, Window w,
                              int mode, int detail) {
    if (w == None || w == PointerRoot) return;
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xfocus.type    = type;
    ev.xfocus.display = dpy;
    ev.xfocus.window  = w;
    ev.xfocus.mode    = mode;
    ev.xfocus.detail  = detail;
    emx11_event_queue_push(dpy, &ev);
}

/* x11protocol.txt §SetInputFocus: update focus iff `time` is no earlier
 * than the last-focus-change-time. CurrentTime (0) in the request is
 * accepted unconditionally and treated as "now". We synthesize a
 * FocusOut on the outgoing window and a FocusIn on the incoming one so
 * Tk's focus tracker (tk/generic/tkFocus.c) sees the transition. */
int XSetInputFocus(Display *display, Window focus, int revert_to, Time time) {
    if (!display) return 0;
    if (time != CurrentTime && time < display->focus_last_time) {
        return 1;                               /* too old -- silent no-op  */
    }

    Window old_focus = display->focus_window;
    display->focus_window    = focus;
    display->focus_revert_to = revert_to;
    display->focus_last_time = (time == CurrentTime) ? display->focus_last_time
                                                      : time;

    if (old_focus != focus) {
        push_focus_change(display, FocusOut, old_focus,
                          NotifyNormal, NotifyNonlinear);
        push_focus_change(display, FocusIn,  focus,
                          NotifyNormal, NotifyNonlinear);
    }
    return 1;
}

int XGetInputFocus(Display *display, Window *focus_return,
                   int *revert_to_return) {
    if (!display) return 0;
    if (focus_return)     *focus_return     = display->focus_window;
    if (revert_to_return) *revert_to_return = display->focus_revert_to;
    return 1;
}
