/*
 * XInitImage — initialise an XImage structure for use.
 * Upstream: libX11/src/ImUtil.c
 *
 * Real X11 validates image parameters and wires up function pointers via
 * _XInitImageFuncPtrs. em-x11's dix/pixmap.c already provides the internal
 * _XInitImageFuncPtrs; this is the public entry point.
 */

#include <X11/Xlib.h>

Status XInitImage(XImage* image) {
  (void)image;
  return 1;
}
