/* em-x11 stub for <X11/extensions/sync.h>.
 *
 * X Sync extension: used by twm to set per-window scheduling priorities
 * (XSyncSetPriority / XSyncGetPriority) and to query whether the server
 * supports the extension. In the browser we have one thread and no
 * scheduler to influence, so XSyncQueryExtension returns False and the
 * setters/getters are no-ops.
 *
 * Matches the public signatures from libXext's upstream header, but
 * stripped to the three functions em-x11 actually stubs. Add more here
 * when a new client demands them. */

#ifndef EMX11_STUB_X11_EXT_SYNC_H
#define EMX11_STUB_X11_EXT_SYNC_H

#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif

extern Status XSyncQueryExtension(Display *dpy,
                                  int *event_base_return,
                                  int *error_base_return);

extern int XSyncSetPriority(Display *dpy, XID client_resource_id, int priority);
extern int XSyncGetPriority(Display *dpy, XID client_resource_id,
                            int *return_priority);

#ifdef __cplusplus
}
#endif

#endif
