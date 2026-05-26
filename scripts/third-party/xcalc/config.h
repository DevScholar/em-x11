/* em-x11 config.h for xcalc 1.1.3 (overlaid by scripts/fetch-third-party.sh).
 *
 * Upstream xcalc is autotools-driven; this replaces the autoheader-produced
 * config.h. Mirrors the xeyes recipe: stdlib headers always available,
 * no optional features claimed. */

#ifndef EMX11_XCALC_CONFIG_H
#define EMX11_XCALC_CONFIG_H

#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H   1
#define HAVE_STDIO_H    1
#define HAVE_STDLIB_H   1
#define HAVE_STRINGS_H  1
#define HAVE_STRING_H   1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H   1
#define STDC_HEADERS    1

/* Emscripten's musl-derived libc exposes strlcpy; declaring xcalc's own
 * static fallback would collide with the header declaration. */
#define HAVE_STRLCPY    1

#define PACKAGE                    "xcalc"
#define PACKAGE_NAME               "xcalc"
#define PACKAGE_TARNAME            "xcalc"
#define PACKAGE_VERSION            "1.1.3"
#define PACKAGE_VERSION_MAJOR      1
#define PACKAGE_VERSION_MINOR      1
#define PACKAGE_VERSION_PATCHLEVEL 3
#define PACKAGE_STRING             "xcalc 1.1.3"
#define PACKAGE_BUGREPORT          "https://gitlab.freedesktop.org/xorg/app/xcalc/issues"
#define PACKAGE_URL                ""
#define VERSION                    "1.1.3"

#endif
