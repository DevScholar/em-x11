/*
 * emx11_internal.h -- em-x11's private internal header.
 *
 * Defines the memory layout of the opaque types (struct _XDisplay,
 * struct _XGC) that upstream <X11/Xlib.h> only forward-declares. Clients
 * never see this file; only native/src C files include it.
 *
 * Upstream Xlib exposes "convenience" macros such as DefaultScreen(dpy),
 * BlackPixel(dpy, scr), and ScreenOfDisplay(dpy, scr) that cast the
 * Display* to a pointer-to-anonymous-struct type called _XPrivDisplay
 * (defined inline in Xlib.h). For those macros to work, the memory
 * prefix of struct _XDisplay must match that anonymous struct exactly.
 * We therefore replicate the public prefix here, then append em-x11
 * private fields after.
 */

#ifndef EMX11_INTERNAL_H
#define EMX11_INTERNAL_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include <stddef.h>

#define EMX11_MAX_WINDOWS          256
#define EMX11_EVENT_QUEUE_CAPACITY 256

/* ------------------------------------------------------------------------- */
/*  GC -- opaque to clients; upstream only exposes "ext_data" (through the  */
/*  XLIB_ILLEGAL_ACCESS define which we never set). Everything else is ours. */
/* ------------------------------------------------------------------------- */

struct _XGC {
    XExtData      *ext_data;
    GContext       gid;             /* nominal protocol id, unused by us */
    unsigned long  foreground;
    unsigned long  background;
    int            line_width;
    int            line_style;
    int            fill_style;
};

/* ------------------------------------------------------------------------- */
/*  EmxWindow -- em-x11's per-window bookkeeping. Not visible to clients;   */
/*  they only ever see opaque Window (XID).                                  */
/* ------------------------------------------------------------------------- */

typedef struct EmxWindow {
    Window         id;
    Window         parent;
    int            x, y;
    unsigned int   width, height;
    unsigned int   border_width;
    unsigned long  border_pixel;
    unsigned long  background_pixel;
    long           event_mask;
    bool           mapped;
    bool           in_use;
    char           name[64];

    /* SHAPE extension state (bounding region only for v1).
     * shape_bounding is NULL when the window has no custom shape, in which
     * case the window is a plain rectangle of its full size. When set, it
     * points to an array of `shape_bounding_count` XRectangle records,
     * each in window-local coordinates. */
    XRectangle    *shape_bounding;
    int            shape_bounding_count;
} EmxWindow;

/* ------------------------------------------------------------------------- */
/*  struct _XDisplay                                                         */
/*                                                                           */
/*  The fields up to `xdefaults` mirror the anonymous struct in upstream    */
/*  Xlib.h (libX11-1.8.13, lines ~489-541). Order and types must remain in  */
/*  sync with whichever libX11 release we ship headers from -- changing a   */
/*  field here requires re-verifying Xlib.h.                                 */
/* ------------------------------------------------------------------------- */

struct _XPrivate;        /* forward decl -- we never define/use */
struct _XrmHashBucketRec;/* forward decl -- resource manager not wired yet */

struct _XDisplay {
    /* ---- public prefix (must match upstream _XPrivDisplay layout) ------ */
    XExtData                  *ext_data;
    struct _XPrivate          *private1;
    int                        fd;
    int                        private2;
    int                        proto_major_version;
    int                        proto_minor_version;
    char                      *vendor;
    XID                        private3;
    XID                        private4;
    XID                        private5;
    int                        private6;
    XID (*resource_alloc)(struct _XDisplay *);
    int                        byte_order;
    int                        bitmap_unit;
    int                        bitmap_pad;
    int                        bitmap_bit_order;
    int                        nformats;
    ScreenFormat              *pixmap_format;
    int                        private8;
    int                        release;
    struct _XPrivate          *private9;
    struct _XPrivate          *private10;
    int                        qlen;
    unsigned long              last_request_read;
    unsigned long              request;
    XPointer                   private11;
    XPointer                   private12;
    XPointer                   private13;
    XPointer                   private14;
    unsigned                   max_request_size;
    struct _XrmHashBucketRec  *db;
    int (*private15)(struct _XDisplay *);
    char                      *display_name;
    int                        default_screen;
    int                        nscreens;
    Screen                    *screens;
    unsigned long              motion_buffer;
    unsigned long              private16;
    int                        min_keycode;
    int                        max_keycode;
    XPointer                   private17;
    XPointer                   private18;
    int                        private19;
    char                      *xdefaults;

    /* ---- em-x11 private tail (invisible to clients) -------------------- */
    Screen                     screen0;         /* backing for screens[0]   */
    Visual                     visual0;         /* root_visual of screen0   */
    Depth                      depth0;          /* single 24-bit depth      */
    ScreenFormat               format0;         /* 32-bit pixel format      */

    XID                        next_xid;

    EmxWindow                  windows[EMX11_MAX_WINDOWS];

    XEvent                     event_queue[EMX11_EVENT_QUEUE_CAPACITY];
    unsigned int               event_head;      /* next slot to read        */
    unsigned int               event_tail;      /* next slot to write       */
};

/* ------------------------------------------------------------------------- */
/*  Internal helpers shared across native/src C files.                       */
/* ------------------------------------------------------------------------- */

Display   *emx11_get_display(void);            /* singleton accessor        */
EmxWindow *emx11_window_find(Display *dpy, Window id);
EmxWindow *emx11_window_alloc(Display *dpy);
XID        emx11_next_xid(Display *dpy);

bool         emx11_event_queue_push(Display *dpy, const XEvent *event);
bool         emx11_event_queue_pop (Display *dpy, XEvent *out);
unsigned int emx11_event_queue_size(const Display *dpy);

/* ------------------------------------------------------------------------- */
/*  JS bridge. These symbols are defined by src/bindings/emx11.library.js   */
/*  and hooked into the link via --js-library. The C side calls into the    */
/*  browser (canvas draw, DOM mutation) through these.                       */
/* ------------------------------------------------------------------------- */

extern void emx11_js_init(int screen_width, int screen_height);
extern void emx11_js_window_create(Window id, int x, int y,
                                   unsigned int w, unsigned int h,
                                   unsigned long background);
extern void emx11_js_window_map(Window id);
extern void emx11_js_window_unmap(Window id);
extern void emx11_js_window_destroy(Window id);
extern void emx11_js_fill_rect(Window id, int x, int y,
                               unsigned int w, unsigned int h,
                               unsigned long color);
extern void emx11_js_draw_line(Window id, int x1, int y1, int x2, int y2,
                               unsigned long color, int line_width);
extern void emx11_js_flush(void);

/* SHAPE extension: push the new bounding rectangles to the compositor.
 * rects is an array of (x, y, width, height) int quadruples, length
 * 4 * count. Passing count==0 clears the shape (window returns to a
 * plain rectangle). */
extern void emx11_js_window_shape(Window id, const int *rects, int count);

#endif /* EMX11_INTERNAL_H */
