/*
 * Minimal <X11/XKBlib.h> for em-x11.
 *
 * Only declares the slice of XKB that real X11 clients (Tk 8.6, Xt,
 * Motif, future GTK1) actually link against in our wasm builds. Skip
 * keymap geometry, indicators, controls, bell, action atoms -- all the
 * machinery a full X server needs but a stateless browser keymap
 * doesn't have any equivalent for.
 *
 * Reference: libX11-1.8.13/include/X11/XKBlib.h (in references/). Match
 * the upstream signatures exactly so configure probes don't get
 * tripped up by type mismatch.
 */

#ifndef _X11_XKBLIB_H_
#define _X11_XKBLIB_H_

#include <X11/Xlib.h>
#include <X11/extensions/XKB.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Error codes returned via the `reason` out-param of XkbOpenDisplay. */
#define XkbOD_Success           0
#define XkbOD_BadLibraryVersion 1
#define XkbOD_ConnectionRefused 2
#define XkbOD_NonXkbServer      3
#define XkbOD_BadServerVersion  4

extern Display *XkbOpenDisplay(
    const char *name,
    int *ev_rtrn,
    int *err_rtrn,
    int *major_rtrn,
    int *minor_rtrn,
    int *reason);

extern Bool XkbQueryExtension(
    Display *dpy,
    int *opcodeReturn,
    int *eventBaseReturn,
    int *errorBaseReturn,
    int *majorRtrn,
    int *minorRtrn);

extern Bool XkbUseExtension(
    Display *dpy,
    int *major_rtrn,
    int *minor_rtrn);

extern Bool XkbLibraryVersion(
    int *libMajorRtrn,
    int *libMinorRtrn);

/* Match upstream NeedWidePrototypes default (kc widened to unsigned int). */
extern KeySym XkbKeycodeToKeysym(
    Display *dpy,
    unsigned int kc,
    int group,
    int level);

extern Bool XkbSetDetectableAutoRepeat(
    Display *dpy,
    Bool detectable,
    Bool *supported);

#ifdef __cplusplus
}
#endif

#endif /* _X11_XKBLIB_H_ */
