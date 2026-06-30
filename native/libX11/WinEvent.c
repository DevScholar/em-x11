/*
 * XWindowEvent — pull the next event matching a window and event mask.
 * Upstream: libX11/src/WinEvent.c
 */

#include <X11/Xlib.h>

/* Map event type to its mask bit. libX11's _XEventTypeToMask / EvToMas.c. */
static unsigned long event_type_to_mask(int type) {
  switch (type) {
    case KeyPress:
      return KeyPressMask;
    case KeyRelease:
      return KeyReleaseMask;
    case ButtonPress:
      return ButtonPressMask;
    case ButtonRelease:
      return ButtonReleaseMask;
    case EnterNotify:
      return EnterWindowMask;
    case LeaveNotify:
      return LeaveWindowMask;
    case Expose:
      return ExposureMask;
    case GraphicsExpose:
      return (1L << 16);
    case NoExpose:
      return (1L << 17);
    case VisibilityNotify:
      return VisibilityChangeMask;
    case CreateNotify:
      return SubstructureNotifyMask;
    case DestroyNotify:
      return SubstructureNotifyMask;
    case UnmapNotify:
      return StructureNotifyMask | SubstructureNotifyMask;
    case MapNotify:
      return StructureNotifyMask | SubstructureNotifyMask;
    case MapRequest:
      return SubstructureRedirectMask;
    case ReparentNotify:
      return StructureNotifyMask | SubstructureNotifyMask;
    case ConfigureNotify:
      return StructureNotifyMask;
    case GravityNotify:
      return StructureNotifyMask;
    case CirculateNotify:
      return StructureNotifyMask;
    case PropertyNotify:
      return PropertyChangeMask;
    case SelectionClear:
      return (1L << 23);
    case SelectionRequest:
      return (1L << 23);
    case SelectionNotify:
      return (1L << 23);
    case ColormapNotify:
      return ColormapChangeMask;
    default:
      if (type >= MotionNotify)
        return (1UL << type);
      return 0;
  }
}

int XWindowEvent(Display* dpy,
                 Window w,
                 long event_mask,
                 XEvent* event_return) {
  int guard = 10000;
  while (guard-- > 0) {
    XEvent ev;
    XNextEvent(dpy, &ev);
    if (ev.xany.window == w && (event_type_to_mask(ev.type) & event_mask)) {
      if (event_return)
        *event_return = ev;
      return 0;
    }
  }
  return 1;
}
