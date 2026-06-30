/*
 * Xutf8SetWMProperties — UTF-8 window-manager property setup.
 * Upstream: libX11/src/xlibi18n/utf8WMProps.c
 *
 * Converts UTF-8 window/icon names to XTextProperty via
 * Xutf8TextListToTextProperty, then delegates to XSetWMProperties.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>

void Xutf8SetWMProperties(Display* dpy,
                          Window w,
                          _Xconst char* windowName,
                          _Xconst char* iconName,
                          char** argv,
                          int argc,
                          XSizeHints* normalHints,
                          XWMHints* wmHints,
                          XClassHint* classHints) {
  XTextProperty name_prop, icon_prop;
  XTextProperty *name_ptr = NULL, *icon_ptr = NULL;

  if (windowName) {
    if (Xutf8TextListToTextProperty(
          dpy, (char**)&windowName, 1, XStdICCTextStyle, &name_prop) == 0)
      name_ptr = &name_prop;
  }
  if (iconName) {
    if (Xutf8TextListToTextProperty(
          dpy, (char**)&iconName, 1, XStdICCTextStyle, &icon_prop) == 0)
      icon_ptr = &icon_prop;
  }

  XSetWMProperties(
    dpy, w, name_ptr, icon_ptr, argv, argc, normalHints, wmHints, classHints);

  if (name_ptr && name_prop.value)
    XFree(name_prop.value);
  if (icon_ptr && icon_prop.value)
    XFree(icon_prop.value);
}
