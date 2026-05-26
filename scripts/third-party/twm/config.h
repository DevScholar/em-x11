/* em-x11 config.h for twm 1.0.13.1 (overlaid by scripts/fetch-third-party.sh).
 *
 * Replaces the autoheader-produced config.h. HAVE_XRANDR is intentionally
 * undefined -- twm's xrandr monitor-tracking code would drag in libXrandr
 * which em-x11 does not provide, and we have exactly one fixed-size
 * "screen" anyway. */

#ifndef EMX11_TWM_CONFIG_H
#define EMX11_TWM_CONFIG_H

#define HAVE_INTTYPES_H  1
#define HAVE_STDINT_H    1
#define HAVE_STDIO_H     1
#define HAVE_STDLIB_H    1
#define HAVE_STRINGS_H   1
#define HAVE_STRING_H    1
#define HAVE_SYS_STAT_H  1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H    1
#define HAVE_WCHAR_H     1
#define HAVE_MKSTEMP     1
#define STDC_HEADERS     1

/* lex.c reads `char *yytext` (pointer form). Modern flex does, so tell
 * twm.c / gram.c not to assume the array form. */
#define YYTEXT_POINTER   1

#define PACKAGE                    "twm"
#define PACKAGE_NAME               "twm"
#define PACKAGE_TARNAME            "twm"
#define PACKAGE_VERSION            "1.0.13.1"
#define PACKAGE_VERSION_MAJOR      1
#define PACKAGE_VERSION_MINOR      0
#define PACKAGE_VERSION_PATCHLEVEL 13
#define PACKAGE_STRING             "twm 1.0.13.1"
#define PACKAGE_BUGREPORT          "https://gitlab.freedesktop.org/xorg/app/twm/issues"
#define PACKAGE_URL                ""
#define VERSION                    "1.0.13.1"

/* twm's Makefile.am would inject -DAPP_VERSION=... via AM_CPPFLAGS.
 * Provide it here so we don't have to thread it through CMake. */
#define APP_VERSION                "1.0.13.1"
#define DATADIR                    "/dev/null"

/* Intentionally left undefined: HAVE_XRANDR. */

#endif
