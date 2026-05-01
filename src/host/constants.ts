/**
 * Constants shared across host/* manager modules.
 *
 * X protocol numerics are kept here when more than one manager needs
 * them (XID layout, well-known XIDs, event-mask bits we route on).
 * Property mode codes / AnyPropertyType stay in host/property.ts; pixmap
 * caps stay in host/gc.ts. The split rule: extract here only when
 * inlining the literal would create a "what does 0x00100000 mean?"
 * mystery in two different files.
 */

/* X11 event-type numerics we send to the C side via emx11_push_*_event. */
export const X_ButtonPress = 4;
export const X_ButtonRelease = 5;
export const X_KeyPress = 2;
export const X_KeyRelease = 3;

/* X11 event-mask bits we act on directly (x11protocol.txt §847).
 * These are the literal mask values clients pass to XSelectInput. */
export const SubstructureRedirectMask = 1 << 20;

/* XID layout: top 3 bits always 0 (x11protocol.txt §935). We dedicate
 * the low 0x00200000 slot (conn_id=0, 2 M IDs) to Host-owned resources
 * -- root window, default cursor, the weave pixmap -- so no client can
 * ever forge one. Real connections start at conn_id=1. */
export const XID_RANGE_BITS = 21;
export const XID_PER_CONN = 1 << XID_RANGE_BITS;         // 0x00200000
export const XID_MASK = XID_PER_CONN - 1;                // 0x001FFFFF

/* Well-known XIDs for Host-owned resources. Clients learn the root
 * window's XID through the get_root_window bridge during XOpenDisplay
 * and put a local shadow entry in their EmxWindow table pointing at
 * it -- the Host keeps the authoritative renderer record. */
export const HOST_ROOT_ID = 0x00000001;
export const HOST_WEAVE_PIXMAP_ID = 0x00000002;

/* Maximum depth we walk up the parent chain when resolving a window's
 * absolute origin or building a clip stack. X has no formal depth limit,
 * but a real session never nests deeper than a handful of levels (root
 * -> WM frame -> shell -> composite -> leaf). The cap is purely a guard
 * against accidentally cyclic parent links. */
export const MAX_PARENT_WALK = 32;
