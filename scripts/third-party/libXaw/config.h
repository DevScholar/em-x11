/*
 * Minimal hand-rolled config.h for libXaw inside em-x11.
 *
 * Upstream libXaw is autotools-based and generates this from
 * configure.ac. Our build target is Emscripten/musl with a fixed
 * feature set, so we set the knobs by hand. See the upstream
 * config.h.in for the full list.
 */
#ifndef EMX11_LIBXAW_CONFIG_H
#define EMX11_LIBXAW_CONFIG_H

#define PACKAGE         "libXaw"
#define PACKAGE_NAME    "libXaw"
#define PACKAGE_STRING  "libXaw 1.0.16 (em-x11)"
#define PACKAGE_VERSION "1.0.16"
#define PACKAGE_VERSION_MAJOR 1
#define PACKAGE_VERSION_MINOR 0
#define PACKAGE_VERSION_PATCHLEVEL 16
#define VERSION         "1.0.16"

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
#define HAVE_WCTYPE_H    1
#define HAVE_ISWALNUM    1
#define STDC_HEADERS     1

/* No dlopen in Emscripten's default build mode; Xaw guards
 * XawPixmapLoader registration with this. */
/* #undef HAVE_DLFCN_H */

/* Emscripten libc lacks malloc_usable_size; Xaw only uses it as an
 * optional fast-path for reallocarray-style sizing. */
/* #undef HAVE_MALLOC_USABLE_SIZE */
/* #undef HAVE_MALLOC_H */

/* _CONST_X_STRING makes Xaw internals use `const char *` for string
 * params. Matches how we've been compiling. */
#define _CONST_X_STRING 1

#endif /* EMX11_LIBXAW_CONFIG_H */
