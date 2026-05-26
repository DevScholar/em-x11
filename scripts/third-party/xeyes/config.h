/* em-x11 config.h for xeyes 1.3.1 (overlaid by scripts/fetch-third-party.sh).
 *
 * Upstream xeyes is autotools-driven; this replaces the autoheader-produced
 * config.h. We enable nothing optional: no XRender (Eyes.c's XRender path
 * wants full Picture/XRenderFindFormat machinery), no PRESENT (which drags
 * in XCB + xfixes + damage + present). That leaves xeyes on the pure-Xlib
 * polling path, which is the subset em-x11 actually implements. */

#ifndef EMX11_XEYES_CONFIG_H
#define EMX11_XEYES_CONFIG_H

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

#define PACKAGE                    "xeyes"
#define PACKAGE_NAME               "xeyes"
#define PACKAGE_TARNAME            "xeyes"
#define PACKAGE_VERSION            "1.3.1"
#define PACKAGE_VERSION_MAJOR      1
#define PACKAGE_VERSION_MINOR      3
#define PACKAGE_VERSION_PATCHLEVEL 1
#define PACKAGE_STRING             "xeyes 1.3.1"
#define PACKAGE_BUGREPORT          "https://gitlab.freedesktop.org/xorg/app/xeyes/issues"
#define PACKAGE_URL                ""
#define VERSION                    "1.3.1"

/* Intentionally left undefined: XRENDER, PRESENT. */

#endif
