/*
 * hello -- em-x11 pipeline smoke test.
 *
 * Exercises the Xlib APIs implemented so far:
 *   - XOpenDisplay / XCreateSimpleWindow / XMapWindow
 *   - XFillRectangle, XFillArc (circle), XDrawLine (crosshair)
 *   - ButtonPress / KeyPress / MotionNotify / Expose routing
 *
 * The red square follows mouse clicks. Keyboard keys print to the browser
 * console so you can verify the keysym plumbing.
 */

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <emscripten.h>
#include <stdio.h>
#include <string.h>

static Display     *display;
static Window       window;
static GC           gc;
static XFontStruct *font_fixed;   /* monospace, from alias "fixed"       */
static XFontStruct *font_sans;    /* sans-serif, from an XLFD helvetica  */

static int block_x = 150;
static int block_y = 100;

/* Last click position, printed as text in the window. */
static char status_text[64] = "click anywhere, press keys (Esc quits)";

static void redraw(void) {
    XSetForeground(display, gc, 0x003020UL);
    XFillRectangle(display, window, gc, 0, 0, 400, 300);

    /* A yellow filled circle in the corner using the new XFillArc path.
     * Angles are in 64ths of a degree; 360*64 = 23040 means full ellipse. */
    XSetForeground(display, gc, 0xE0C040UL);
    XFillArc(display, window, gc, 20, 20, 60, 60, 0, 360 * 64);

    /* Red square -- position follows the last click. */
    XSetForeground(display, gc, 0xE04040UL);
    XFillRectangle(display, window, gc, block_x, block_y, 100, 100);

    /* Thin white crosshair through the centre of the red square, via
     * XDrawLine -- proves XDrawLine still works alongside the new
     * primitives. */
    XSetForeground(display, gc, 0xFFFFFFUL);
    XDrawLine(display, window, gc,
              block_x + 50, block_y,
              block_x + 50, block_y + 100);
    XDrawLine(display, window, gc,
              block_x,       block_y + 50,
              block_x + 100, block_y + 50);

    /* Status text at the bottom. Two lines showing (1) the monospace
     * "fixed" font and (2) a proportional sans-serif resolved from an
     * XLFD helvetica spec -- proves family mapping and browser-measured
     * advances both work. */
    XSetForeground(display, gc, 0xFFFFFFUL);
    XSetFont(display, gc, font_fixed->fid);
    XDrawString(display, window, gc, 10, 260,
                status_text, (int)strlen(status_text));

    const char *tag = "em-x11 / XDrawString / measureText metrics";
    XSetFont(display, gc, font_sans->fid);
    XDrawString(display, window, gc, 10, 285, tag, (int)strlen(tag));

    XFlush(display);
}

static void tick(void) {
    while (XPending(display) > 0) {
        XEvent event;
        XNextEvent(display, &event);
        switch (event.type) {
        case Expose:
            if (event.xexpose.count == 0) redraw();
            break;

        case ButtonPress:
            printf("hello: button %u at (%d,%d)\n",
                   event.xbutton.button, event.xbutton.x, event.xbutton.y);
            block_x = event.xbutton.x - 50;
            block_y = event.xbutton.y - 50;
            snprintf(status_text, sizeof(status_text),
                     "last click: (%d, %d)", event.xbutton.x, event.xbutton.y);
            redraw();
            break;

        case MotionNotify:
            /* Too spammy to log on every move; the event is still being
             * delivered, which the non-zero XPending count proves. */
            break;

        case KeyPress: {
            KeySym ks = XLookupKeysym(&event.xkey, 0);
            printf("hello: KeyPress keysym=0x%lx state=0x%x\n",
                   (unsigned long)ks, event.xkey.state);
            if (ks == XK_Escape) {
                printf("hello: got Escape, stopping main loop\n");
                emscripten_cancel_main_loop();
                return;
            }
            break;
        }

        case KeyRelease: {
            KeySym ks = XLookupKeysym(&event.xkey, 0);
            printf("hello: KeyRelease keysym=0x%lx\n", (unsigned long)ks);
            break;
        }

        default:
            break;
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
                                 0, BlackPixel(display, 0), 0x003020UL);
    XStoreName(display, window, "em-x11 hello");
    XSelectInput(display, window,
                 ExposureMask | ButtonPressMask | KeyPressMask |
                 KeyReleaseMask | PointerMotionMask);
    XMapWindow(display, window);

    gc = XCreateGC(display, window, 0, NULL);

    /* Load two fonts to demonstrate family mapping. "fixed" resolves to
     * "13px monospace" with per-char widths measured by the browser.
     * The XLFD below resolves to "bold 14px sans-serif" -- parsed from
     * field 2 (helvetica -> sans-serif), field 3 (bold -> bold), and
     * field 7 (14 -> 14px). */
    font_fixed = XLoadQueryFont(display, "fixed");
    font_sans  = XLoadQueryFont(
        display, "-*-helvetica-bold-r-normal-*-14-*-*-*-*-*-*-*");
    if (font_fixed) XSetFont(display, gc, font_fixed->fid);

    emscripten_set_main_loop(tick, 0, 1);
    return 0;
}
