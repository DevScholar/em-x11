/*
 * Error handler stubs.
 */

#include "emx11_internal.h"

#include <stdio.h>
#include <string.h>

static XErrorHandler g_error_handler = NULL;

XErrorHandler XSetErrorHandler(XErrorHandler handler) {
    XErrorHandler prev = g_error_handler;
    g_error_handler = handler;
    return prev;
}

int XGetErrorText(Display *dpy, int code, char *buffer_return, int length) {
    (void)dpy;
    if (!buffer_return || length <= 0) return 0;
    snprintf(buffer_return, (size_t)length, "X error %d", code);
    return 0;
}

int XGetErrorDatabaseText(Display *dpy, _Xconst char *name, _Xconst char *message,
                          _Xconst char *default_string, char *buffer_return, int length) {
    (void)dpy; (void)name; (void)message;
    if (!buffer_return || length <= 0) return 0;
    const char *src = default_string ? default_string : "";
    size_t n = strlen(src);
    if (n >= (size_t)length) n = (size_t)length - 1;
    memcpy(buffer_return, src, n);
    buffer_return[n] = '\0';
    return 0;
}
