/*
 * Stub <X11/ICE/ICElib.h> for em-x11.
 *
 * ICE is the Inter-Client Exchange protocol underneath SM. Xt's Shell.c
 * includes it unconditionally but only uses ICE symbols inside
 * #ifndef XT_NO_SM, so an empty opaque stub is enough to compile.
 */
#ifndef EMX11_STUB_ICELIB_H
#define EMX11_STUB_ICELIB_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void *IceConn;

typedef enum {
    IceProcessMessagesSuccess = 0,
    IceProcessMessagesIOError,
    IceProcessMessagesConnectionClosed
} IceProcessMessagesStatus;

#ifdef __cplusplus
}
#endif

#endif /* EMX11_STUB_ICELIB_H */
