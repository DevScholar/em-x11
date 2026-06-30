/*
 * StBytes.c — cut-buffer operations (obsolete X10 API, kept for compatibility).
 * Upstream: libX11/src/StBytes.c
 */

#include <X11/Xlib.h>
#include <stdlib.h>

char* XFetchBuffer(Display* dpy, int* nbytes_return, int buffer) {
  (void)dpy;
  (void)buffer;
  if (nbytes_return)
    *nbytes_return = 0;
  return NULL;
}

int XRotateBuffers(Display* dpy, int rotate) {
  (void)dpy;
  (void)rotate;
  return 1;
}

int XStoreBuffer(Display* dpy, const char* bytes, int nbytes, int buffer) {
  (void)dpy;
  (void)bytes;
  (void)nbytes;
  (void)buffer;
  return 1;
}
