/* em-x11 config.h for blackbox 0.77 (overlaid by scripts/fetch-third-party.sh).
 *
 * SHAPE and Xft are enabled — em-x11 has full implementations of both
 * (Xext/shape.c and emx11/xft.c). MITSHM is disabled because SysV shared
 * memory (XShmAttach) has no equivalent in the browser sandbox; blackbox's
 * configure.ac guards all MIT-SHM code behind #ifdef MITSHM, so the
 * extension is simply never probed at runtime.
 *
 * NLS (gettext) is also disabled — there's no locale/mo infrastructure in
 * the browser — but gettext.h provides no-op wrappers when ENABLE_NLS is
 * undefined, so every source file still compiles. */

#ifndef EMX11_BLACKBOX_CONFIG_H
#define EMX11_BLACKBOX_CONFIG_H

/* Standard header availability */
#define HAVE_FCNTL_H      1
#define HAVE_LANGINFO_H   1
#define HAVE_LIMITS_H     1
#define HAVE_LOCALE_H     1
#define HAVE_STRING_H     1
#define HAVE_SYS_TIME_H   1
#define HAVE_UNISTD_H     1
#define HAVE_STDBOOL_H    1
#define HAVE_STDDEF_H     1
#define HAVE_STDLIB_H     1
#define HAVE_STRINGS_H    1
#define HAVE_SYS_STAT_H   1
#define HAVE_SYS_TYPES_H  1
#define HAVE_WCHAR_H      1
#define STDC_HEADERS      1

/* Function availability — Emscripten provides all of these */
#define HAVE_GETHOSTNAME  1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_MEMMOVE      1
#define HAVE_MEMSET       1
#define HAVE_MKDIR        1
#define HAVE_NL_LANGINFO  1
#define HAVE_PUTENV       1
#define HAVE_SELECT       1
#define HAVE_SETLOCALE    1
#define HAVE_SQRT         1
#define HAVE_STRCASECMP   1
#define HAVE_STRNCASECMP  1
#define HAVE_STRTOL       1
#define HAVE_STRTOUL      1
#define HAVE_STRNLEN      1
#define HAVE_MKSTEMP      1

/* Package identity */
#define PACKAGE                    "blackbox"
#define PACKAGE_NAME               "blackbox"
#define PACKAGE_TARNAME            "blackbox"
#define PACKAGE_VERSION            "0.77"
#define PACKAGE_VERSION_MAJOR      0
#define PACKAGE_VERSION_MINOR      77
#define PACKAGE_STRING             "blackbox 0.77"
#define PACKAGE_BUGREPORT          "http://github.com/bbidulock/blackboxwm/issues"
#define PACKAGE_URL                "http://github.com/bbidulock/blackboxwm"
#define VERSION                    "0.77"

/* Data directories — compiled-in fallbacks when -rc is not used.
 * In practice the session harness always passes -rc, so these are
 * dead paths, but the compiler needs them. */
#define DEFAULTMENU  "/em-x11.menu"
#define DEFAULTSTYLE "/em-x11.style"
#define LOCDIR       "/dev/null"
#define LOCALEDIR    "/dev/null"

/* Enabled X extensions — em-x11 has real implementations */
#define SHAPE   1
#define XFT     1

/* Intentionally undefined:
 *   MITSHM — SysV shared memory (XShmAttach) impossible in browser sandbox
 *   ENABLE_NLS, HAVE_LIBINTL_H — no gettext/mo infrastructure in browser
 *   DEBUG, COLORCACHE_DEBUG, FOCUS_DEBUG, FONTCACHE_DEBUG,
 *   MITSHM_DEBUG, PRINT_SIZES — no debug
 *   WITH_FULLSCREEN — not useful in fixed-size virtual screen
 *   HAVE_FORK — Emscripten has no fork() */

#endif
