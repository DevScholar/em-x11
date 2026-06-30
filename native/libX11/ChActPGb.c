/*
 * XChangeActivePointerGrab — change the active pointer grab parameters.
 * Upstream: libX11/src/ChActPGb.c
 *
 * Real X11 sends a ChangeActivePointerGrab protocol request. em-x11
 * handles grabs through the host-side input router.
 */

#include <X11/Xlib.h>

int XChangeActivePointerGrab(Display* dpy,
                             unsigned int event_mask,
                             Cursor cursor,
                             Time time) {
  (void)dpy;
  (void)event_mask;
  (void)cursor;
  (void)time;
  return 1;
}
