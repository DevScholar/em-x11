#include "emx11_internal.h"

#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

#define EMX11_SCREEN_WIDTH  1024
#define EMX11_SCREEN_HEIGHT  768
#define EMX11_SCREEN_DEPTH    24

static Display g_display;
static bool    g_display_open = false;

Display *emx11_get_display(void) {
    return &g_display;
}

XID emx11_next_xid(Display *dpy) {
    /* x11protocol.txt §935: client-allocated IDs are formed by ORing
     * an arbitrary value whose bits all lie within `resource-id-mask`
     * into `resource-id-base`. We keep a monotonic counter modulo the
     * mask, which gives every connection its own private 2 M-slot
     * range and never hits IDs reserved for Host-owned resources. */
    dpy->next_xid = (dpy->next_xid + 1) & dpy->xid_mask;
    return dpy->xid_base | dpy->next_xid;
}

EmxWindow *emx11_window_alloc(Display *dpy) {
    for (int i = 0; i < EMX11_MAX_WINDOWS; i++) {
        if (!dpy->windows[i].in_use) {
            memset(&dpy->windows[i], 0, sizeof(EmxWindow));
            dpy->windows[i].in_use = true;
            return &dpy->windows[i];
        }
    }
    return NULL;
}

EmxWindow *emx11_window_find(Display *dpy, Window id) {
    for (int i = 0; i < EMX11_MAX_WINDOWS; i++) {
        if (dpy->windows[i].in_use && dpy->windows[i].id == id) {
            return &dpy->windows[i];
        }
    }
    return NULL;
}

/* Default resource allocator used by the XAllocID macro in upstream Xlib.h.
 * Real Xlib syncs with the server; we just hand out monotonically. */
static XID emx11_resource_alloc(Display *dpy) {
    return emx11_next_xid(dpy);
}

static void emx11_init_screen(Display *dpy) {
    /* One 24-bit TrueColor visual, one depth, one pixmap format. */
    dpy->visual0.ext_data    = NULL;
    dpy->visual0.visualid    = 1;
    dpy->visual0.class       = TrueColor;
    dpy->visual0.red_mask    = 0x00FF0000UL;
    dpy->visual0.green_mask  = 0x0000FF00UL;
    dpy->visual0.blue_mask   = 0x000000FFUL;
    dpy->visual0.bits_per_rgb = 8;
    dpy->visual0.map_entries = 256;

    dpy->depth0.depth    = EMX11_SCREEN_DEPTH;
    dpy->depth0.nvisuals = 1;
    dpy->depth0.visuals  = &dpy->visual0;

    dpy->format0.ext_data       = NULL;
    dpy->format0.depth          = EMX11_SCREEN_DEPTH;
    dpy->format0.bits_per_pixel = 32;
    dpy->format0.scanline_pad   = 32;

    Screen *s = &dpy->screen0;
    s->ext_data        = NULL;
    s->display         = dpy;
    s->root            = 0;                     /* fixed up below */
    s->width           = EMX11_SCREEN_WIDTH;
    s->height          = EMX11_SCREEN_HEIGHT;
    s->mwidth          = 270;                   /* 96 dpi, ~10 inches wide  */
    s->mheight         = 203;
    s->ndepths         = 1;
    s->depths          = &dpy->depth0;
    s->root_depth      = EMX11_SCREEN_DEPTH;
    s->root_visual     = &dpy->visual0;
    s->default_gc      = NULL;                  /* lazy; set on first use   */
    s->cmap            = 1;                     /* dummy colormap id        */
    s->white_pixel     = 0x00FFFFFFUL;
    s->black_pixel     = 0x00000000UL;
    s->max_maps        = 1;
    s->min_maps        = 1;
    s->backing_store   = NotUseful;
    s->save_unders     = False;
    s->root_input_mask = 0;
}

Display *XOpenDisplay(const char *display_name) {
    /* Force the bridges.c TU into the link so its EM_JS bodies survive
     * archive-pull semantics. See bridges.c for the rationale. */
    extern void emx11_bridges_link_anchor(void);
    emx11_bridges_link_anchor();
    (void)display_name;
    if (g_display_open) {
        return &g_display;
    }

    memset(&g_display, 0, sizeof(g_display));

    /* Open a connection with the Host first: the returned XID range
     * must be in place before anything calls emx11_next_xid. */
    int conn_id = 0;
    unsigned int xid_base = 0, xid_mask = 0;
    emx11_js_open_display(&conn_id, &xid_base, &xid_mask);
    g_display.conn_id  = conn_id;
    g_display.xid_base = (XID)xid_base;
    g_display.xid_mask = (XID)xid_mask;
    g_display.next_xid = 0;

    /* Public Display fields -- a plausible-looking minimum. Clients rarely
     * read these but Xt/Xaw inspect a few (protocol version, release). */
    g_display.proto_major_version = 11;
    g_display.proto_minor_version = 0;
    g_display.vendor              = (char *)"em-x11";
    g_display.release             = 1;
    g_display.byte_order          = LSBFirst;
    g_display.bitmap_unit         = 32;
    g_display.bitmap_pad          = 32;
    g_display.bitmap_bit_order    = LSBFirst;
    g_display.max_request_size    = 65535;
    g_display.resource_alloc      = emx11_resource_alloc;
    g_display.display_name        = (char *)":0";
    g_display.default_screen      = 0;
    g_display.nscreens            = 1;
    g_display.nformats            = 1;
    g_display.pixmap_format       = &g_display.format0;
    g_display.min_keycode         = 8;
    g_display.max_keycode         = 255;

    g_display.next_keycode = 8;                 /* X reserves 0..7          */

    /* Pre-populate the keysym table at evdev keycode positions with the
     * US QWERTY layout. The host will then call emx11_install_keysym
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
     * subset of keys (see project_emx11_keysym_table_prepop). */
    emx11_keysym_table_install_us_qwerty(&g_display);

    emx11_init_screen(&g_display);
    g_display.screens = &g_display.screen0;

    /* Root window is Host-owned since Step 3a. Every client's XOpenDisplay
     * asks the Host for the shared root XID and installs a local shadow
     * in its EmxWindow table -- the authoritative record (and the weave
     * pixmap hanging off it) lives in the JS compositor. We do NOT call
     * emx11_js_window_create for root: the Host already has the entry
     * and a second window_create for the same XID would either clobber
     * state or put two compositor rows in conflict. */
    Window root_xid = emx11_js_get_root_window();
    EmxWindow *root = emx11_window_alloc(&g_display);
    root->id               = root_xid;
    root->parent           = None;
    root->x                = 0;
    root->y                = 0;
    root->width            = EMX11_SCREEN_WIDTH;
    root->height           = EMX11_SCREEN_HEIGHT;
    root->background_pixel = 0x00FFFFFFUL;
    root->mapped           = true;
    g_display.screen0.root = root_xid;

    emx11_js_init(EMX11_SCREEN_WIDTH, EMX11_SCREEN_HEIGHT);

    g_display_open = true;

    return &g_display;
}

/* Used by the weak execvp override in process.c (linked into demos
 * that opt in -- twm currently) to identify the calling wasm to the
 * host so it can target the right ProcessImpl for respawn. */
int emx11_current_conn_id(void) {
    return g_display_open ? g_display.conn_id : 0;
}

int XCloseDisplay(Display *display) {
    (void)display;
    if (g_display_open) {
        emx11_js_close_display(g_display.conn_id);
    }
    g_display_open = false;
    return 0;
}

int XFlush(Display *display) {
    (void)display;
    emx11_js_flush();
    return 0;
}

int XSync(Display *display, Bool discard) {
    (void)discard;
    return XFlush(display);
}

/* -- Function-form accessors. Upstream provides macros AND function forms; */
/*    clients may call either. We supply the functions.                      */

int XDefaultScreen(Display *display) {
    return display->default_screen;
}

Window XDefaultRootWindow(Display *display) {
    return display->screens[display->default_screen].root;
}

Window XRootWindow(Display *display, int screen_number) {
    return display->screens[screen_number].root;
}

int XDisplayWidth(Display *display, int screen_number) {
    return display->screens[screen_number].width;
}

int XDisplayHeight(Display *display, int screen_number) {
    return display->screens[screen_number].height;
}

unsigned long XBlackPixel(Display *display, int screen_number) {
    return display->screens[screen_number].black_pixel;
}

unsigned long XWhitePixel(Display *display, int screen_number) {
    return display->screens[screen_number].white_pixel;
}

/* -- Generic allocator -- */

int XFree(void *data) {
    free(data);
    return 1;
}

/* -- Display / Screen accessors -- */

Display *XDisplayOfScreen(Screen *screen) {
    return screen ? screen->display : NULL;
}

int XScreenNumberOfScreen(Screen *screen) {
    (void)screen;
    return 0;
}

char *XDisplayName(const char *string) {
    if (string && *string) return (char *)string;
    return (char *)":0";
}

int XDisplayKeycodes(Display *dpy, int *min_keycodes_return,
                     int *max_keycodes_return) {
    (void)dpy;
    if (min_keycodes_return) *min_keycodes_return = 8;
    if (max_keycodes_return) *max_keycodes_return = 255;
    return 1;
}

/* -- Visual matching -- */

Status XMatchVisualInfo(Display *dpy, int screen, int depth,
                        int class_, XVisualInfo *vinfo_return) {
    if (!dpy || !vinfo_return) return 0;
    if (screen != 0) return 0;
    if (class_ != TrueColor) return 0;

    Visual *v = dpy->screens[0].root_visual;
    memset(vinfo_return, 0, sizeof(*vinfo_return));
    vinfo_return->visual        = v;
    vinfo_return->visualid      = v ? v->visualid : 0;
    vinfo_return->screen        = 0;
    vinfo_return->depth         = depth ? depth : dpy->screens[0].root_depth;
    vinfo_return->class         = TrueColor;
    vinfo_return->red_mask      = 0x00ff0000;
    vinfo_return->green_mask    = 0x0000ff00;
    vinfo_return->blue_mask     = 0x000000ff;
    vinfo_return->colormap_size = 256;
    vinfo_return->bits_per_rgb  = 8;
    return 1;
}

/* -- X context manager (id-based hash) -- */

typedef struct ContextEntry {
    XID                  xid;
    XContext             context;
    XPointer             data;
    struct ContextEntry *next;
} ContextEntry;

static ContextEntry *g_context_head = NULL;

int XSaveContext(Display *dpy, XID xid, XContext context, const char *data) {
    (void)dpy;
    for (ContextEntry *e = g_context_head; e; e = e->next) {
        if (e->xid == xid && e->context == context) {
            e->data = (XPointer)data;
            return 0;
        }
    }
    ContextEntry *e = calloc(1, sizeof(*e));
    if (!e) return XCNOMEM;
    e->xid = xid;
    e->context = context;
    e->data = (XPointer)data;
    e->next = g_context_head;
    g_context_head = e;
    return 0;
}

int XFindContext(Display *dpy, XID xid, XContext context, XPointer *data_return) {
    (void)dpy;
    for (ContextEntry *e = g_context_head; e; e = e->next) {
        if (e->xid == xid && e->context == context) {
            if (data_return) *data_return = e->data;
            return 0;
        }
    }
    if (data_return) *data_return = NULL;
    return XCNOENT;
}

int XDeleteContext(Display *dpy, XID xid, XContext context) {
    (void)dpy;
    ContextEntry **link = &g_context_head;
    for (ContextEntry *e = g_context_head; e; link = &e->next, e = e->next) {
        if (e->xid == xid && e->context == context) {
            *link = e->next;
            free(e);
            return 0;
        }
    }
    return XCNOENT;
}

/* -- Internal-connection watches -- */

Status XAddConnectionWatch(Display *dpy, XConnectionWatchProc proc,
                           XPointer data) {
    (void)dpy; (void)proc; (void)data;
    return 1;
}

void XProcessInternalConnection(Display *dpy, int fd) {
    (void)dpy; (void)fd;
}

/* -- Display-level metadata -- */

Visual *XDefaultVisual(Display *dpy, int screen_number) {
    (void)screen_number;
    return dpy ? dpy->screens[0].root_visual : NULL;
}

Colormap XDefaultColormap(Display *dpy, int screen_number) {
    (void)screen_number;
    return dpy ? dpy->screens[0].cmap : 0;
}

int XDefaultDepth(Display *dpy, int screen_number) {
    (void)screen_number;
    return dpy ? dpy->screens[0].root_depth : 24;
}

unsigned long XNextRequest(Display *dpy) {
    (void)dpy; return 1UL;
}

int *XListDepths(Display *dpy, int screen_number, int *count_return) {
    (void)screen_number;
    int *out = malloc(sizeof(int));
    if (!out) {
        if (count_return) *count_return = 0;
        return NULL;
    }
    out[0] = dpy ? dpy->screens[0].root_depth : 24;
    if (count_return) *count_return = 1;
    return out;
}

long XMaxRequestSize(Display *dpy) {
    (void)dpy;
    return 262140;
}

/* -- Server grabs -- */

int XGrabServer(Display *dpy)   { (void)dpy; return 1; }
int XUngrabServer(Display *dpy) { (void)dpy; return 1; }

/* -- Extension registration -- */

XExtCodes *XAddExtension(Display *dpy) {
    (void)dpy;
    XExtCodes *codes = calloc(1, sizeof(*codes));
    if (codes) codes->extension = 0;
    return codes;
}

typedef int (*emx11_close_display_proc)(Display *, XExtCodes *);

emx11_close_display_proc XESetCloseDisplay(Display *dpy, int extension,
                                           emx11_close_display_proc proc) {
    (void)dpy; (void)extension; (void)proc;
    return NULL;
}

/* -- Locale -- */

Bool XSupportsLocale(void) {
    return True;
}

char *XSetLocaleModifiers(_Xconst char *modifier_list) {
    (void)modifier_list;
    return (char *)"";
}

/* -- Misc stubs -- */

int XBell(Display *dpy, int percent) {
    (void)dpy; (void)percent;
    return 1;
}

int XKillClient(Display *dpy, XID resource) {
    (void)dpy; (void)resource;
    return 1;
}

int XNoOp(Display *dpy) {
    (void)dpy;
    return 1;
}

XHostAddress *XListHosts(Display *dpy, int *nhosts_return, Bool *state_return) {
    (void)dpy;
    if (nhosts_return) *nhosts_return = 0;
    if (state_return)  *state_return  = False;
    return NULL;
}
