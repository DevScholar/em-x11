/*
 * Minimal hand-rolled config.h for libXt inside em-x11.
 *
 * Upstream libXt is autotools-based and generates config.h from
 * configure.ac. We are building against Emscripten in a fixed, known
 * environment (single-threaded, no fd polling, musl-based libc), so the
 * relevant knobs are trivial to set by hand.
 */
#ifndef EMX11_LIBXT_CONFIG_H
#define EMX11_LIBXT_CONFIG_H

#define PACKAGE_NAME    "libXt"
#define PACKAGE_STRING  "libXt 1.3.1 (em-x11)"
#define PACKAGE_VERSION "1.3.1"
#define VERSION         "1.3.1"

/* No threads: browser main thread is the only execution context, and
 * Xt's own mutex helpers expand to no-ops when XTHREADS is undefined. */
/* #undef XTHREADS */

/* No fds to poll in a browser; fall back to the "everything is ready"
 * code paths that match our XNextEvent blocking-on-queue model. */
/* #undef USE_POLL */

/* musl 1.2.2+ and modern glibc expose reallocarray, but emscripten's
 * libc does not surface it via default headers. Let Xt use its manual
 * overflow check + realloc fallback in Alloc.c. */
/* #undef HAVE_REALLOCARRAY */

/* Xt uses these locale-ish helpers; emscripten libc has them. */
#define HAVE_STRCASECMP 1
#define HAVE_STRNCASECMP 1

/* _XtRegExp path in TMparse.c -- we keep it disabled; no regcomp
 * needed for the default Xt translation tables. */
/* #undef HAVE_REGEX_H */

/* Sanity: sizeof(long) == 4 under wasm32. */
#define SIZEOF_LONG 4
#define SIZEOF_INT  4

#endif /* EMX11_LIBXT_CONFIG_H */
