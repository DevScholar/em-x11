/*
 * hello -- smallest possible em-x11 client.
 *
 * Opens a display, creates one window, paints it, and yields to the
 * browser. Exercises the full C -> WASM -> JS -> Canvas pipeline.
 */

#include <X11/Xlib.h>
#include <emscripten.h>
#include <stdio.h>

static Display *display;
static Window   window;
static GC       gc;

static void redraw(void) {
    /* Two rectangles: a dark green background and a red square in the middle. */
    XSetForeground(display, gc, 0x003020UL);
    XFillRectangle(display, window, gc, 0, 0, 400, 300);
    XSetForeground(display, gc, 0xE04040UL);
    XFillRectangle(display, window, gc, 150, 100, 100, 100);
    XFlush(display);
}

static void tick(void) {
    while (XPending(display) > 0) {
        XEvent event;
        XNextEvent(display, &event);
        if (event.type == Expose && event.xexpose.count == 0) {
            redraw();
        } else if (event.type == ButtonPress) {
            printf("hello: click at (%d,%d)\n", event.xbutton.x, event.xbutton.y);
        }
    }
}

int main(void) {
    display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "hello: XOpenDisplay failed\n");
        return 1;
    }

    Window root = XDefaultRootWindow(display);
    window = XCreateSimpleWindow(display, root,
                                 80, 60, 400, 300,
                                 0, XBlackPixel(display, 0), 0x003020UL);
    XStoreName(display, window, "em-x11 hello");
    XSelectInput(display, window, ExposureMask | ButtonPressMask);
    XMapWindow(display, window);

    gc = XCreateGC(display, window, 0, NULL);

    emscripten_set_main_loop(tick, 0, 1);
    return 0;
}
