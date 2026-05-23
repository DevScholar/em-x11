/*
 * em-x11 GLX 1.4 header — minimal subset.
 *
 * Just enough of <GL/glx.h> to compile glxgears and similar fixed-pipeline
 * GLX clients against em-x11. Backed by native/glx/glx.c, which bridges
 * GLXContext → emscripten WebGL context (WebGL1 + LEGACY_GL_EMULATION).
 *
 * Not implemented: pbuffers, GLX_SGIX_*, indirect rendering, FBConfig query
 * surface beyond glXChooseFBConfig. Add as clients demand them.
 */

#ifndef _GLX_H_
#define _GLX_H_

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <GL/gl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- types --------------------------------------------------------------- */

typedef struct __GLXcontextRec   *GLXContext;
typedef struct __GLXFBConfigRec  *GLXFBConfig;
typedef XID GLXDrawable;
typedef XID GLXPixmap;
typedef XID GLXWindow;
typedef XID GLXPbuffer;
typedef XID GLXContextID;

/* --- attribute tokens (glXChooseVisual / glXChooseFBConfig) --------------- */

#define GLX_USE_GL              1
#define GLX_BUFFER_SIZE         2
#define GLX_LEVEL               3
#define GLX_RGBA                4
#define GLX_DOUBLEBUFFER        5
#define GLX_STEREO              6
#define GLX_AUX_BUFFERS         7
#define GLX_RED_SIZE            8
#define GLX_GREEN_SIZE          9
#define GLX_BLUE_SIZE           10
#define GLX_ALPHA_SIZE          11
#define GLX_DEPTH_SIZE          12
#define GLX_STENCIL_SIZE        13
#define GLX_ACCUM_RED_SIZE      14
#define GLX_ACCUM_GREEN_SIZE    15
#define GLX_ACCUM_BLUE_SIZE     16
#define GLX_ACCUM_ALPHA_SIZE    17

/* GLX 1.3 FBConfig tokens (subset). */
#define GLX_X_VISUAL_TYPE       0x22
#define GLX_TRUE_COLOR          0x8002
#define GLX_DIRECT_COLOR        0x8003
#define GLX_DRAWABLE_TYPE       0x8010
#define GLX_RENDER_TYPE         0x8011
#define GLX_X_RENDERABLE        0x8012
#define GLX_FBCONFIG_ID         0x8013
#define GLX_RGBA_TYPE           0x8014
#define GLX_COLOR_INDEX_TYPE    0x8015
#define GLX_RGBA_BIT            0x00000001
#define GLX_WINDOW_BIT          0x00000001
#define GLX_PIXMAP_BIT          0x00000002
#define GLX_PBUFFER_BIT         0x00000004
#define GLX_VISUAL_ID           0x800B

/* glXQueryExtensionsString / glXGetClientString tokens. */
#define GLX_VENDOR              0x1
#define GLX_VERSION             0x2
#define GLX_EXTENSIONS          0x3

/* glXGetConfig error returns. */
#define GLX_BAD_SCREEN          1
#define GLX_BAD_ATTRIBUTE       2
#define GLX_NO_EXTENSION        3
#define GLX_BAD_VISUAL          4
#define GLX_BAD_CONTEXT         5
#define GLX_BAD_VALUE           6
#define GLX_BAD_ENUM            7

/* --- functions ----------------------------------------------------------- */

extern Bool         glXQueryExtension(Display *dpy, int *errorBase, int *eventBase);
extern Bool         glXQueryVersion(Display *dpy, int *major, int *minor);
extern const char  *glXQueryExtensionsString(Display *dpy, int screen);
extern const char  *glXQueryServerString(Display *dpy, int screen, int name);
extern const char  *glXGetClientString(Display *dpy, int name);

extern XVisualInfo *glXChooseVisual(Display *dpy, int screen, int *attribList);
extern int          glXGetConfig(Display *dpy, XVisualInfo *vis, int attrib, int *value);

extern GLXContext   glXCreateContext(Display *dpy, XVisualInfo *vis,
                                     GLXContext shareList, Bool direct);
extern void         glXDestroyContext(Display *dpy, GLXContext ctx);
extern Bool         glXMakeCurrent(Display *dpy, GLXDrawable drawable, GLXContext ctx);
extern Bool         glXMakeContextCurrent(Display *dpy, GLXDrawable draw,
                                          GLXDrawable read, GLXContext ctx);
extern GLXContext   glXGetCurrentContext(void);
extern GLXDrawable  glXGetCurrentDrawable(void);
extern Display     *glXGetCurrentDisplay(void);
extern Bool         glXIsDirect(Display *dpy, GLXContext ctx);

extern void         glXSwapBuffers(Display *dpy, GLXDrawable drawable);
extern void         glXWaitGL(void);
extern void         glXWaitX(void);

extern GLXFBConfig *glXChooseFBConfig(Display *dpy, int screen,
                                      const int *attribList, int *nitems);
extern XVisualInfo *glXGetVisualFromFBConfig(Display *dpy, GLXFBConfig config);
extern int          glXGetFBConfigAttrib(Display *dpy, GLXFBConfig config,
                                         int attribute, int *value);

extern void       (*glXGetProcAddress(const GLubyte *procName))(void);
extern void       (*glXGetProcAddressARB(const GLubyte *procName))(void);

#ifdef __cplusplus
}
#endif

#endif /* _GLX_H_ */
