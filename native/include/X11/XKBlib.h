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
#include <X11/extensions/XKBstr.h>

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

/* -- Keymap allocation + retrieval. ------------------------------------ *
 *
 * Real X11 splits keymap acquisition into half a dozen entry points
 * (XkbGetMap, XkbGetUpdatedMap, XkbGetKeyboard, XkbGetKeyboardByName,
 * XkbAllocClientMap, ...) so the client can fetch incremental updates
 * and choose which subset of XkbDescRec fields to populate. We
 * implement the same external API; all variants converge on building
 * a fresh XkbDescRec from the current keysym_table, which the host
 * has already filled with navigator.keyboard.getLayoutMap() data.
 *
 * `which` bitmask honored: XkbKeySymsMask. Other bits (KeyTypes,
 * ModifierMap, ...) are silently accepted -- callers that don't ask
 * for KeySyms get an empty map (matches real Xlib's behaviour).
 */

extern XkbDescPtr XkbAllocKeyboard(void);

extern Status XkbAllocClientMap(
    XkbDescPtr xkb,
    unsigned int which,
    unsigned int nTypes);

extern void XkbFreeClientMap(
    XkbDescPtr xkb,
    unsigned int what,
    Bool freeMap);

extern void XkbFreeKeyboard(
    XkbDescPtr xkb,
    unsigned int which,
    Bool freeDesc);

extern XkbDescPtr XkbGetMap(
    Display *dpy,
    unsigned int which,
    unsigned int deviceSpec);

extern Status XkbGetUpdatedMap(
    Display *dpy,
    unsigned int which,
    XkbDescPtr desc);

extern XkbDescPtr XkbGetKeyboard(
    Display *dpy,
    unsigned int which,
    unsigned int deviceSpec);

extern XkbDescPtr XkbGetKeyboardByName(
    Display *dpy,
    unsigned int deviceSpec,
    void *names,           /* XkbComponentNamesPtr in real Xlib */
    unsigned int want,
    unsigned int need,
    Bool load);

#ifdef __cplusplus
}
#endif

#endif /* _X11_XKBLIB_H_ */
