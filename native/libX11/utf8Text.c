/*
 * Xutf8TextListToTextProperty — UTF-8 text-list-to-property conversion.
 * Upstream: libX11/src/xlibi18n/lcWrap.c
 *
 * Real libX11 dispatches through the locale-conversion layer. em-x11
 * always uses UTF-8, so the "conversion" is just strdup.
 */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

int Xutf8TextListToTextProperty(Display* dpy,
                                char** list,
                                int count,
                                XICCEncodingStyle style,
                                XTextProperty* text_prop) {
  (void)dpy;
  (void)count;
  (void)style;
  if (list && list[0] && text_prop) {
    text_prop->value = (unsigned char*)strdup(list[0]);
    text_prop->nitems = strlen(list[0]);
    text_prop->encoding = XA_STRING;
    text_prop->format = 8;
    return 0;
  }
  return -1;
}
