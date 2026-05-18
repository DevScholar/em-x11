/*
 * XKB extension -- minimal slice.
 *
 * Tk 8.6 (when configured with --enable-xkb / HAVE_XKBKEYCODETOKEYSYM)
 * calls into XKB through exactly two entry points:
 *
 *   XkbOpenDisplay     -- wraps XOpenDisplay + extension check. If this
 *                         returns NULL Tk falls back to plain XOpenDisplay
 *                         and skips the XKB branch entirely. We make it
 *                         always succeed so Tk takes the XKB branch.
 *
 *   XkbKeycodeToKeysym -- like XKeycodeToKeysym but with (group, level).
 *                         Our keymap is browser-driven (runtime/keymap.ts
 *                         resolves the platform keyboard layout before
 *                         events ever reach C) so group/level are already
 *                         baked into the keysym sitting in
 *                         dpy->keysym_table. Delegate to the existing
 *                         lookup and ignore the indices.
 *
 * The other entry points (XkbQueryExtension, XkbUseExtension, etc.) are
 * here for Xt/Xaw/future Motif feature-detection; they all advertise
 * XKB 1.0 available.
 *
 * Reference: libX11-1.8.13/src/xkb/. The real client-side library is
 * thousands of lines of state-machine code reconstructing the server's
 * keymap; none of that is meaningful when the keymap originates in the
 * browser. We replace it with a stateless adapter.
 */

#include "emx11_internal.h"
#include <X11/XKBlib.h>

/* Defined in event_keysym.c -- the same table backs XKeycodeToKeysym. */
extern KeySym XKeycodeToKeysym(Display *dpy, unsigned int keycode, int index);

Display *XkbOpenDisplay(const char *name,
                        int *ev_rtrn, int *err_rtrn,
                        int *major_rtrn, int *minor_rtrn, int *reason) {
    Display *dpy = XOpenDisplay(name);
    if (!dpy) {
        if (reason) *reason = XkbOD_ConnectionRefused;
        return NULL;
    }
    /* Negotiate version: real Xlib treats major/minor as in/out and
     * returns BadServerVersion if the server is older than the client
     * requests. Browser keymap has no version skew -- always succeed
     * at 1.0 (XkbMajorVersion / XkbMinorVersion). */
    if (major_rtrn) *major_rtrn = XkbMajorVersion;
    if (minor_rtrn) *minor_rtrn = XkbMinorVersion;
    /* We don't allocate XKB event / error codes; report 0 so callers
     * that compare event->type against (ev_rtrn + XkbAnyEvent) simply
     * never match -- there are no XKB events to emit. */
    if (ev_rtrn)  *ev_rtrn  = 0;
    if (err_rtrn) *err_rtrn = 0;
    if (reason)   *reason   = XkbOD_Success;
    return dpy;
}

Bool XkbQueryExtension(Display *dpy,
                       int *opcodeReturn, int *eventBaseReturn,
                       int *errorBaseReturn,
                       int *majorRtrn, int *minorRtrn) {
    (void)dpy;
    if (opcodeReturn)     *opcodeReturn     = 0;
    if (eventBaseReturn)  *eventBaseReturn  = 0;
    if (errorBaseReturn)  *errorBaseReturn  = 0;
    if (majorRtrn)        *majorRtrn        = XkbMajorVersion;
    if (minorRtrn)        *minorRtrn        = XkbMinorVersion;
    return True;
}

Bool XkbUseExtension(Display *dpy,
                     int *major_rtrn, int *minor_rtrn) {
    (void)dpy;
    if (major_rtrn) *major_rtrn = XkbMajorVersion;
    if (minor_rtrn) *minor_rtrn = XkbMinorVersion;
    return True;
}

Bool XkbLibraryVersion(int *libMajorRtrn, int *libMinorRtrn) {
    if (libMajorRtrn) *libMajorRtrn = XkbMajorVersion;
    if (libMinorRtrn) *libMinorRtrn = XkbMinorVersion;
    return True;
}

KeySym XkbKeycodeToKeysym(Display *dpy, unsigned int kc,
                          int group, int level) {
    (void)group;
    (void)level;
    /* See header comment: the browser-side keymap pre-resolves group /
     * level into the keysym that lands in dpy->keysym_table. Group 0
     * level 0 is the only meaningful index.
     *
     * Suppress the upstream deprecation attribute on XKeycodeToKeysym
     * here -- we're explicitly delegating one legacy keysym lookup to
     * another, and the keysym_table-backed implementation is the right
     * destination regardless of upstream's policy. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    return XKeycodeToKeysym(dpy, kc, 0);
#pragma GCC diagnostic pop
}

Bool XkbSetDetectableAutoRepeat(Display *dpy, Bool detectable,
                                Bool *supported) {
    (void)dpy;
    (void)detectable;
    /* em-x11 already emits autorepeat as a stream of KeyPress events
     * with no intervening KeyRelease (browser keydown repeat semantics),
     * which IS detectable-autorepeat behaviour. Report it as supported
     * regardless of the requested value. */
    if (supported) *supported = True;
    return True;
}
