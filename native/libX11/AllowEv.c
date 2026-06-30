/*
 * XAllowEvents — release queued events after a grab.
 * Upstream: libX11/src/AllowEv.c
 *
 * Real X11 sends an AllowEvents protocol request. em-x11 has no separate
 * X server process, so this is always a no-op.
 */

#include <X11/Xlib.h>

int XAllowEvents(Display* dpy, int event_mode, Time time) {
  (void)dpy;
  (void)event_mode;
  (void)time;
  return 1;
}
