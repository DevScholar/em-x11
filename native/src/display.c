#include "emx11_internal.h"

#include <stdlib.h>
#include <string.h>

#define EMX11_SCREEN_WIDTH  1024
#define EMX11_SCREEN_HEIGHT  768
#define EMX11_SCREEN_DEPTH    24

static Display g_display;
static bool    g_display_open = false;

static void emx11_atexit_cleanup(void);

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

    /* Pre-populate the keysym table with the standard keysyms our TS-side
     * keymap can produce (runtime/keymap.ts). Xt's translation manager
     * calls XGetKeyboardMapping once at first key event and caches the
     * result; any keysym we lazily allocated AFTER that snapshot is
     * invisible to Xt forever, so apps like xcalc/glxgears would only
     * react to whichever single keysym happened to populate the table
     * before Xt's cache build. Tk does not cache this way, hence Tk apps
     * worked fine. By installing the full set up front, Xt's cached
     * keymap matches the live one for every key the TS layer can send. */
    {
        unsigned int kc = 8;

        /* ASCII printable 0x20..0x7e -- keysym == codepoint per X11. */
        for (KeySym ks = 0x20; ks <= 0x7e && kc < 256; ks++) {
            g_display.keysym_table[kc++] = ks;
        }

        /* Common terminal-control keys also handled by XLookupString. */
        static const KeySym k_specials[] = {
            0xff0d, /* XK_Return     */
            0xff09, /* XK_Tab        */
            0xff08, /* XK_BackSpace  */
            0xff1b, /* XK_Escape     */
            0xffff, /* XK_Delete     */
            0xff63, /* XK_Insert     */
            0xff50, /* XK_Home       */
            0xff57, /* XK_End        */
            0xff55, /* XK_Page_Up    */
            0xff56, /* XK_Page_Down  */
            0xff51, /* XK_Left       */
            0xff52, /* XK_Up         */
            0xff53, /* XK_Right      */
            0xff54, /* XK_Down       */
            0xffe5, /* XK_Caps_Lock  */
            0xffe1, /* XK_Shift_L    */
            0xffe3, /* XK_Control_L  */
            0xffe9, /* XK_Alt_L      */
            0xffeb, /* XK_Super_L    */
            0xffbe, 0xffbf, 0xffc0, 0xffc1, 0xffc2, 0xffc3,
            0xffc4, 0xffc5, 0xffc6, 0xffc7, 0xffc8, 0xffc9, /* F1..F12 */
        };
        for (size_t i = 0; i < sizeof(k_specials)/sizeof(k_specials[0])
                          && kc < 256; i++) {
            g_display.keysym_table[kc++] = k_specials[i];
        }

        g_display.next_keycode = kc;
    }

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

    /* Catch exit() paths that skip XCloseDisplay -- see emx11_atexit_cleanup. */
    static bool atexit_registered = false;
    if (!atexit_registered) {
        atexit(emx11_atexit_cleanup);
        atexit_registered = true;
    }

    return &g_display;
}

int XCloseDisplay(Display *display) {
    (void)display;
    if (g_display_open) {
        emx11_js_close_display(g_display.conn_id);
    }
    g_display_open = false;
    return 0;
}

/* Run on exit(0) / abort(). Apps that bypass XCloseDisplay (xcalc's
 * Quit() defers XtDestroyApplicationContext, then immediately exit()s
 * before Xt's main loop drains the destroy list) would otherwise leave
 * their windows on the host's compositor as inert frames -- the user
 * sees a "frozen" window because the wasm process is gone but the host
 * has no signal to tear down. atexit fires before Emscripten's runtime
 * cleanup, so the host still has a live module to ccall back through. */
static void emx11_atexit_cleanup(void) {
    if (g_display_open) {
        emx11_js_close_display(g_display.conn_id);
        g_display_open = false;
    }
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
