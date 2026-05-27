#ifndef _XRANDR_H_
#define _XRANDR_H_

#include <X11/extensions/randr.h>

/* Stub: em-x11 does not implement RandR extension. These declarations
 * satisfy compilation; the functions are no-ops at runtime. */

typedef struct {
    int type;
    unsigned long serial;
    Bool send_event;
    Display *display;
    Window window;
    Window root;
    Time timestamp;
    Time config_timestamp;
    SizeID size_index;
    Rotation rotation;
    int width;
    int height;
} XRRScreenChangeNotifyEvent;

void XRRSelectInput(Display *dpy, Window window, int mask);
void XRRUpdateConfiguration(XEvent *event);

#endif /* _XRANDR_H_ */
