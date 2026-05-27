/*
 * em-x11 GLX implementation.
 *
 * Per-context model:
 *   - Each GLXContext owns an OffscreenCanvas (allocated host-side via
 *     emx11_js_glx_create_context, registered in
 *     Module.specialHTMLTargets["!emx11-glx-N"]) and the
 *     EMSCRIPTEN_WEBGL_CONTEXT_HANDLE that emscripten_webgl_create_context
 *     returns when we point it at that target.
 *   - glXMakeCurrent toggles the active emscripten WebGL context. The
 *     demo's libGL (LEGACY_GL_EMULATION=1) routes every glBegin /
 *     glRotatef / glClear into the current context's canvas.
 *   - glXSwapBuffers asks the host to drawImage(offscreen) into the
 *     X window's 2D backing surface; the compositor stitches it in
 *     at the next rAF.
 *
 * Pixmap drawables and indirect rendering are not supported. Visual /
 * FBConfig query is canned (single 32-bit RGBA + 24-bit depth).
 */

#include "emx11_internal.h"

#include <GL/gl.h>
#include <GL/glx.h>

#include <emscripten.h>
#include <emscripten/em_js.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

#include <stdlib.h>
#include <string.h>

/* Bridges in src/bridges.c (EM_JS). */
extern int  emx11_js_glx_create_context(int width, int height,
                                        int outTargetIdPtr, int outTargetIdLen);
extern void emx11_js_glx_destroy_context(int id);
extern void emx11_js_glx_swap_buffers(int id, unsigned int drawable);
extern void emx11_js_glx_resize(int id, int width, int height);
extern void emx11_js_glx_legacy_init_once(void);

/* Default initial GL surface size. glXCreateContext doesn't get a
 * drawable, so we allocate at a reasonable default and resize on first
 * MakeCurrent. glxgears starts at 300x300; pick something close. */
#define EMX11_GLX_DEFAULT_W 300
#define EMX11_GLX_DEFAULT_H 300

struct __GLXcontextRec {
    Display    *dpy;
    XVisualInfo vis;
    Bool        direct;
    int         js_id;          /* GlxManager-side id; 0 means uninitialised. */
    char        target_id[32];  /* "!emx11-glx-N" registered in specialHTMLTargets. */
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE webgl;  /* 0 until first MakeCurrent. */
    int         width;
    int         height;
    GLXDrawable drawable;       /* Last drawable bound via MakeCurrent. */
};

struct __GLXFBConfigRec {
    int placeholder;
};

/* --- query / version ----------------------------------------------------- */

Bool glXQueryExtension(Display *dpy, int *errorBase, int *eventBase) {
    (void)dpy;
    if (errorBase) *errorBase = 128;
    if (eventBase) *eventBase = 64;
    return True;
}

Bool glXQueryVersion(Display *dpy, int *major, int *minor) {
    (void)dpy;
    if (major) *major = 1;
    if (minor) *minor = 4;
    return True;
}

const char *glXQueryExtensionsString(Display *dpy, int screen) {
    (void)dpy; (void)screen;
    return "";
}

const char *glXQueryServerString(Display *dpy, int screen, int name) {
    (void)dpy; (void)screen;
    switch (name) {
        case GLX_VENDOR:     return "em-x11";
        case GLX_VERSION:    return "1.4";
        case GLX_EXTENSIONS: return "";
        default:             return "";
    }
}

const char *glXGetClientString(Display *dpy, int name) {
    return glXQueryServerString(dpy, 0, name);
}

/* --- visual / fbconfig --------------------------------------------------- */

static void emx11_fill_default_visual(Display *dpy, XVisualInfo *out) {
    memset(out, 0, sizeof(*out));
    /* Use the display's actual root visual so XCreateColormap /
     * XCreateWindow accept the visinfo->visual pointer. display.c
     * sets visual0.visualid = 1 and class = TrueColor. */
    out->visual        = dpy ? &dpy->visual0 : NULL;
    out->visualid      = dpy ? dpy->visual0.visualid : 1;
    out->screen        = 0;
    out->depth         = 24;
    out->class         = TrueColor;
    out->red_mask      = 0x00FF0000;
    out->green_mask    = 0x0000FF00;
    out->blue_mask     = 0x000000FF;
    out->colormap_size = 256;
    out->bits_per_rgb  = 8;
}

XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attribList) {
    (void)screen; (void)attribList;
    XVisualInfo *vi = (XVisualInfo *)malloc(sizeof(*vi));
    if (!vi) return NULL;
    emx11_fill_default_visual(dpy, vi);
    return vi;
}

int glXGetConfig(Display *dpy, XVisualInfo *vis, int attrib, int *value) {
    (void)dpy; (void)vis;
    if (!value) return GLX_BAD_VALUE;
    switch (attrib) {
        case GLX_USE_GL:        *value = 1;  break;
        case GLX_RGBA:          *value = 1;  break;
        case GLX_DOUBLEBUFFER:  *value = 1;  break;
        case GLX_RED_SIZE:      *value = 8;  break;
        case GLX_GREEN_SIZE:    *value = 8;  break;
        case GLX_BLUE_SIZE:     *value = 8;  break;
        case GLX_ALPHA_SIZE:    *value = 8;  break;
        case GLX_DEPTH_SIZE:    *value = 24; break;
        case GLX_STENCIL_SIZE:  *value = 8;  break;
        case GLX_BUFFER_SIZE:   *value = 32; break;
        case GLX_LEVEL:         *value = 0;  break;
        case GLX_STEREO:        *value = 0;  break;
        case GLX_AUX_BUFFERS:   *value = 0;  break;
        default:                *value = 0;  return GLX_BAD_ATTRIBUTE;
    }
    return 0;
}

GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen,
                               const int *attribList, int *nitems) {
    (void)dpy; (void)screen; (void)attribList;
    GLXFBConfig *arr = (GLXFBConfig *)malloc(sizeof(GLXFBConfig));
    static struct __GLXFBConfigRec one = { 0 };
    if (!arr) { if (nitems) *nitems = 0; return NULL; }
    arr[0] = &one;
    if (nitems) *nitems = 1;
    return arr;
}

XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config) {
    (void)config;
    return glXChooseVisual(dpy, 0, NULL);
}

int glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config, int attribute, int *value) {
    (void)config;
    return glXGetConfig(dpy, NULL, attribute, value);
}

/* --- context lifecycle --------------------------------------------------- */

static GLXContext  g_current_ctx       = NULL;
static GLXDrawable g_current_drawable  = 0;
static Display    *g_current_display   = NULL;

/* Lazy WebGL context creation: postponed until first MakeCurrent so
 * the GL surface size can match the drawable. Returns True on success. */
static Bool emx11_glx_realize(GLXContext ctx) {
    if (ctx->webgl) return True;

    int js_id = emx11_js_glx_create_context(
        ctx->width, ctx->height,
        (int)(uintptr_t)ctx->target_id, (int)sizeof(ctx->target_id));
    if (js_id <= 0) return False;
    ctx->js_id = js_id;

    EmscriptenWebGLContextAttributes attrs;
    emscripten_webgl_init_context_attributes(&attrs);
    attrs.alpha                 = false;
    attrs.depth                 = true;
    attrs.stencil               = false;
    attrs.antialias             = true;
    attrs.premultipliedAlpha    = false;
    attrs.preserveDrawingBuffer = false;
    /* WebGL1 is required for emscripten's LEGACY_GL_EMULATION fixed-
     * function pipeline; demos opt in via -sLEGACY_GL_EMULATION=1. */
    attrs.majorVersion = 1;
    attrs.minorVersion = 0;
    attrs.enableExtensionsByDefault = true;

    ctx->webgl = emscripten_webgl_create_context(ctx->target_id, &attrs);
    if (!ctx->webgl) {
        emx11_js_glx_destroy_context(ctx->js_id);
        ctx->js_id = 0;
        return False;
    }
    return True;
}

GLXContext glXCreateContext(Display *dpy, XVisualInfo *vis,
                            GLXContext shareList, Bool direct) {
    (void)shareList;
    GLXContext ctx = (GLXContext)calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;
    ctx->dpy    = dpy;
    if (vis) ctx->vis = *vis;
    ctx->direct = direct;
    ctx->width  = EMX11_GLX_DEFAULT_W;
    ctx->height = EMX11_GLX_DEFAULT_H;
    /* webgl context creation deferred to MakeCurrent so we know the
     * drawable's actual size before allocating the GL surface. */
    return ctx;
}

void glXDestroyContext(Display *dpy, GLXContext ctx) {
    (void)dpy;
    if (!ctx) return;
    if (ctx->webgl) {
        emscripten_webgl_destroy_context(ctx->webgl);
    }
    if (ctx->js_id) {
        emx11_js_glx_destroy_context(ctx->js_id);
    }
    if (ctx == g_current_ctx) {
        g_current_ctx      = NULL;
        g_current_drawable = 0;
        g_current_display  = NULL;
    }
    free(ctx);
}

/* Resize the GL surface to match the drawable's current geometry, so
 * the demo's glViewport (sized after the X window) hits an identically
 * sized framebuffer. */
static void emx11_glx_match_drawable(GLXContext ctx, Display *dpy, GLXDrawable drawable) {
    if (!dpy || !drawable) return;
    XWindowAttributes wa;
    if (XGetWindowAttributes(dpy, (Window)drawable, &wa) == 0) return;
    int w = wa.width  > 0 ? wa.width  : 1;
    int h = wa.height > 0 ? wa.height : 1;
    if (w == ctx->width && h == ctx->height) return;
    ctx->width  = w;
    ctx->height = h;
    if (ctx->js_id) emx11_js_glx_resize(ctx->js_id, w, h);
}

Bool glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx) {
    if (!ctx) {
        emscripten_webgl_make_context_current(0);
        g_current_display  = dpy;
        g_current_drawable = 0;
        g_current_ctx      = NULL;
        return True;
    }
    /* Pick up drawable size BEFORE allocating the WebGL context so the
     * first frame is rendered at the right resolution. */
    if (drawable) {
        ctx->drawable = drawable;
        emx11_glx_match_drawable(ctx, dpy, drawable);
    }
    if (!emx11_glx_realize(ctx)) return False;
    if (emscripten_webgl_make_context_current(ctx->webgl) != EMSCRIPTEN_RESULT_SUCCESS) {
        return False;
    }
    /* GLctx is now bound; safe to drive emscripten's LEGACY_GL_EMULATION
     * one-time init that Browser.createContext would normally do. */
    emx11_js_glx_legacy_init_once();
    g_current_display  = dpy;
    g_current_drawable = drawable;
    g_current_ctx      = ctx;
    return True;
}

Bool glXMakeContextCurrent(Display *dpy, GLXDrawable draw, GLXDrawable read,
                           GLXContext ctx) {
    (void)read;
    return glXMakeCurrent(dpy, draw, ctx);
}

GLXContext  glXGetCurrentContext(void)  { return g_current_ctx; }
GLXDrawable glXGetCurrentDrawable(void) { return g_current_drawable; }
Display    *glXGetCurrentDisplay(void)  { return g_current_display; }

Bool glXIsDirect(Display *dpy, GLXContext ctx) {
    (void)dpy;
    return ctx ? ctx->direct : False;
}

/* --- frame presentation -------------------------------------------------- */

void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
    (void)dpy;
    GLXContext ctx = g_current_ctx;
    if (!ctx || !ctx->js_id) return;
    /* If the drawable has been resized since last frame, sync the GL
     * surface here so the next frame draws at the new size. The demo
     * still has to re-issue glViewport on its own ConfigureNotify --
     * we just keep the destination buffer matched. */
    emx11_glx_match_drawable(ctx, ctx->dpy ? ctx->dpy : dpy, drawable);
    /* Flush the LEGACY_GL_EMULATION immediate-mode buffer so all GL
     * commands are committed to the OffscreenCanvas before we blit it
     * into the X window backing surface. Without this the canvas may
     * be blank/stale. */
    glFlush();
    emx11_js_glx_swap_buffers(ctx->js_id, (unsigned int)drawable);
    /* Yield to the browser so the compositor's requestAnimationFrame
     * callback can fire and paint the backing surface to the display
     * canvas. Without this the wasm render loop starves rAF and only
     * the root window is ever visible. */
    emscripten_sleep(0);
}

void glXWaitGL(void) { /* no-op; WebGL is implicitly flushed */ }
void glXWaitX(void)  { /* no-op; em-x11 has no async X server */ }

/* --- extension dispatch -------------------------------------------------- */

void (*glXGetProcAddress(const GLubyte *procName))(void) {
    (void)procName;
    return NULL;
}

void (*glXGetProcAddressARB(const GLubyte *procName))(void) {
    return glXGetProcAddress(procName);
}
