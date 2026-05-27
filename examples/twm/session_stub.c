/* em-x11 replacement for twm's session.c.
 *
 * The upstream translation unit handles X Session Management via ICE /
 * SMlib, which has no analogue in the browser: no session daemon, no
 * persistent per-session state files. Every entry point is therefore a
 * no-op that satisfies the linker without pulling in ICE/SM symbols. */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "twm.h"
#include "session.h"

SmcConn smcConn = NULL;

void ConnectToSessionManager(char *previous_id, XtAppContext appContext) {
    (void)previous_id;
    (void)appContext;
}

int GetWindowConfig(TwmWindow *theWindow,
                    short *x, short *y,
                    unsigned short *width, unsigned short *height,
                    Bool *iconified, Bool *icon_info_present,
                    short *icon_x, short *icon_y,
                    Bool *width_ever_changed_by_user,
                    Bool *height_ever_changed_by_user) {
    (void)theWindow;
    (void)x; (void)y; (void)width; (void)height;
    (void)iconified; (void)icon_info_present;
    (void)icon_x; (void)icon_y;
    (void)width_ever_changed_by_user; (void)height_ever_changed_by_user;
    return 0;
}

void ReadWinConfigFile(char *filename) {
    (void)filename;
}

void DestroySession(void) {
}

int SmcCloseConnection(SmcConn smc, int count, void *props) {
    (void)smc; (void)count; (void)props;
    return 0;
}
