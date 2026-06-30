/*
 * enterleave.h — public declarations for enterleave.c
 */
#ifndef EM_X11_ENTERLEAVE_H
#define EM_X11_ENTERLEAVE_H

#include <X11/Xlib.h>
#include <stdbool.h>

struct _XDisplay;

/* Sprite trace */
void em_x11_fill_sprite_trace(struct _XDisplay* dpy,
                              Window deepest_hint,
                              int rx,
                              int ry);
int em_x11_sprite_trace_good(void);
Window em_x11_sprite_trace_get(int i);
Window em_x11_sprite_win(void);

/* Crossing events */
void em_x11_do_enter_leave_events(
  struct _XDisplay* dpy, Window from, Window to, int mode, int rx, int ry);
void em_x11_update_sprite(
  struct _XDisplay* dpy, Window deepest_hint, int rx, int ry, int mode);

/* Grab-state accessors — defined in events.c, consumed by enterleave.c */
bool active_grab_active(struct _XDisplay* dpy);
unsigned int active_grab_event_mask_get(struct _XDisplay* dpy);
Window active_grab_window_get(struct _XDisplay* dpy);
bool active_grab_owner_events_get(struct _XDisplay* dpy);

/* Simple modifier state — defined in events.c */
unsigned int key_modifier_state(struct _XDisplay* dpy);

/* Tree query — exposed for DeliverGrabbedEvent dual-delivery */
int win_is_inferior_of(struct _XDisplay* dpy,
                       Window descendant,
                       Window ancestor);

#endif /* EM_X11_ENTERLEAVE_H */
