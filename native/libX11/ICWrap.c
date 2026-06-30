/*
 * XmbResetIC — reset an X input context (multibyte).
 * Upstream: libX11/src/xlibi18n/ICWrap.c
 *
 * Real libX11 dispatches through the input method's mb_reset method.
 * em-x11's XIM layer (xim.c) manages composition state; this stub
 * returns NULL to indicate no pending preedit.
 */

#include <X11/Xlib.h>

char* XmbResetIC(XIC ic) {
  (void)ic;
  return NULL;
}
