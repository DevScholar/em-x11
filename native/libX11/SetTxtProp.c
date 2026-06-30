/*
 * XSetTextProperty — convenience wrapper around XChangeProperty.
 * Upstream: libX11/src/SetTxtProp.c
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>

void XSetTextProperty(Display* dpy,
                      Window w,
                      XTextProperty* text_prop,
                      Atom property) {
  if (text_prop && text_prop->value && text_prop->nitems > 0) {
    XChangeProperty(dpy,
                    w,
                    property,
                    text_prop->encoding,
                    text_prop->format,
                    PropModeReplace,
                    (unsigned char*)text_prop->value,
                    text_prop->nitems);
  }
}
