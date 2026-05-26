/* em-x11 stub for <X11/SM/SMlib.h>.
 *
 * X Session Management doesn't exist in the browser. session.c (the
 * upstream twm translation unit that actually calls SM_* functions) is
 * excluded from the build; session_stub.c replaces it. But session.h
 * -- which is included from twm.c, add_window.c and menus.c -- pulls
 * this header in unconditionally. The only SM symbol referenced in any
 * header is the opaque SmcConn pointer type, which we provide below.
 *
 * If a future compile error names a missing ICElib / SMlib type, add
 * the forward declaration here rather than stubbing the whole library. */

#ifndef EMX11_STUB_SMLIB_H
#define EMX11_STUB_SMLIB_H

typedef struct _SmcConn *SmcConn;

/* menus.c calls SmcCloseConnection on its F_RESTART path, guarded by
 * `if (smcConn)`. smcConn is always NULL in our build, so the call is
 * dead at runtime -- but the compiler still needs a declaration. */
extern int SmcCloseConnection(SmcConn smcConn, int count, void *props);

#endif
