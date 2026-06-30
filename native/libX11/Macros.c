/*
 * Macros.c — function-form equivalents of the convenience macros in Xlib.h.
 * Upstream: libX11/src/Macros.c
 *
 * Real libX11 provides these so non-C language bindings can call the
 * accessors. em-x11 ships identical macros in Xlib.h; these functions
 * are the corresponding extern-callable entry points.
 */

#include <X11/Xlib.h>

Screen* XDefaultScreenOfDisplay(Display* dpy) {
  return DefaultScreenOfDisplay(dpy);
}

int XScreenCount(Display* dpy) { return ScreenCount(dpy); }

char* XDisplayString(Display* dpy) { return DisplayString(dpy); }

unsigned long XBlackPixelOfScreen(Screen* screen) {
  return BlackPixelOfScreen(screen);
}

unsigned long XWhitePixelOfScreen(Screen* screen) {
  return WhitePixelOfScreen(screen);
}

int XWidthOfScreen(Screen* screen) { return WidthOfScreen(screen); }

int XHeightOfScreen(Screen* screen) { return HeightOfScreen(screen); }

unsigned long XLastKnownRequestProcessed(Display* dpy) {
  return LastKnownRequestProcessed(dpy);
}
