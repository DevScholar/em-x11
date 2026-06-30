/*
 * XSetCloseDownMode — set the display close-down mode.
 * Upstream: libX11/src/ChClMode.c
 *
 * Real X11 sends a SetCloseDownMode protocol request. em-x11 has no X
 * server lifetime management, so this is a no-op.
 */

#include <X11/Xlib.h>

int XSetCloseDownMode(Display* dpy, int close_mode) {
  (void)dpy;
  return close_mode;
}
