#include "em_x11_internal.h"

#include <X11/Xutil.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define EM_X11_SCREEN_WIDTH 1024
#define EM_X11_SCREEN_HEIGHT 768
#define EM_X11_SCREEN_DEPTH 24

/* Per-module singleton. In the current architecture each wasm process
 * is a separate X client with its own Display, so a single instance is
 * correct. Multi-display support (one process → two X servers, as in
 * a real X desktop) would need to replace this with a per-display
 * registry (hash table keyed by display_name). */
static Display g_display;
static bool g_display_open = false;
static int g_open_count = 0;

Display* em_x11_get_display(void) { return &g_display; }

XID em_x11_next_xid(Display* dpy) {
  /* x11protocol.txt §935: client-allocated IDs are formed by ORing
   * an arbitrary value whose bits all lie within `resource-id-mask`
   * into `resource-id-base`. We keep a monotonic counter modulo the
   * mask, which gives every connection its own private 2 M-slot
   * range and never hits IDs reserved for Host-owned resources. */
  dpy->next_xid = (dpy->next_xid + 1) & dpy->xid_mask;
  return dpy->xid_base | dpy->next_xid;
}

static bool grow_window_table(Display* dpy) {
  int new_cap = dpy->window_capacity * 2;
  EmxWindow* new_windows =
    realloc(dpy->windows, (size_t)new_cap * sizeof(EmxWindow));
  if (!new_windows)
    return false;
  memset(new_windows + dpy->window_capacity,
         0,
         (size_t)(new_cap - dpy->window_capacity) * sizeof(EmxWindow));
  dpy->windows = new_windows;
  dpy->window_capacity = new_cap;
  return true;
}

EmxWindow* em_x11_window_alloc(Display* dpy) {
  for (int i = 0; i < dpy->window_count; i++) {
    if (!dpy->windows[i].in_use) {
      memset(&dpy->windows[i], 0, sizeof(EmxWindow));
      dpy->windows[i].in_use = true;
      return &dpy->windows[i];
    }
  }
  /* All slots in [0, window_count) are in-use. Try to grow. */
  if (dpy->window_count >= dpy->window_capacity) {
    if (!grow_window_table(dpy))
      return NULL;
  }
  int i = dpy->window_count++;
  memset(&dpy->windows[i], 0, sizeof(EmxWindow));
  dpy->windows[i].in_use = true;
  return &dpy->windows[i];
}

EmxWindow* em_x11_window_find(Display* dpy, Window id) {
  for (int i = 0; i < dpy->window_count; i++) {
    if (dpy->windows[i].in_use && dpy->windows[i].id == id) {
      return &dpy->windows[i];
    }
  }
  return NULL;
}

/* Default resource allocator used by the XAllocID macro in upstream Xlib.h.
 * Real Xlib syncs with the server; we just hand out monotonically. */
static XID em_x11_resource_alloc(Display* dpy) { return em_x11_next_xid(dpy); }

static void em_x11_init_screen(Display* dpy, int width, int height) {
  /* One 24-bit TrueColor visual, one depth, one pixmap format. */
  dpy->visual0.ext_data = NULL;
  dpy->visual0.visualid = 1;
  dpy->visual0.class = TrueColor;
  dpy->visual0.red_mask = 0x00FF0000UL;
  dpy->visual0.green_mask = 0x0000FF00UL;
  dpy->visual0.blue_mask = 0x000000FFUL;
  dpy->visual0.bits_per_rgb = 8;
  dpy->visual0.map_entries = 256;

  dpy->depth0.depth = EM_X11_SCREEN_DEPTH;
  dpy->depth0.nvisuals = 1;
  dpy->depth0.visuals = &dpy->visual0;

  dpy->format0.ext_data = NULL;
  dpy->format0.depth = EM_X11_SCREEN_DEPTH;
  dpy->format0.bits_per_pixel = 32;
  dpy->format0.scanline_pad = 32;

  Screen* s = &dpy->screen0;
  s->ext_data = NULL;
  s->display = dpy;
  s->root = 0; /* fixed up below */
  s->width = width;
  s->height = height;
  s->mwidth = 270; /* 96 dpi, ~10 inches wide  */
  s->mheight = 203;
  s->ndepths = 1;
  s->depths = &dpy->depth0;
  s->root_depth = EM_X11_SCREEN_DEPTH;
  s->root_visual = &dpy->visual0;
  s->default_gc = NULL; /* lazy; set on first use   */
  s->cmap = 1;          /* dummy colormap id        */
  s->white_pixel = 0x00FFFFFFUL;
  s->black_pixel = 0x00000000UL;
  s->max_maps = 1;
  s->min_maps = 1;
  s->backing_store = NotUseful;
  s->save_unders = False;
  s->root_input_mask = 0;
}

static int env_override(const char* name, int default_val) {
  const char* val = getenv(name);
  if (!val)
    return default_val;
  int n = atoi(val);
  return n > 0 ? n : default_val;
}

Display* XOpenDisplay(const char* display_name) {
  /* Force the bridges.c TU into the link so its EM_JS bodies survive
   * archive-pull semantics. See bridges.c for the rationale. */
  extern void em_x11_bridges_link_anchor(void);
  em_x11_bridges_link_anchor();
  (void)display_name;
  if (g_display_open) {
    g_open_count++;
    return &g_display;
  }

  int screen_w = env_override("EM_X11_SCREEN_WIDTH", EM_X11_SCREEN_WIDTH);
  int screen_h = env_override("EM_X11_SCREEN_HEIGHT", EM_X11_SCREEN_HEIGHT);

  memset(&g_display, 0, sizeof(g_display));

  /* Dynamic window table: start at 64 slots, double on exhaustion.
   * Real xorg allocates each window independently via dixAllocate;
   * a dynamic array with capacity doubling gives the same "no fixed
   * ceiling" property without per-window malloc churn. */
  g_display.window_capacity = EM_X11_WINDOW_INITIAL_CAPACITY;
  g_display.windows = calloc(EM_X11_WINDOW_INITIAL_CAPACITY, sizeof(EmxWindow));
  if (!g_display.windows) {
    return NULL;
  }

  /* Self-pipe: the read end replaces the (nonexistent) X socket fd so
   * that libXt's Select() has something real to block on. When the host
   * pushes an event into the C queue we write one byte to the write end,
   * waking Select() immediately instead of waiting for its timeout.
   *
   * Both ends are O_NONBLOCK: the read end so poll() can probe without
   * blocking, the write end so em_x11_event_queue_push doesn't stall if
   * the pipe buffer is full. */
  {
    int p[2];
    if (pipe(p) == 0) {
      g_display.fd = p[0];
      g_display.wakeup_fd = p[1];
      fcntl(p[0], F_SETFL, fcntl(p[0], F_GETFL) | O_NONBLOCK);
      fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK);

      /* Register with the poll subsystem so poll() can check the ring
       * buffer (non-destructive) instead of consuming pipe bytes. */
      em_x11_poll_register_display_fd(p[0], &g_display);
    }
  }

  /* Open a connection with the Host first: the returned XID range
   * must be in place before anything calls em_x11_next_xid. */
  int conn_id = 0;
  unsigned int xid_base = 0, xid_mask = 0;
  em_x11_js_open_display(&conn_id, &xid_base, &xid_mask);
  g_display.conn_id = conn_id;
  g_display.xid_base = (XID)xid_base;
  g_display.xid_mask = (XID)xid_mask;
  g_display.next_xid = 0;

  {
    extern void em_x11_js_close_display(int conn_id);
    extern int em_x11_current_conn_id(void);
    static bool s_atexit_registered = false;
    if (!s_atexit_registered) {
      s_atexit_registered = true;
      atexit(em_x11_atexit_cleanup);
    }
  }

  /* Public Display fields -- a plausible-looking minimum. Clients rarely
   * read these but Xt/Xaw inspect a few (protocol version, release). */
  g_display.proto_major_version = 11;
  g_display.proto_minor_version = 0;
  g_display.vendor = (char*)"em-x11";
  g_display.release = 1;
  g_display.byte_order = LSBFirst;
  g_display.bitmap_unit = 32;
  g_display.bitmap_pad = 32;
  g_display.bitmap_bit_order = LSBFirst;
  g_display.max_request_size = 65535;
  g_display.resource_alloc = em_x11_resource_alloc;
  g_display.display_name = (char*)":0";
  g_display.default_screen = 0;
  g_display.nscreens = 1;
  g_display.nformats = 1;
  g_display.pixmap_format = &g_display.format0;
  g_display.min_keycode = 8;
  g_display.max_keycode = 255;

  g_display.next_keycode = 8; /* X reserves 0..7          */

  /* Pre-populate the keysym table at evdev keycode positions with the
   * US QWERTY layout. The host will then call em_x11_install_keysym
   * for each entry in navigator.keyboard.getLayoutMap() to patch in
   * layout-specific keysyms before the first KeyPress is dispatched
   * (see src/host/keyboard-layout.ts). Browsers without the Keyboard
   * API (Firefox / Safari) keep the US QWERTY defaults -- character
   * input still works correctly because each KeyPress also carries
   * the resolved keysym from event.key, but XkbGetMap-style layout
   * introspection reports US QWERTY for those browsers.
   *
   * Why pre-fill instead of populating lazily on first event: Xt's
   * translation manager caches XGetKeyboardMapping on its first key
   * event; any keysym we install AFTER that snapshot is invisible to
   * Xt forever, so apps like xcalc/glxgears would only react to a
   * subset of keys (see project_em_x11_keysym_table_prepop). */
  em_x11_keysym_table_install_us_qwerty(&g_display);

  em_x11_init_screen(&g_display, screen_w, screen_h);
  g_display.screens = &g_display.screen0;

  /* Root window is Host-owned since Step 3a. Every client's XOpenDisplay
   * asks the Host for the shared root XID and installs a local shadow
   * in its EmxWindow table -- the authoritative record (and the weave
   * pixmap hanging off it) lives in the JS compositor. We do NOT call
   * em_x11_js_window_create for root: the Host already has the entry
   * and a second window_create for the same XID would either clobber
   * state or put two compositor rows in conflict. */
  Window root_xid = em_x11_js_get_root_window();
  EmxWindow* root = em_x11_window_alloc(&g_display);
  root->id = root_xid;
  root->parent = None;
  root->x = 0;
  root->y = 0;
  root->width = screen_w;
  root->height = screen_h;
  root->background_pixel = 0x00FFFFFFUL;
  root->mapped = true;
  g_display.screen0.root = root_xid;

  em_x11_js_init(screen_w, screen_h);

  g_display_open = true;
  g_open_count = 1;

  return &g_display;
}

/* Used by the weak execvp override in process.c (linked into demos
 * that opt in -- twm currently) to identify the calling wasm to the
 * host so it can target the right ProcessImpl for respawn. */
int em_x11_current_conn_id(void) {
  return g_display_open ? g_display.conn_id : 0;
}

int XCloseDisplay(Display* display) {
  (void)display;
  if (!g_display_open)
    return 0;
  g_open_count--;
  if (g_open_count > 0)
    return 0;
  em_x11_js_close_display(g_display.conn_id);
  g_display_open = false;
  return 0;
}

int XFlush(Display* display) {
  (void)display;
  em_x11_js_flush();
  return 0;
}

int XSync(Display* display, Bool discard) {
  /* Real XSync (libX11 Sync.c) sends GetInputFocus as a round-trip
   * request, blocks on _XReply, then drains the event queue to qfree
   * when discard=True. em-x11 has no server to round-trip, but we can
   * flush pending output and drain the local queue to match the
   * observable behaviour clients depend on. */
  XFlush(display);
  em_x11_js_flush_roundtrip();
  if (discard) {
    XEvent sink;
    while (em_x11_event_queue_size(display) > 0)
      em_x11_event_queue_pop(display, &sink);
  }
  return 0;
}

/* -- Function-form accessors. Upstream provides macros AND function forms; */
/*    clients may call either. We supply the functions.                      */

int XDefaultScreen(Display* display) { return display->default_screen; }

Window XDefaultRootWindow(Display* display) {
  return display->screens[display->default_screen].root;
}

Window XRootWindow(Display* display, int screen_number) {
  return display->screens[screen_number].root;
}

int XDisplayWidth(Display* display, int screen_number) {
  return display->screens[screen_number].width;
}

int XDisplayHeight(Display* display, int screen_number) {
  return display->screens[screen_number].height;
}

unsigned long XBlackPixel(Display* display, int screen_number) {
  return display->screens[screen_number].black_pixel;
}

unsigned long XWhitePixel(Display* display, int screen_number) {
  return display->screens[screen_number].white_pixel;
}

/* -- Generic allocator -- */

int XFree(void* data) {
  free(data);
  return 1;
}

/* -- Display / Screen accessors -- */

Display* XDisplayOfScreen(Screen* screen) {
  return screen ? screen->display : NULL;
}

int XScreenNumberOfScreen(Screen* screen) {
  (void)screen;
  return 0;
}

char* XDisplayName(const char* string) {
  if (string && *string)
    return (char*)string;
  return (char*)":0";
}

int XDisplayKeycodes(Display* dpy,
                     int* min_keycodes_return,
                     int* max_keycodes_return) {
  (void)dpy;
  if (min_keycodes_return)
    *min_keycodes_return = 8;
  if (max_keycodes_return)
    *max_keycodes_return = 255;
  return 1;
}

/* -- Visual matching -- */

Status XMatchVisualInfo(
  Display* dpy, int screen, int depth, int class_, XVisualInfo* vinfo_return) {
  if (!dpy || !vinfo_return)
    return 0;
  if (screen != 0)
    return 0;
  if (class_ != TrueColor)
    return 0;

  Visual* v = dpy->screens[0].root_visual;
  memset(vinfo_return, 0, sizeof(*vinfo_return));
  vinfo_return->visual = v;
  vinfo_return->visualid = v ? v->visualid : 0;
  vinfo_return->screen = 0;
  vinfo_return->depth = depth ? depth : dpy->screens[0].root_depth;
  vinfo_return->class = TrueColor;
  vinfo_return->red_mask = 0x00ff0000;
  vinfo_return->green_mask = 0x0000ff00;
  vinfo_return->blue_mask = 0x000000ff;
  vinfo_return->colormap_size = 256;
  vinfo_return->bits_per_rgb = 8;
  return 1;
}

/* -- X context manager (id-based hash) -- */

typedef struct ContextEntry {
  XID xid;
  XContext context;
  XPointer data;
  struct ContextEntry* next;
} ContextEntry;

static ContextEntry* g_context_head = NULL;

int XSaveContext(Display* dpy, XID xid, XContext context, const char* data) {
  (void)dpy;
  for (ContextEntry* e = g_context_head; e; e = e->next) {
    if (e->xid == xid && e->context == context) {
      e->data = (XPointer)data;
      return 0;
    }
  }
  ContextEntry* e = calloc(1, sizeof(*e));
  if (!e)
    return XCNOMEM;
  e->xid = xid;
  e->context = context;
  e->data = (XPointer)data;
  e->next = g_context_head;
  g_context_head = e;
  return 0;
}

int XFindContext(Display* dpy,
                 XID xid,
                 XContext context,
                 XPointer* data_return) {
  (void)dpy;
  for (ContextEntry* e = g_context_head; e; e = e->next) {
    if (e->xid == xid && e->context == context) {
      if (data_return)
        *data_return = e->data;
      return 0;
    }
  }
  /* Standard Xlib: do NOT write to *data_return when not found.
   * Callers (e.g. Motif DisplayInitialize) rely on the original
   * variable being preserved across a failed lookup. */
  return XCNOENT;
}

int XDeleteContext(Display* dpy, XID xid, XContext context) {
  (void)dpy;
  ContextEntry** link = &g_context_head;
  for (ContextEntry* e = g_context_head; e; link = &e->next, e = e->next) {
    if (e->xid == xid && e->context == context) {
      *link = e->next;
      free(e);
      return 0;
    }
  }
  return XCNOENT;
}

/* -- Internal-connection watches -- */

Status
XAddConnectionWatch(Display* dpy, XConnectionWatchProc proc, XPointer data) {
  (void)dpy;
  (void)proc;
  (void)data;
  return 1;
}

void XProcessInternalConnection(Display* dpy, int fd) {
  (void)dpy;
  (void)fd;
}

/* -- Display-level metadata -- */

Visual* XDefaultVisual(Display* dpy, int screen_number) {
  (void)screen_number;
  return dpy ? dpy->screens[0].root_visual : NULL;
}

Colormap XDefaultColormap(Display* dpy, int screen_number) {
  (void)screen_number;
  return dpy ? dpy->screens[0].cmap : 0;
}

int XDefaultDepth(Display* dpy, int screen_number) {
  (void)screen_number;
  return dpy ? dpy->screens[0].root_depth : 24;
}

unsigned long XNextRequest(Display* dpy) {
  (void)dpy;
  return 1UL;
}

int* XListDepths(Display* dpy, int screen_number, int* count_return) {
  (void)screen_number;
  int* out = malloc(sizeof(int));
  if (!out) {
    if (count_return)
      *count_return = 0;
    return NULL;
  }
  out[0] = dpy ? dpy->screens[0].root_depth : 24;
  if (count_return)
    *count_return = 1;
  return out;
}

long XMaxRequestSize(Display* dpy) {
  (void)dpy;
  return 262140;
}

/* -- Server grabs -- */

int XGrabServer(Display* dpy) {
  (void)dpy;
  return 1;
}
int XUngrabServer(Display* dpy) {
  (void)dpy;
  return 1;
}

/* -- Extension registration -- */

XExtCodes* XAddExtension(Display* dpy) {
  (void)dpy;
  XExtCodes* codes = calloc(1, sizeof(*codes));
  if (codes)
    codes->extension = 0;
  return codes;
}

typedef int (*em_x11_close_display_proc)(Display*, XExtCodes*);

em_x11_close_display_proc
XESetCloseDisplay(Display* dpy, int extension, em_x11_close_display_proc proc) {
  (void)dpy;
  (void)extension;
  (void)proc;
  return NULL;
}

/* -- Locale -- */

Bool XSupportsLocale(void) { return True; }

char* XSetLocaleModifiers(_Xconst char* modifier_list) {
  (void)modifier_list;
  return (char*)"";
}

/* -- Misc stubs -- */

int XBell(Display* dpy, int percent) {
  (void)dpy;
  (void)percent;
  return 1;
}

int XKillClient(Display* dpy, XID resource) {
  (void)dpy;
  (void)resource;
  return 1;
}

int XNoOp(Display* dpy) {
  (void)dpy;
  return 1;
}

XHostAddress* XListHosts(Display* dpy, int* nhosts_return, Bool* state_return) {
  (void)dpy;
  if (nhosts_return)
    *nhosts_return = 0;
  if (state_return)
    *state_return = False;
  return NULL;
}
