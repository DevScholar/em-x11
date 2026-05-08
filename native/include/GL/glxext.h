/*
 * em-x11 glxext.h -- minimal subset of the GLX extension tokens that
 * common GLX clients reference at compile time but rarely actually
 * exercise. Real ext function pointers come from glXGetProcAddress(),
 * which em-x11's glx.c returns NULL for; clients are expected to test
 * the returned pointer before calling.
 */

#ifndef _GLX_GLXEXT_H_
#define _GLX_GLXEXT_H_

#include <GL/glx.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GLX_EXT_framebuffer_sRGB */
#define GLX_FRAMEBUFFER_SRGB_CAPABLE_EXT     0x20B2

/* GLX_ARB_multisample */
#define GLX_SAMPLE_BUFFERS                   100000
#define GLX_SAMPLES                          100001
#define GLX_SAMPLE_BUFFERS_ARB               100000
#define GLX_SAMPLES_ARB                      100001

/* GLX_EXT_swap_control / GLX_MESA_swap_control / GLX_SGI_swap_control:
 * function pointers retrieved via glXGetProcAddressARB. em-x11's
 * glx.c returns NULL for these probes so clients fall back. */
typedef void (*PFNGLXSWAPINTERVALEXTPROC)(Display *dpy, GLXDrawable drawable, int interval);
typedef int  (*PFNGLXSWAPINTERVALMESAPROC)(unsigned int interval);
typedef int  (*PFNGLXGETSWAPINTERVALMESAPROC)(void);
typedef int  (*PFNGLXSWAPINTERVALSGIPROC)(int interval);

/* GLX_ARB_get_proc_address */
typedef void (*__GLXextFuncPtr)(void);

#ifdef __cplusplus
}
#endif

#endif /* _GLX_GLXEXT_H_ */
