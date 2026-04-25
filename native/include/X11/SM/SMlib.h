/*
 * Stub <X11/SM/SMlib.h> for em-x11.
 *
 * Upstream libSM implements X11's Session Management Protocol, which
 * lets X clients save/restore state across logout. In a browser there
 * is no session manager and no reason to implement the protocol. Xt's
 * Shell.c is already guarded with XT_NO_SM, so we just need the
 * opaque types visible to Shell.h / ShellP.h so they compile.
 */
#ifndef EMX11_STUB_SMLIB_H
#define EMX11_STUB_SMLIB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void *SmcConn;
typedef void *SmsConn;
typedef void *IceConn;

/* SmRestartStyle values -- Xt Converters.c maps RestartIfRunning etc.
 * to these codes even when the session-management path is compiled out. */
#define SmRestartIfRunning   0
#define SmRestartAnyway      1
#define SmRestartImmediately 2
#define SmRestartNever       3

typedef struct {
    int    length;
    void  *value;
} SmPropValue;

typedef struct {
    char        *name;
    char        *type;
    int          num_vals;
    SmPropValue *vals;
} SmProp;

#ifdef __cplusplus
}
#endif

#endif /* EMX11_STUB_SMLIB_H */
