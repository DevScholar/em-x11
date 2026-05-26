/*
 * Hand-rolled config.h for libXmu inside em-x11. Mirrors libXaw's
 * config; see that file for the rationale.
 */
#ifndef EMX11_LIBXMU_CONFIG_H
#define EMX11_LIBXMU_CONFIG_H

#define PACKAGE         "libXmu"
#define PACKAGE_NAME    "libXmu"
#define PACKAGE_STRING  "libXmu 1.2.1 (em-x11)"
#define PACKAGE_VERSION "1.2.1"
#define VERSION         "1.2.1"

#define HAVE_STDIO_H     1
#define HAVE_STDLIB_H    1
#define HAVE_STRING_H    1
#define HAVE_STRINGS_H   1
#define HAVE_STDINT_H    1
#define HAVE_INTTYPES_H  1
#define HAVE_SYS_STAT_H  1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H    1
#define HAVE_WCHAR_H     1

/* Xmu has a reallocarray fallback it compiles when the host lacks it.
 * Emscripten musl ships reallocarray, so skip the fallback. */
#define HAVE_REALLOCARRAY 1

/* No uname in a browser; Xmu only uses it for the "DNS host name" fallback
 * in GetHost.c which we do not need. */
/* #undef HAVE_UNAME */

/* #undef HAVE_DLFCN_H */
/* #undef HAVE_MALLOC_H */
/* #undef HAVE_MALLOC_USABLE_SIZE */

#endif /* EMX11_LIBXMU_CONFIG_H */
