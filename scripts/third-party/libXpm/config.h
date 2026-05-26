/*
 * Hand-rolled config.h for libXpm inside em-x11. Same template as
 * libXaw / libXmu.
 */
#ifndef EMX11_LIBXPM_CONFIG_H
#define EMX11_LIBXPM_CONFIG_H

#define PACKAGE         "libXpm"
#define PACKAGE_NAME    "libXpm"
#define PACKAGE_STRING  "libXpm 3.5.17 (em-x11)"
#define PACKAGE_VERSION "3.5.17"
#define VERSION         "3.5.17"
#define PACKAGE_VERSION_MAJOR 3
#define PACKAGE_VERSION_MINOR 5

#define HAVE_STDIO_H     1
#define HAVE_STDLIB_H    1
#define HAVE_STRING_H    1
#define HAVE_STRINGS_H   1
#define HAVE_STDINT_H    1
#define HAVE_INTTYPES_H  1
#define HAVE_SYS_STAT_H  1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H    1

/* No pipes / subprocesses in a browser — libXpm's compressed-reader
 * path shells out to gzip/compress, which would break under wasm. */
#define NO_ZPIPE 1

/* Emscripten musl does not ship strlcat. libXpm has a fallback in
 * simx.c / misc.c that kicks in when this is absent. */
/* #undef HAVE_STRLCAT */

/* No closefrom / close_range in emscripten libc. */
/* #undef HAVE_CLOSEFROM */
/* #undef HAVE_CLOSE_RANGE */

#define LOCALEDIR "/usr/share/locale"

#endif /* EMX11_LIBXPM_CONFIG_H */
