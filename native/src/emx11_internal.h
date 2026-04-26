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
    Font           font;            /* currently bound font, or None */
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
    /* Window background_pixmap. None (=0) means "use background_pixel as a
     * solid fill"; any other id is a Pixmap whose content is tiled across
     * the window (X semantics: tile origin = window's top-left). The
     * classic X root weave lives here on the root window. */
    Pixmap         background_pixmap;
    long           event_mask;
    bool           mapped;
    bool           in_use;
    bool           override_redirect;
    char           name[64];

    /* SHAPE extension state (bounding region only for v1).
     * shape_bounding is NULL when the window has no custom shape, in which
     * case the window is a plain rectangle of its full size. When set, it
     * points to an array of `shape_bounding_count` XRectangle records,
     * each in window-local coordinates. */
    XRectangle    *shape_bounding;
    int            shape_bounding_count;

    /* Linked list of XChangeProperty payloads. See property.c. */
    struct EmxProperty *properties;
} EmxWindow;

typedef struct EmxProperty {
    struct EmxProperty *next;
    Atom                name;
    Atom                type;
    int                 format;         /* 8, 16, or 32 */
    int                 nitems;
    unsigned char      *data;           /* raw bytes, length = nitems * fmt/8 */
} EmxProperty;

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

    /* Connection bookkeeping granted by the Host (the JS "X server")
     * when XOpenDisplay runs. `conn_id` identifies this wasm module in
     * the Host's connection table so events / redirect dispatch know
     * which queue to push into. `xid_base` and `xid_mask` carve out
     * this client's resource-id range per x11protocol.txt §869/§935:
     * every XID we hand out is `xid_base | (counter & xid_mask)`, and
     * the top three bits are always zero. Other connections use
     * non-overlapping bases so XIDs are globally unique without a
     * round-trip to the Host for each alloc. */
    int                        conn_id;
    XID                        xid_base;
    XID                        xid_mask;

    EmxWindow                  windows[EMX11_MAX_WINDOWS];

    XEvent                     event_queue[EMX11_EVENT_QUEUE_CAPACITY];
    unsigned int               event_head;      /* next slot to read        */
    unsigned int               event_tail;      /* next slot to write       */

    /* Keymap. em-x11 has no real hardware keyboard: we synthesize keycodes
     * on demand as new keysyms appear in browser events. keysym_table[kc]
     * holds the keysym assigned to keycode `kc`. X reserves keycodes 0..7,
     * so we start allocating at 8. */
    KeySym                     keysym_table[256];
    unsigned int               next_keycode;    /* next free keycode >= 8   */

    /* Input focus (XSetInputFocus / XGetInputFocus).
     *   focus_window    = current focus; None (0) or PointerRoot (1) are
     *                     the X sentinel values, otherwise a real XID.
     *   focus_revert_to = one of RevertToNone / RevertToPointerRoot /
     *                     RevertToParent; stored verbatim and echoed back
     *                     by XGetInputFocus. We don't yet act on revert
     *                     when the focus window becomes unviewable; Tk's
     *                     first-window path doesn't exercise that.
     *   focus_last_time = CurrentTime-ordering per x11protocol.txt
     *                     §SetInputFocus: a request with time < this is
     *                     ignored. CurrentTime (0) in the request always
     *                     wins (treated as "now"). */
    Window                     focus_window;
    int                        focus_revert_to;
    Time                       focus_last_time;
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

/* Remove the first event from the queue whose type's event-mask bit is
 * set in `mask`. Copies the event into *out and compacts the queue.
 * Returns true on hit, false if nothing matched. */
bool emx11_event_queue_peek_match(Display *dpy, long mask, XEvent *out);

/* Remove the first event whose type == `type` and xany.window == `w`. */
bool emx11_event_queue_peek_typed(Display *dpy, Window w, int type, XEvent *out);

/* Look up or allocate a keycode for a given keysym. Returns 0 if the
 * keycode table is exhausted (very unlikely in practice). */
KeyCode emx11_keysym_to_keycode(Display *dpy, KeySym keysym);

/* Look up the CSS font string bound to a loaded Font id. Returns NULL
 * if the font hasn't been loaded (caller falls back to a default). */
const char *emx11_font_css(Font font);

/* Release the linked list of XChangeProperty payloads on destroy. */
void emx11_window_free_properties(EmxWindow *win);

/* ------------------------------------------------------------------------- */
/*  JS bridge. These symbols are defined by src/bindings/emx11.library.js   */
/*  and hooked into the link via --js-library. The C side calls into the    */
/*  browser (canvas draw, DOM mutation) through these.                       */
/* ------------------------------------------------------------------------- */

extern void emx11_js_init(int screen_width, int screen_height);

/* Connection setup. Called by XOpenDisplay before anything else touches
 * the Host. The Host allocates a connection id and grants this client
 * an XID range (see struct _XDisplay for layout). `emx11_js_close_display`
 * tells the Host to drop the connection; any resources still owned by
 * it are cleaned up server-side. */
extern void emx11_js_open_display(int *conn_id_out,
                                  unsigned int *xid_base_out,
                                  unsigned int *xid_mask_out);
extern void emx11_js_close_display(int conn_id);
/* Shared root window. Host owns the single root; every client's
 * XOpenDisplay asks for its XID instead of minting a local root. */
extern Window emx11_js_get_root_window(void);

extern void emx11_js_window_create(int conn_id, Window id, Window parent,
                                   int x, int y,
                                   unsigned int w, unsigned int h,
                                   unsigned long background);
/* Geometry change on an existing window (XMoveWindow / XResizeWindow /
 * XConfigureWindow). Distinct from window_create so Host doesn't have
 * to re-seed parent, shape, or background_pixmap -- we only touch the
 * geometry fields. */
extern void emx11_js_window_configure(Window id, int x, int y,
                                      unsigned int w, unsigned int h);
extern void emx11_js_window_map(int conn_id, Window id);
extern void emx11_js_window_unmap(int conn_id, Window id);
extern void emx11_js_window_destroy(Window id);
/* Per-window event-mask subscription. XSelectInput mirrors its new
 * value to the Host so SubstructureRedirect / SubstructureNotify holders
 * can be located without scanning every client's C-side window table. */
extern void emx11_js_select_input(int conn_id, Window id, long mask);
/* Toggle a window's override_redirect flag on the Host side. Used by
 * XChangeWindowAttributes(CWOverrideRedirect). OR=True means "window
 * managers must not interfere" (popup menus, tooltips, twm's own
 * decoration frames) -- the Host skips redirect processing for them. */
extern void emx11_js_set_override_redirect(Window id, int flag);
/* XReparentWindow -- move a window under a new parent. (x, y) is the
 * new origin in the new parent's coordinate space. Always forwarded to
 * the Host even when the caller has no local shadow of `id`, because
 * cross-connection reparents are legal (twm reparenting xeyes's shell
 * is the canonical example). */
extern void emx11_js_reparent_window(Window id, Window parent, int x, int y);
/* Bind or unbind a Pixmap as the window's tiled background. pm_id=0
 * reverts to the solid background_pixel; any other id must reference a
 * live Pixmap on the JS side. */
extern void emx11_js_window_set_bg_pixmap(Window id, Pixmap pm_id);
/* XClearWindow / XClearArea entry. Lets the compositor decide whether to
 * paint with a solid colour or the window's background_pixmap without
 * the caller having to know. */
extern void emx11_js_clear_area(Window id, int x, int y,
                                unsigned int w, unsigned int h);
extern void emx11_js_fill_rect(Window id, int x, int y,
                               unsigned int w, unsigned int h,
                               unsigned long color);
extern void emx11_js_draw_line(Window id, int x1, int y1, int x2, int y2,
                               unsigned long color, int line_width);
extern void emx11_js_flush(void);

/* Arc drawing: angles are X-semantics (64ths of a degree, counterclockwise
 * from 3 o'clock). The compositor converts to canvas-2d semantics
 * (radians, clockwise, origin at ellipse centre). */
extern void emx11_js_draw_arc(Window id, int x, int y,
                              unsigned int width, unsigned int height,
                              int angle1, int angle2,
                              unsigned long color, int line_width);
extern void emx11_js_fill_arc(Window id, int x, int y,
                              unsigned int width, unsigned int height,
                              int angle1, int angle2,
                              unsigned long color);

/* Polygon: points are a flat int array of 2*count values (x0,y0,x1,y1,...).
 * The shape field is one of CoordModeOrigin (absolute) or CoordModePrevious
 * (relative to prior point). */
extern void emx11_js_fill_polygon(Window id, const int *points, int count,
                                  int shape, int mode, unsigned long color);

/* Points: same flat int array layout as polygon. */
extern void emx11_js_draw_points(Window id, const int *points, int count,
                                 int mode, unsigned long color);

/* Text rendering. font_css is a CSS `font` shorthand string (e.g.
 * "13px monospace"). image_mode=1 fills the text background with
 * bg_color (XDrawImageString semantics); image_mode=0 leaves the
 * background untouched (XDrawString). */
extern void emx11_js_draw_string(Window id, int x, int y,
                                 const char *font_css,
                                 const char *text, int length,
                                 unsigned long fg_color,
                                 unsigned long bg_color,
                                 int image_mode);

/* Query the browser for the real metrics of a CSS font, exported once
 * per XLoadQueryFont so we never have to approximate. Writes:
 *   *ascent    -- ceil(fontBoundingBoxAscent),  pixels
 *   *descent   -- ceil(fontBoundingBoxDescent), pixels
 *   *max_width -- max advance over ASCII 32..126
 *   widths[95] -- per-char advance for ASCII 32..126
 * Silent no-op plus default-filled outputs if no browser canvas is
 * available (e.g. during test harnesses). */
extern void emx11_js_measure_font(const char *font_css,
                                  int *ascent, int *descent,
                                  int *max_width, int *widths);

/* Measure the advance width of `length` bytes of `text` (interpreted as
 * UTF-8) in the given CSS font. Single round-trip to the browser; the
 * result matches what fillText will render pixel-for-pixel. */
extern int emx11_js_measure_string(const char *font_css,
                                   const char *text, int length);

/* SHAPE extension: push the new bounding rectangles to the compositor.
 * rects is an array of (x, y, width, height) int quadruples, length
 * 4 * count. Passing count==0 clears the shape (window returns to a
 * plain rectangle). */
extern void emx11_js_window_shape(Window id, const int *rects, int count);

/* Last-known pointer position in canvas CSS pixels. XQueryPointer polls
 * this every time xeyes fires its 50ms tick. Writes are the JS host's
 * responsibility -- we just read the two ints back. */
extern void emx11_js_pointer_xy(int *x_out, int *y_out);

/* Cross-connection XGetWindowAttributes fallback. When the caller has
 * no local EmxWindow for `id` (the WM case: twm querying xeyes's shell),
 * this returns the Host-authoritative state. `out` is an int[8] buffer:
 *   [0] found (0/1)  [1] x  [2] y
 *   [3] width        [4] height  [5] mapped
 *   [6] override_redirect  [7] border_width (Host tracks 0 for now)
 * When [0]==0, the Host doesn't know the window either; caller should
 * return 0 from XGetWindowAttributes. */
extern void emx11_js_get_window_attrs(Window id, int *out);

/* -- Property bridges (Host-owned storage, dix/property.c layout).
 * Properties are keyed by (XID, atom) server-side so any client can
 * read back what any client wrote. The four entry points mirror the
 * Xlib calls we expose. */
extern int  emx11_js_change_property(Window w, Atom atom, Atom type,
                                     int format, int mode,
                                     const unsigned char *data,
                                     int nelements);
/* XGetWindowProperty: two-call pattern so C owns the output buffer.
 * First call returns meta + required buffer size; second call copies
 * bytes into a caller-provided buffer and optionally deletes the
 * property atomically. meta layout (int[8]):
 *   [0] found (0/1)          [1] actual_type   [2] actual_format
 *   [3] nitems_returned      [4] bytes_after   [5] data_bytes
 *   [6] valid_window (0 => BadWindow, caller returns BadWindow)
 *   [7] reserved */
extern void emx11_js_get_property_meta(Window w, Atom atom, Atom req_type,
                                       long long_offset, long long_length,
                                       int *meta_out);
extern void emx11_js_get_property_data(Window w, Atom atom, Atom req_type,
                                       long long_offset, long long_length,
                                       int delete_flag,
                                       unsigned char *dst, int capacity);
extern void emx11_js_delete_property(Window w, Atom atom);
extern int  emx11_js_list_properties_count(Window w);
extern int  emx11_js_list_properties_fetch(Window w, Atom *dst, int capacity);

/* Pixmap lifecycle. Create installs an OffscreenCanvas on the JS side,
 * keyed by id; destroy drops the reference so its backing bitmap can
 * be reclaimed. depth is 1 for SHAPE masks, 24/32 for color pixmaps. */
extern void emx11_js_pixmap_create(Pixmap id, int width, int height,
                                   int depth);
extern void emx11_js_pixmap_destroy(Pixmap id);

/* SHAPE: decode a 1-bit pixmap into a bounding region and apply it to
 * the given window. op mirrors ShapeSet / ShapeUnion / etc. (op values
 * from X11/extensions/shape.h). */
extern void emx11_js_shape_combine_mask(Window dest, Pixmap src,
                                        int x_off, int y_off, int op);

/* Atom table. Predefined atoms 1..68 are still resolved locally in
 * atom.c for zero round-trip cost; anything else goes through Host
 * so every wasm module in the same page agrees on the id. Fixes the
 * WM_PROTOCOLS / WM_DELETE_WINDOW divergence the per-module tables
 * used to have. emx11_js_get_atom_name returns a malloc'd string
 * that the caller releases via XFree (== free). */
extern Atom  emx11_js_intern_atom(const char *name, Bool only_if_exists);
extern char *emx11_js_get_atom_name(Atom atom);

/* Internal pixmap accessors (implemented in pixmap.c). */
Bool          emx11_pixmap_exists(Pixmap id);
unsigned int  emx11_pixmap_depth(Pixmap id);

/* Parse a CSS / X11 colour name (e.g. "slategrey", "gray85",
 * "rebeccapurple") by delegating to the browser's own colour parser.
 * Writes 16-bit per-channel values on success -- same precision as
 * XColor's red/green/blue fields. Returns 1 on success, 0 if the
 * name isn't recognised or no Host is installed. "rgb:R/G/B" and
 * "#RRGGBB" forms are handled in C; this bridge is only for bare
 * names, which are where the CSS spec's complete table saves us
 * from shipping an rgb.txt of our own. */
extern Status emx11_js_parse_color(const char *name,
                                   unsigned short *red_out,
                                   unsigned short *green_out,
                                   unsigned short *blue_out);

#endif /* EMX11_INTERNAL_H */
