/*
 * em_x11_internal.h -- em-x11's private internal header.
 *
 * Defines the memory layout of the opaque types (struct _XDisplay,
 * struct _XGC) that upstream <X11/Xlib.h> only forward-declares. Clients
 * never see this file; only native/ C files include it.
 *
 * Upstream Xlib exposes "convenience" macros such as DefaultScreen(dpy),
 * BlackPixel(dpy, scr), and ScreenOfDisplay(dpy, scr) that cast the
 * Display* to a pointer-to-anonymous-struct type called _XPrivDisplay
 * (defined inline in Xlib.h). For those macros to work, the memory
 * prefix of struct _XDisplay must match that anonymous struct exactly.
 * We therefore replicate the public prefix here, then append em-x11
 * private fields after.
 */

#ifndef EM_X11_INTERNAL_H
#define EM_X11_INTERNAL_H

#include <X11/Xlib.h>
#include <stdbool.h>
#include <stddef.h>

#define EM_X11_MAX_WINDOWS 256
/* Queue sized for a multi-second drag: twm's F_MOVE only drains the events
 * its XMaskEvent mask selects (Button/Motion/Crossing/Expose/Visibility),
 * so every XMoveWindow we generate accumulates a StructureNotify-gated
 * ConfigureNotify on twm's queue that won't be consumed until F_MOVE
 * returns. At 60Hz a 4-second drag lands ~240 entries; add Expose for
 * siblings + headroom and 256 overflows, dropping ButtonRelease and
 * wedging F_MOVE's XMaskEvent. */
#define EM_X11_EVENT_QUEUE_CAPACITY 4096

/* Per-event UTF-8 typed-text slot. One key event carries at most a
 * single grapheme today; sized at 32 bytes leaves room for stacked
 * combining marks and 4-byte codepoints (4 BMP-supplementary chars max).
 * Storage cost: 32 * EM_X11_EVENT_QUEUE_CAPACITY = 128 KiB per Display,
 * which is small next to the event_queue itself. */
#define EM_X11_KEY_TEXT_SLOT 32

/* ------------------------------------------------------------------------- */
/*  GC -- opaque to clients; upstream only exposes "ext_data" (through the  */
/*  XLIB_ILLEGAL_ACCESS define which we never set). Everything else is ours. */
/* ------------------------------------------------------------------------- */

struct _XGC {
  XExtData* ext_data;
  GContext gid; /* nominal protocol id, unused by us */
  unsigned long foreground;
  unsigned long background;
  int line_width;
  int line_style;
  int fill_style;
  int function;    /* GXcopy and friends; non-copy modes
                      have no Canvas 2D analogue, so any
                      non-GXcopy draw is short-circuited
                      in the drawing primitives. */
  Font font;       /* currently bound font, or None */
  Pixmap tile;     /* FillTiled pixmap */
  Pixmap stipple;  /* FillStippled / FillOpaqueStippled pixmap */
  int ts_x_origin; /* tile/stipple x origin */
  int ts_y_origin; /* tile/stipple y origin */
};

/* ------------------------------------------------------------------------- */
/*  EmxWindow -- em-x11's per-window bookkeeping. Not visible to clients;   */
/*  they only ever see opaque Window (XID).                                  */
/* ------------------------------------------------------------------------- */

typedef struct EmxWindow {
  Window id;
  Window parent;
  int x, y;
  unsigned int width, height;
  unsigned int border_width;
  unsigned long border_pixel;
  unsigned long background_pixel;
  /* Window background_pixmap. None (=0) means "use background_pixel as a
   * solid fill"; any other id is a Pixmap whose content is tiled across
   * the window (X semantics: tile origin = window's top-left). The
   * classic X root weave lives here on the root window. */
  Pixmap background_pixmap;
  long event_mask;
  bool mapped;
  bool in_use;
  bool override_redirect;
  char name[64];

  /* SHAPE extension state (bounding region only for v1).
   * shape_bounding is NULL when the window has no custom shape, in which
   * case the window is a plain rectangle of its full size. When set, it
   * points to an array of `shape_bounding_count` XRectangle records,
   * each in window-local coordinates. */
  XRectangle* shape_bounding;
  int shape_bounding_count;
  unsigned long shape_event_mask; /* XShapeSelectInput subscription */

  /* Linked list of XChangeProperty payloads. See property.c. */
  struct EmxProperty* properties;
} EmxWindow;

typedef struct EmxProperty {
  struct EmxProperty* next;
  Atom name;
  Atom type;
  int format; /* 8, 16, or 32 */
  int nitems;
  unsigned char* data; /* raw bytes, length = nitems * fmt/8 */
} EmxProperty;

/* ------------------------------------------------------------------------- */
/*  struct _XDisplay                                                         */
/*                                                                           */
/*  The fields up to `xdefaults` mirror the anonymous struct in upstream    */
/*  Xlib.h (libX11-1.8.13, lines ~489-541). Order and types must remain in  */
/*  sync with whichever libX11 release we ship headers from -- changing a   */
/*  field here requires re-verifying Xlib.h.                                 */
/* ------------------------------------------------------------------------- */

struct _XPrivate;         /* forward decl -- we never define/use */
struct _XrmHashBucketRec; /* forward decl -- resource manager not wired yet */

struct _XDisplay {
  /* ---- public prefix (must match upstream _XPrivDisplay layout) ------ */
  XExtData* ext_data;
  struct _XPrivate* private1;
  int fd;
  int private2;
  int proto_major_version;
  int proto_minor_version;
  char* vendor;
  XID private3;
  XID private4;
  XID private5;
  int private6;
  XID (*resource_alloc)(struct _XDisplay*);
  int byte_order;
  int bitmap_unit;
  int bitmap_pad;
  int bitmap_bit_order;
  int nformats;
  ScreenFormat* pixmap_format;
  int private8;
  int release;
  struct _XPrivate* private9;
  struct _XPrivate* private10;
  int qlen;
  unsigned long last_request_read;
  unsigned long request;
  XPointer private11;
  XPointer private12;
  XPointer private13;
  XPointer private14;
  unsigned max_request_size;
  struct _XrmHashBucketRec* db;
  int (*private15)(struct _XDisplay*);
  char* display_name;
  int default_screen;
  int nscreens;
  Screen* screens;
  unsigned long motion_buffer;
  unsigned long private16;
  int min_keycode;
  int max_keycode;
  XPointer private17;
  XPointer private18;
  int private19;
  char* xdefaults;

  /* ---- em-x11 private tail (invisible to clients) -------------------- */
  Screen screen0;       /* backing for screens[0]   */
  Visual visual0;       /* root_visual of screen0   */
  Depth depth0;         /* single 24-bit depth      */
  ScreenFormat format0; /* 32-bit pixel format      */

  XID next_xid;

  /* Connection bookkeeping granted by the Host (the JS "X server")
   * when XOpenDisplay runs. `conn_id` identifies this wasm module in
   * the Host's connection table so events / redirect dispatch know
   * which queue to push into. `xid_base` and `xid_mask` carve out
   * this client's resource-id range per x11protocol.txt §869/§935:
   * every XID we hand out is `xid_base | (counter & xid_mask)`, and
   * the top three bits are always zero. Other connections use
   * non-overlapping bases so XIDs are globally unique without a
   * round-trip to the Host for each alloc. */
  int conn_id;
  XID xid_base;
  XID xid_mask;
  int wakeup_fd; /* write end of self-pipe */

  EmxWindow windows[EM_X11_MAX_WINDOWS];

  XEvent event_queue[EM_X11_EVENT_QUEUE_CAPACITY];
  unsigned int event_head; /* next slot to read        */
  unsigned int event_tail; /* next slot to write       */

  /* Keymap. em-x11 has no real hardware keyboard: we synthesize keycodes
   * on demand as new keysyms appear in browser events. keysym_table[kc]
   * holds the keysym assigned to keycode `kc`. X reserves keycodes 0..7,
   * so we start allocating at 8. */
  KeySym keysym_table[256];
  unsigned int next_keycode; /* next free keycode >= 8   */

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
  Window focus_window;
  int focus_revert_to;
  Time focus_last_time;

  /* ICCCM selection state. Selection ownership is server-global in real
   * X (dix/selection.c); here it's per-Display because each wasm module
   * is its own in-process "server". Cross-module selection is deferred.
   *
   * `selections` holds at most 8 concurrent owners; slot sel==0 means
   * free. Typical use: PRIMARY, CLIPBOARD, plus a few custom atoms.
   *
   * CLIPBOARD has a virtual owner (clipboard_proxy_win) that represents
   * the browser clipboard; when no real client owns CLIPBOARD,
   * XGetSelectionOwner(CLIPBOARD) returns this sentinel XID and
   * XConvertSelection(CLIPBOARD, ...) routes to the browser bridge in
   * selection.c instead of pushing a SelectionRequest.
   *
   * Atoms that don't have predefined ids are interned lazily on first
   * use (selection_ensure_atoms in selection.c) and cached here to
   * avoid repeat round-trips through em_x11_js_intern_atom. */
  struct {
    Atom sel;
    Window owner;
    Time time;
  } selections[8];

  Window clipboard_proxy_win;
  Atom atom_clipboard;
  Atom atom_utf8_string;
  Atom atom_targets;
  Atom atom_timestamp;
  Atom atom_text;
  Atom atom_incr;
  Atom atom_em_x11_clipboard_data;

  /* INCR back-channel state (ICCCM §2.7).
   *
   * Active while a chunked Tk→browser transfer is in flight.
   * Set by em_x11_selection_intercept_send when it sees an INCR marker
   * in the proxy's SelectionNotify reply; cleared by em_x11_incr_handle_chunk
   * when the zero-length terminator arrives.
   *
   *   incr_active   — nonzero while transfer is in progress
   *   incr_property — atom of the property on the proxy window used for
   *                   the transfer (matches ev->xselection.property in
   *                   the triggering SelectionNotify)
   *   incr_buf      — malloc'd accumulation buffer; NULL when empty
   *   incr_len      — bytes accumulated so far
   *   incr_cap      — allocated capacity of incr_buf */
  int incr_active;
  Atom incr_property;
  unsigned char* incr_buf;
  int incr_len;
  int incr_cap;

  /* XIM side-channel for typed UTF-8 (see xim.c).
   *
   *   pending_key_text     -- staged by JS (em_x11_set_pending_key_text)
   *                           right before each em_x11_push_key_event.
   *                           Captured into key_text_queue[tail] then
   *                           cleared, so a KeyRelease without text
   *                           can't inherit stale bytes.
   *   key_text_queue       -- parallel to event_queue. Slot N holds the
   *                           UTF-8 text that belongs to event_queue[N].
   *   key_text_len_queue   -- byte length of slot N (excluding NUL).
   *   current_key_text     -- text from the most-recently popped event,
   *                           served to Xutf8LookupString. */
  char pending_key_text[EM_X11_KEY_TEXT_SLOT];
  int pending_key_text_len;
  char key_text_queue[EM_X11_EVENT_QUEUE_CAPACITY][EM_X11_KEY_TEXT_SLOT];
  int key_text_len_queue[EM_X11_EVENT_QUEUE_CAPACITY];
  char current_key_text[EM_X11_KEY_TEXT_SLOT];
  int current_key_text_len;

  /* XIC linked list anchor. Each XCreateIC inserts at head;
   * XDestroyIC unlinks. The host calls preedit bridges with a
   * focus_window; we walk this list to find the owning XIC. */
  XIC xic_list;
};

/* ------------------------------------------------------------------------- */
/*  Internal helpers shared across native/ C files.                          */
/* ------------------------------------------------------------------------- */

Display* em_x11_get_display(void); /* singleton accessor        */

/* Register/unregister a Display self-pipe fd with the poll subsystem.
 * After registration, poll()/select() check the ring buffer directly
 * instead of consuming pipe bytes — see poll.c. */
void em_x11_poll_register_display_fd(int fd, Display* dpy);
void em_x11_poll_unregister_display_fd(int fd);

/* Signal delivery at cooperative yield points (signal.c).
 * Called after every emscripten_sleep return to fire any pending
 * SIGALRM / SIGCHLD / SIGINT / SIGPIPE / SIGTERM handlers.
 * Safe to call when no signals are pending (amortises to a no-op). */
void em_x11_deliver_pending_signals(void);
void em_x11_signal_set_pending(int sig);

/* vfork/exec coordination (fork.c, process.c).
 * em_x11_vfork_clear() must be called from the exec*() path so exit()
 * kills the wasm module instead of longjmp'ing back to vfork(). */
void em_x11_vfork_clear(void);
int em_x11_vfork_active(void);

EmxWindow* em_x11_window_find(Display* dpy, Window id);
EmxWindow* em_x11_window_alloc(Display* dpy);
XID em_x11_next_xid(Display* dpy);

bool em_x11_event_queue_push(Display* dpy, const XEvent* event);
bool em_x11_event_queue_pop(Display* dpy, XEvent* out);
unsigned int em_x11_event_queue_size(const Display* dpy);

/* Re-run the hit test against the cached pointer position and emit any
 * Enter/Leave pair the change implies. Called from XMapWindow /
 * XUnmapWindow so a window that pops up (or down) under a stationary
 * cursor still generates the crossing real X would. Required for twm's
 * root-menu hover, which gates on EnterNotify on the menu window. */
void em_x11_repoll_pointer_window(Display* dpy);

/* Reset the C-side implicit pointer grab. XGrabPointer calls this before
 * installing the active grab so the stale grab_window from the original
 * ButtonPress (which triggered the menu/combobox popup) doesn't keep
 * capturing ButtonRelease events meant for popup entries. Without this,
 * MenuButton items see ButtonPress but never ButtonRelease, so their
 * -command callbacks don't fire. */
void em_x11_reset_implicit_grab(void);

/* Remove the first event from the queue whose type's event-mask bit is
 * set in `mask`. Copies the event into *out and compacts the queue.
 * Returns true on hit, false if nothing matched. */
bool em_x11_event_queue_peek_match(Display* dpy, long mask, XEvent* out);

/* Remove the first event whose type == `type` and xany.window == `w`. */
bool em_x11_event_queue_peek_typed(Display* dpy,
                                   Window w,
                                   int type,
                                   XEvent* out);

/* Look up or allocate a keycode for a given keysym. Returns 0 if the
 * keycode table is exhausted (very unlikely in practice). */
KeyCode em_x11_keysym_to_keycode(Display* dpy, KeySym keysym);

/* Install the US QWERTY default keysyms at every evdev keycode slot.
 * Called once at Display init before host's getLayoutMap() patches
 * land. See event_keysym.c::em_x11_us_qwerty for the table. */
void em_x11_keysym_table_install_us_qwerty(Display* dpy);

/* Look up the CSS font string bound to a loaded Font id. Returns NULL
 * if the font hasn't been loaded (caller falls back to a default). */
const char* em_x11_font_css(Font font);

/* ------------------------------------------------------------------------- */
/*  JS bridge. These symbols are defined as EM_JS in src/bridges.c, so they  */
/*  are embedded in libem_x11 and resolve under both static link and          */
/*  SIDE_MODULE dlopen. The C side calls into the browser (canvas draw,      */
/*  DOM mutation) through them.                                              */
/* ------------------------------------------------------------------------- */

extern void em_x11_js_init(int screen_width, int screen_height);

/* Connection setup. Called by XOpenDisplay before anything else touches
 * the Host. The Host allocates a connection id and grants this client
 * an XID range (see struct _XDisplay for layout). `em_x11_js_close_display`
 * tells the Host to drop the connection; any resources still owned by
 * it are cleaned up server-side. */
extern void em_x11_js_open_display(int* conn_id_out,
                                   unsigned int* xid_base_out,
                                   unsigned int* xid_mask_out);
extern void em_x11_js_close_display(int conn_id);
/* Shared root window. Host owns the single root; every client's
 * XOpenDisplay asks for its XID instead of minting a local root. */
extern Window em_x11_js_get_root_window(void);

/* bg_type encodes the four xserver backgroundState values
 * (xserver/dix/window.c around line 1185):
 *   0 = None             (no auto-paint; previous pixels stay)
 *   1 = BackgroundPixel  (solid colour fill)
 *   2 = BackgroundPixmap (tile fill, bg_value is the Pixmap XID)
 * ParentRelative is currently mapped to None. The renderer must skip
 * any background paint when bg_type == 0, mirroring miPaintWindow's
 * `state != None` gate in xserver/mi/miwindow.c:115. */
extern void em_x11_js_window_create(int conn_id,
                                    Window id,
                                    Window parent,
                                    int x,
                                    int y,
                                    unsigned int w,
                                    unsigned int h,
                                    unsigned int border_width,
                                    unsigned long border_pixel,
                                    int bg_type,
                                    unsigned long bg_value);
/* Border-only update (XSetWindowBorder / XSetWindowBorderWidth /
 * XChangeWindowAttributes CWBorderPixel / XConfigureWindow CWBorderWidth).
 * Geometry stays unchanged; Host repaints the border ring around the
 * window's content rect. */
extern void em_x11_js_window_set_border(Window id,
                                        unsigned int border_width,
                                        unsigned long border_pixel);
/* Solid-background update (XSetWindowBackground / XChangeWindowAttributes
 * CWBackPixel). Repaints the window with the new colour. Distinct from
 * window_set_bg_pixmap which binds a tile pattern. */
extern void
em_x11_js_window_set_bg(Window id, int bg_type, unsigned long bg_value);
/* Geometry change on an existing window (XMoveWindow / XResizeWindow /
 * XConfigureWindow). Distinct from window_create so Host doesn't have
 * to re-seed parent, shape, or background_pixmap -- we only touch the
 * geometry fields. */
extern void em_x11_js_window_configure(
  int conn_id, Window id, int x, int y, unsigned int w, unsigned int h);
extern void em_x11_js_window_map(int conn_id, Window id);
extern void em_x11_js_window_unmap(int conn_id, Window id);
extern void em_x11_js_schedule_repoll(unsigned int conn_id);
extern void em_x11_js_window_destroy(Window id);
extern void em_x11_js_window_raise(Window id);
extern void em_x11_js_window_lower(Window id);
/* Per-window event-mask subscription. XSelectInput mirrors its new
 * value to the Host so SubstructureRedirect / SubstructureNotify holders
 * can be located without scanning every client's C-side window table. */
extern void em_x11_js_select_input(int conn_id, Window id, long mask);
/* Toggle a window's override_redirect flag on the Host side. Used by
 * XChangeWindowAttributes(CWOverrideRedirect). OR=True means "window
 * managers must not interfere" (popup menus, tooltips, twm's own
 * decoration frames) -- the Host skips redirect processing for them. */
extern void em_x11_js_set_override_redirect(Window id, int flag);
/* XChangeWindowAttributes(CWBitGravity) mirror. Per-window gravity
 * controls the backing-pixmap-on-resize policy: 1 (NorthWestGravity)
 * preserves top-left, 0 (ForgetGravity, X-default) discards and
 * triggers a full Expose for the new content rect. Without this
 * forwarding, Xaw widgets (default ForgetGravity) doubled their text
 * on resize. */
extern void em_x11_js_window_set_bit_gravity(Window id, int gravity);
/* XReparentWindow -- move a window under a new parent. (x, y) is the
 * new origin in the new parent's coordinate space. Always forwarded to
 * the Host even when the caller has no local shadow of `id`, because
 * cross-connection reparents are legal (twm reparenting xeyes's shell
 * is the canonical example). */
extern void em_x11_js_reparent_window(Window id, Window parent, int x, int y);
/* Bind or unbind a Pixmap as the window's tiled background. pm_id=0
 * reverts to the solid background_pixel; any other id must reference a
 * live Pixmap on the JS side. */
extern void em_x11_js_window_set_bg_pixmap(Window id, Pixmap pm_id);
/* XDefineCursor / XUndefineCursor mirror. cursor=0 means "inherit from
 * parent" (XUndefineCursor). Font cursors are encoded as
 * 0x70000000 | shape so the host can map shape -> CSS keyword without
 * a separate registration call. Pixmap cursors fall through to the
 * default arrow on the host side. */
extern void em_x11_js_window_set_cursor(Window id, unsigned int cursor);

/* Active-grab cursor override (XGrabPointer's cursor argument). While
 * a grab is active twm wants its MoveCursor / ResizeCursor displayed
 * everywhere the pointer roams, regardless of which window is under
 * it. cursor=0 clears the override; canvas cursor falls back to the
 * normal per-window XDefineCursor resolution. */
extern void em_x11_js_set_grab_cursor(unsigned int cursor);
/* XClearWindow / XClearArea entry. Lets the compositor decide whether to
 * paint with a solid colour or the window's background_pixmap without
 * the caller having to know. */
extern void
em_x11_js_clear_area(Window id, int x, int y, unsigned int w, unsigned int h);
extern void em_x11_js_fill_rect(
  Window id, int x, int y, unsigned int w, unsigned int h, unsigned long color);
extern void em_x11_js_draw_line(Window id,
                                int x1,
                                int y1,
                                int x2,
                                int y2,
                                unsigned long color,
                                int line_width);
extern void em_x11_js_flush(void);
extern void em_x11_js_flush_roundtrip(void);

/* Arc drawing: angles are X-semantics (64ths of a degree, counterclockwise
 * from 3 o'clock). The compositor converts to canvas-2d semantics
 * (radians, clockwise, origin at ellipse centre). */
extern void em_x11_js_draw_arc(Window id,
                               int x,
                               int y,
                               unsigned int width,
                               unsigned int height,
                               int angle1,
                               int angle2,
                               unsigned long color,
                               int line_width);
extern void em_x11_js_fill_arc(Window id,
                               int x,
                               int y,
                               unsigned int width,
                               unsigned int height,
                               int angle1,
                               int angle2,
                               unsigned long color);

/* Polygon: points are a flat int array of 2*count values (x0,y0,x1,y1,...).
 * The shape field is one of CoordModeOrigin (absolute) or CoordModePrevious
 * (relative to prior point). */
extern void em_x11_js_fill_polygon(Window id,
                                   const int* points,
                                   int count,
                                   int shape,
                                   int mode,
                                   unsigned long color);

/* Points: same flat int array layout as polygon. */
extern void em_x11_js_draw_points(
  Window id, const int* points, int count, int mode, unsigned long color);

/* Text rendering. font_css is a CSS `font` shorthand string (e.g.
 * "13px monospace"). image_mode=1 fills the text background with
 * bg_color (XDrawImageString semantics); image_mode=0 leaves the
 * background untouched (XDrawString). */
extern void em_x11_js_draw_string(Window id,
                                  int x,
                                  int y,
                                  const char* font_css,
                                  const char* text,
                                  int length,
                                  unsigned long fg_color,
                                  unsigned long bg_color,
                                  int image_mode);

/* Latin-1 variant: decodes `text` bytes as ISO 8859-1 instead of UTF-8.
 * Core X11 fonts (XDrawString/XDrawImageString) pass font-encoded text,
 * which is single-byte Latin-1 for Western fonts.  Xft paths use the
 * UTF-8 variant above. */
extern void em_x11_js_draw_string_latin1(Window id,
                                         int x,
                                         int y,
                                         const char* font_css,
                                         const char* text,
                                         int length,
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
extern void em_x11_js_measure_font(
  const char* font_css, int* ascent, int* descent, int* max_width, int* widths);

/* Measure the advance width of `length` bytes of `text` (interpreted as
 * UTF-8) in the given CSS font. Single round-trip to the browser; the
 * result matches what fillText will render pixel-for-pixel. */
extern int
em_x11_js_measure_string(const char* font_css, const char* text, int length);

/* Latin-1 variant for core X11 font measurement (XTextWidth). */
extern int em_x11_js_measure_string_latin1(const char* font_css,
                                           const char* text,
                                           int length);

/* SHAPE extension: push the new bounding rectangles to the compositor.
 * rects is an array of (x, y, width, height) int quadruples, length
 * 4 * count. Passing count==0 clears the shape (window returns to a
 * plain rectangle). */
extern void em_x11_js_window_shape(Window id, const int* rects, int count);

/* Last-known pointer position in canvas CSS pixels. XQueryPointer polls
 * this every time xeyes fires its 50ms tick. Writes are the JS host's
 * responsibility -- we just read the two ints back. */
extern void em_x11_js_pointer_xy(int* x_out, int* y_out);

/* Cross-connection XGetWindowAttributes fallback. When the caller has
 * no local EmxWindow for `id` (the WM case: twm querying xeyes's shell),
 * this returns the Host-authoritative state. `out` is an int[8] buffer:
 *   [0] found (0/1)  [1] x  [2] y
 *   [3] width        [4] height  [5] mapped
 *   [6] override_redirect  [7] border_width (Host tracks 0 for now)
 * When [0]==0, the Host doesn't know the window either; caller should
 * return 0 from XGetWindowAttributes. */
extern void em_x11_js_get_window_attrs(Window id, int* out);

/* Cross-connection XQueryTree: list mapped children of `parent` in
 * bottom-to-top stacking order. Two-call pattern (count, then fetch);
 * count returns 0 for unknown parent / no children. Used by twm's
 * RestartPreviousState walk after F_RESTART respawn so the new wasm
 * re-manages still-mapped client windows from the prior session. */
extern int em_x11_js_get_window_children_count(Window parent);
extern int em_x11_js_get_window_children(Window parent, int* out, int capacity);

/* Cross-connection abs-origin lookup. Returns 3 ints:
 *   [0] found (0/1)  [1] ax  [2] ay
 * Used by event.c::window_abs_origin when the parent chain in the
 * caller's local table dead-ends at a window owned by another
 * connection (e.g. xcalc walking up into a twm frame). */
extern void em_x11_js_get_window_abs_origin(Window id, int* out);

/* Cross-connection bounding-shape lookup, two-call pattern. The count
 * call returns:
 *    -1  -- window unknown to the host
 *     0  -- window known but rectangular (no shape)
 *    >0  -- number of XRectangles available
 * The fetch call writes (x, y, w, h) int quadruples into `dst` (int[4*n])
 * up to `capacity` rects, returning the number actually written. Used by
 * shape.c when the destination connection doesn't own the source window
 * (twm's XShapeQueryExtents / XShapeCombineShape on a foreign client).
 * Without these, twm's frame stays rectangular and the shape's hole
 * fails to pass clicks through to lower windows. */
extern int em_x11_js_get_window_shape_count(Window id);
extern int em_x11_js_get_window_shape_rects(Window id, int* dst, int capacity);

/* -- Property bridges (Host-owned storage, dix/property.c layout).
 * Properties are keyed by (XID, atom) server-side so any client can
 * read back what any client wrote. The four entry points mirror the
 * Xlib calls we expose. */
extern int em_x11_js_change_property(Window w,
                                     Atom atom,
                                     Atom type,
                                     int format,
                                     int mode,
                                     const unsigned char* data,
                                     int nelements);
/* XGetWindowProperty: two-call pattern so C owns the output buffer.
 * First call returns meta + required buffer size; second call copies
 * bytes into a caller-provided buffer and optionally deletes the
 * property atomically. meta layout (int[8]):
 *   [0] found (0/1)          [1] actual_type   [2] actual_format
 *   [3] nitems_returned      [4] bytes_after   [5] data_bytes
 *   [6] valid_window (0 => BadWindow, caller returns BadWindow)
 *   [7] reserved */
extern void em_x11_js_get_property_meta(Window w,
                                        Atom atom,
                                        Atom req_type,
                                        long long_offset,
                                        long long_length,
                                        int* meta_out);
extern void em_x11_js_get_property_data(Window w,
                                        Atom atom,
                                        Atom req_type,
                                        long long_offset,
                                        long long_length,
                                        int delete_flag,
                                        unsigned char* dst,
                                        int capacity);
extern void em_x11_js_delete_property(Window w, Atom atom);
extern int em_x11_js_list_properties_count(Window w);
extern int em_x11_js_list_properties_fetch(Window w, Atom* dst, int capacity);

/* Pixmap lifecycle. Create installs an OffscreenCanvas on the JS side,
 * keyed by id; destroy drops the reference so its backing bitmap can
 * be reclaimed. depth is 1 for SHAPE masks, 24/32 for color pixmaps. */
extern void
em_x11_js_pixmap_create(Pixmap id, int width, int height, int depth);
extern void em_x11_js_pixmap_destroy(Pixmap id);

/* SHAPE: decode a 1-bit pixmap into a bounding region and apply it to
 * the given window. op mirrors ShapeSet / ShapeUnion / etc. (op values
 * from X11/extensions/shape.h). */
extern void em_x11_js_shape_combine_mask(
  Window dest, Pixmap src, int x_off, int y_off, int op);
extern void
em_x11_js_shape_select_input(int conn_id, Window window, unsigned long mask);

/* XCopyArea / XCopyPlane / XPutImage bridges. Host dispatches by looking
 * up src/dst ids in its pixmap table: unknown id = window path, known =
 * OffscreenCanvas path. XCopyPlane is simplified to the depth-1 pixmap
 * source that Xaw/Tk actually use (see host.ts onCopyPlane). XPutImage
 * takes the already-sliced byte buffer; the C side is responsible for
 * computing bytes_per_line * height and pointing at image->data. */
extern void em_x11_js_copy_area(Drawable src,
                                Drawable dst,
                                int src_x,
                                int src_y,
                                unsigned int width,
                                unsigned int height,
                                int dst_x,
                                int dst_y);
extern void em_x11_js_copy_plane(Drawable src,
                                 Drawable dst,
                                 int src_x,
                                 int src_y,
                                 unsigned int width,
                                 unsigned int height,
                                 int dst_x,
                                 int dst_y,
                                 unsigned long plane,
                                 unsigned long fg,
                                 unsigned long bg,
                                 int apply_bg);
extern void em_x11_js_put_image(Drawable dst,
                                int dst_x,
                                int dst_y,
                                unsigned int width,
                                unsigned int height,
                                int format,
                                int depth,
                                int bytes_per_line,
                                const unsigned char* data,
                                int data_len,
                                unsigned long fg,
                                unsigned long bg);
extern void em_x11_js_get_image(Drawable drawable,
                                int x,
                                int y,
                                unsigned int w,
                                unsigned int h,
                                unsigned char* dst,
                                int dst_capacity);

/* Atom table. Predefined atoms 1..68 are still resolved locally in
 * atom.c for zero round-trip cost; anything else goes through Host
 * so every wasm module in the same page agrees on the id. Fixes the
 * WM_PROTOCOLS / WM_DELETE_WINDOW divergence the per-module tables
 * used to have. em_x11_js_get_atom_name returns a malloc'd string
 * that the caller releases via XFree (== free). */
extern Atom em_x11_js_intern_atom(const char* name, Bool only_if_exists);
extern char* em_x11_js_get_atom_name(Atom atom);

/* Internal pixmap accessors (implemented in pixmap.c). */
Bool em_x11_pixmap_exists(Pixmap id);
unsigned int em_x11_pixmap_depth(Pixmap id);
/* Refcount hooks for window background_pixmap ownership. acquire = +1
 * on the pixmap; release = decrement-or-destroy (mirrors XFreePixmap).
 * window.c calls these whenever a window starts/stops referencing a
 * pixmap as its CWBackPixmap, so the canvas outlives a client free. */
void em_x11_pixmap_acquire(Pixmap id);
void em_x11_pixmap_release(Display* dpy, Pixmap id);

/* Parse a CSS / X11 colour name (e.g. "slategrey", "gray85",
 * "rebeccapurple") by delegating to the browser's own colour parser.
 * Writes 16-bit per-channel values on success -- same precision as
 * XColor's red/green/blue fields. Returns 1 on success, 0 if the
 * name isn't recognised or no Host is installed. "rgb:R/G/B" and
 * "#RRGGBB" forms are handled in C; this bridge is only for bare
 * names, which are where the CSS spec's complete table saves us
 * from shipping an rgb.txt of our own. */
extern Status em_x11_js_parse_color(const char* name,
                                    unsigned short* red_out,
                                    unsigned short* green_out,
                                    unsigned short* blue_out);

/* Passive button grab table (XGrabButton / XUngrabButton). The host
 * keeps the (window, button, modifiers) -> grab-info map; devices.ts
 * walks the parent chain at ButtonPress time and redirects the event
 * to the deepest matching grab window. owner_events / pointer_mode /
 * keyboard_mode / confine_to / cursor are forwarded for fidelity but
 * the host's minimal impl honours only the routing -- there is no
 * sync-mode replay queue, so XAllowEvents is a stub. AnyButton (0)
 * and AnyModifier (1<<15) are wildcards on the host side. */
extern void em_x11_js_grab_button(Window window,
                                  unsigned int button,
                                  unsigned int modifiers,
                                  int owner_events,
                                  unsigned int event_mask,
                                  int pointer_mode,
                                  int keyboard_mode,
                                  Window confine_to,
                                  Cursor cursor);
extern void em_x11_js_ungrab_button(Window window,
                                    unsigned int button,
                                    unsigned int modifiers);
/* Active pointer grab plumbing -- see bridges.c. conn_id identifies the
 * calling wasm so subsequent button/motion events route back to it. */
extern void
em_x11_js_grab_pointer(unsigned int conn_id, Window window, int owner_events);
extern void em_x11_js_ungrab_pointer(void);
extern void em_x11_js_set_input_focus(Window window);

/* XIM bridge -- wires Tk's XSetICFocus / XSetICValues(XNSpotLocation)
 * to the host's hidden <textarea> overlay, which is what the OS IME
 * actually anchors its candidate window on. xim.c calls these. The host
 * ignores spot updates whose window doesn't currently own focus -- Tk
 * sets XNSpotLocation pre-emptively on every entry and we only want
 * the active widget's caret position to drive the overlay. */
extern void em_x11_js_xim_set_focus(Window window);
extern void em_x11_js_xim_clear_focus(void);
extern void em_x11_js_xim_set_spot(Window window, int x, int y);

/* Preedit bridges: host calls these from compositionstart/update/end on
 * the hidden textarea. See text-input.ts + xim.c */
extern void em_x11_xim_preedit_start(Window window);
extern void em_x11_xim_preedit_draw(
  Window window, const char* text, int caret, int chg_first, int chg_length);
extern void em_x11_xim_preedit_done(Window window);

/* xim.c side-channel hooks. event.c calls _capture_key_text after each
 * KeyPress/KeyRelease push so the parallel queue stays in lockstep with
 * event_queue; event_queue.c calls _capture_pop_text right before
 * advancing event_head so Xutf8LookupString sees the right slot. */
void em_x11_xim_capture_key_text(Display* dpy, unsigned int slot);
void em_x11_xim_capture_pop_text(Display* dpy, unsigned int slot);

/* Look up the XIC whose focus_window matches `w`. Returns NULL when no
 * XIC is attached to that window (not a text-input widget). */
XIC em_x11_find_xic_for_window(Display* dpy, Window w);

/* Called from event_queue.c::XFilterEvent. Returns True for KeyPress on
 * the focused window during active preedit so Tk skips normal key
 * processing (preedit callbacks handle composing text separately). */
Bool em_x11_xim_filter_event(XEvent* event, Window w);

/* Browser clipboard bridge (see selection.c). The read path is split in
 * two to keep the C side synchronous: first call awaits
 * navigator.clipboard.readText() (via the host) and stashes the UTF-8
 * bytes on the JS side, returning the byte length (or -1 on error);
 * second call copies up to `capacity` bytes into `dst` and clears the
 * stash. The write path is fire-and-forget (writeText Promise failures
 * logged to console, never propagated to C). */
extern int em_x11_js_clipboard_read_begin(void);
extern int em_x11_js_clipboard_read_fetch(unsigned char* dst, int capacity);
extern void em_x11_js_clipboard_write_utf8(const unsigned char* data, int len);

/* ------------------------------------------------------------------------- */
/*  Selection / ICCCM (selection.c).                                         */
/* ------------------------------------------------------------------------- */

/* Event push helpers used by both selection.c and XSendEvent's proxy
 * interception path in event.c. */
void em_x11_push_selection_clear(Display* dpy,
                                 Window owner,
                                 Atom selection,
                                 Time time);
void em_x11_push_selection_request(Display* dpy,
                                   Window owner,
                                   Window requestor,
                                   Atom selection,
                                   Atom target,
                                   Atom property,
                                   Time time);
void em_x11_push_shape_notify(Window window,
                              int kind,
                              int x,
                              int y,
                              unsigned int width,
                              unsigned int height,
                              Bool shaped);
void em_x11_push_selection_notify(Display* dpy,
                                  Window requestor,
                                  Atom selection,
                                  Atom target,
                                  Atom property,
                                  Time time);

/* Lazy-intern the cached CLIPBOARD / UTF8_STRING / TARGETS / ... atoms
 * on the Display if they haven't been set yet. Safe to call repeatedly;
 * amortises to zero after first hit. */
void em_x11_selection_ensure_atoms(Display* dpy);

/* Intercept hook used by XSendEvent: when a SelectionNotify is being
 * sent to the clipboard proxy window, reads the response property and
 * pushes it to the browser clipboard, returning True to indicate the
 * event was consumed (do not queue). Returns False for any other target,
 * in which case XSendEvent falls through to its normal path. */
Bool em_x11_selection_intercept_send(Display* dpy, Window w, const XEvent* ev);

/* Push a PropertyNotify event into the display queue.
 * state is PropertyNewValue (0) or PropertyDelete (1). */
void em_x11_push_property_notify(Display* dpy,
                                 Window win,
                                 Atom prop,
                                 int state);

/* Called from XChangeProperty when writing to the clipboard proxy window
 * during an active INCR transfer. Accumulates the chunk (or finalises the
 * transfer when nelements==0) and pushes the next PropertyNotify(Delete)
 * to drive the Tk owner's INCR state machine.
 * Returns True if the write was consumed (do NOT forward to the JS property
 * store). Returns False if this write is unrelated to the INCR transfer. */
Bool em_x11_incr_handle_chunk(Display* dpy,
                              Atom property,
                              const unsigned char* data,
                              int nelements,
                              int format);

#endif /* EM_X11_INTERNAL_H */
