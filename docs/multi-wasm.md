# Multi-wasm architecture for em-x11

Status: design, pre-implementation. Phase 0 (twm links + boots alone) is done; this
doc fixes the client/server split, state ownership, and the message paths
before we start wiring multiple clients together.

Treat this as a contract: changes that contradict it need to update the doc
first. Section numbers in the X11 protocol reference point at
`references/x11-docs/x11protocol.txt`.

## 1. Principles

- **JS Host = X server.** Every piece of state that real X keeps on the
  server side lives in the Host. One source of truth.
- **Each wasm Module = one X client.** C-side state is only what real Xlib
  caches client-side (GC value structs, loaded font table, the client's
  local event queue, pending request buffer — though we have no wire
  buffer, so just the queue).
- **`emx11_js_*` bridges = X wire protocol.** Synchronous JS calls with
  optional return values stand in for the request/reply/event wire
  format. Async events get pushed back via `ccall('emx11_push_*_event')`
  on the target Module.

## 2. Connection lifecycle

Follows the X11 wire protocol connection setup (§8285, "Successful
response"). Each client gets a private XID range it can allocate out
of without round-tripping the Host.

1. C-side `XOpenDisplay` calls new bridge `emx11_js_open_display()`.
2. Host allocates a fresh `connection_id` (monotonic counter) and stores
   `{connection_id → Module reference}` in its connection table.
3. Host grants the client an XID range via `(resource_id_base,
   resource_id_mask)`. We use the same layout real X uses:
   `base = connection_id << 21`, `mask = 0x1FFFFF`, leaving 2 M resource
   IDs per connection and the top three bits zero (§935). Returned to C
   through the bridge's output params.
4. C-side `resource_alloc` becomes `base | (++counter & mask)`.
5. `XCloseDisplay` calls `emx11_js_close_display(conn_id)`. Host drops
   every window / pixmap / atom reference owned by that connection.

## 3. State ownership

**Host owns** (one copy for the whole display):

| State                            | Keyed by               |
|----------------------------------|------------------------|
| Window tree                      | XID                    |
| Window → owning `connection_id`  | XID                    |
| Window event subscriptions       | XID, `connection_id`   |
| Substructure-redirect holder     | XID (at most one)      |
| Pixmap registry                  | XID                    |
| Atom table                       | name string            |
| Property store                   | (XID, atom)            |
| Cursor registry                  | XID                    |
| Compositor / framebuffer         | (global, already here) |

**Each C client owns** (per-Module, private memory):

| State                       | Notes                                    |
|-----------------------------|------------------------------------------|
| Open GC structs             | Value-copies; draw calls read fg/bg now  |
| Loaded-font table           | font id → CSS font string                |
| Keymap (keysym → keycode)   | Local synthesis table                    |
| Incoming event queue        | Ring buffer Host pushes into             |
| XID counter (next free)     | Inside this connection's range           |

Nothing X-semantics-critical is duplicated across clients.

## 4. Protocol surface (bridge groups)

Each existing bridge gains a leading `conn_id` parameter. Most drawing
bridges (`emx11_js_fill_rect`, etc.) don't need one — the target XID is
globally resolvable and the Host already knows which connection owns it.

New bridges:

- Connection: `open_display() → (conn_id, xid_base, xid_mask)`,
  `close_display(conn_id)`
- Atoms: `intern_atom(conn_id, name, only_if_exists) → atom_id`,
  `get_atom_name(atom_id) → string`
- Properties: `change_property(conn_id, win, atom, type, format, data,
  mode)`, `get_property(conn_id, win, atom, offset, length, delete) →
  (type, format, bytes)`, `delete_property(conn_id, win, atom)`
- Event subscription: `select_input(conn_id, win, event_mask)` — Host
  enforces the "only one client per SubstructureRedirect/ResizeRedirect/
  ButtonPress" rule (§1477).
- Query: `get_window_attrs(win) → {x, y, w, h, mapped, override_redirect,
  event_mask_of_caller, all_event_masks}`

## 5. Event routing

Events originate from two sources, both land on Host first:

- **Browser** (mouse, key) — Host does hit-test, finds target window,
  looks up owning `conn_id`, pushes event into that connection's C-side
  queue via `ccall('emx11_push_*_event', ...)` on the right Module.
- **X semantics** (MapRequest, ConfigureRequest, PropertyNotify, etc.)
  — Host synthesizes and pushes to the correct subscriber's queue(s).

The Host keeps `conn_id → Module` so it can ccall into the right one.
When several clients subscribe to the same event type on the same
window (e.g. two clients with PropertyChangeMask), Host pushes a
separate copy into each queue — matching real X (§1476, "reported to
all interested clients").

## 6. The two XMapWindow flows

### 6.1. Normal map (no redirect holder, or caller is the holder)

```
client C            Host (JS)                        compositor
-----------------   ----------------------------     ----------
XMapWindow(w) ─bridge→ js_window_map(conn_id, w)
                    │ find win in window table
                    │ parent = win.parent
                    │ no SubstructureRedirect on     │
                    │   parent, OR caller IS holder  │
                    │
                    ├─ win.mapped = true
                    ├─ push MapNotify → conn_id's    │
                    │   subscribers of StructureNotify/
                    │   SubstructureNotify
                    ├─ push Expose → owning conn_id
                    └─ compositor.markDirty() ────────→ repaints
```

### 6.2. Redirected map (§1592: "MapRequest is generated, window remains unmapped")

```
xeyes C                         Host (JS)                        twm C
-------------                   ----------------------------     -----
XMapWindow(w) ───bridge────→    js_window_map(xeyes_id, w)
                                │ look up win
                                │ parent = root
                                │ root has SubstructureRedirect
                                │   held by twm_id, AND
                                │   win.override_redirect == False
                                │
                                ├─ do NOT change win.mapped
                                ├─ synthesize MapRequest event:
                                │   .parent = root
                                │   .window = w
                                └─ push to twm's queue ─────→    XNextEvent returns
                                                                  MapRequest. twm:
                                                                    - builds frame
                                                                    - XReparentWindow
                                                                    - XMapWindow(frame)
                                                                    - XMapWindow(w)
                                                                      ↑ still redirect-
                                                                      checked, but now
                                                                      caller IS holder,
                                                                      so the normal
                                                                      flow in 6.1 runs
```

Same two-branch structure applies to `XConfigureWindow` (§1667,
"ConfigureRequest is generated") and the three `XCirculate*` calls
(CirculateRequest).

## 7. Redirect rules (checklist)

- **At most one holder per window.** `select_input` with
  SubstructureRedirectMask while someone else holds it → Access error.
- **Override-redirect bypass.** A window with `override_redirect=True`
  is never redirected (§1419, §1592). twm relies on this for its
  menus / icon manager.
- **Caller == holder bypass.** When twm itself calls XMapWindow (on a
  frame it just built), redirect is skipped so twm doesn't get stuck
  requesting permission from itself. Track this: the holder's own
  map/configure goes through the normal flow.
- **No holder → normal flow**, even if the parent has SubstructureNotify
  subscribers (those just get MapNotify after the fact).

## 8. Atom and property rule

Atoms and properties are Host-global. Consequences:

- Each client's `XInternAtom("WM_PROTOCOLS", False)` returns the same
  numeric value.
- `XChangeProperty(root, RESOURCE_MANAGER, ...)` from twm is immediately
  visible to any client's subsequent `XGetWindowProperty`.
- PropertyNotify goes to every client that selected PropertyChangeMask
  on that window.
- C side keeps no local atom cache. Every `XInternAtom` is a bridge
  call. (Performance: cheap — it's a JS-side map lookup.)

## 9. Bootstrap convention

Convention, not mechanism. Host is always ready; connection order is
a UX concern only.

1. Host constructs on page load.
2. twm wasm loads, opens display, subscribes
   `SubstructureRedirectMask | SubstructureNotifyMask` on root.
3. Client wasms load afterward. Their XMapWindow calls get redirected
   to twm.

If a client loads before twm, it maps unmanaged — same as starting an
X app before the window manager in a real session. No error path,
just a visual mismatch the user resolves by reloading or by the client
supporting re-management (ICCCM-wise this is fine).

## 10. Deferred (not Phase 2)

- Real grab semantics with pointer confinement — stubbed GrabSuccess today
- ICCCM selections (CLIPBOARD / PRIMARY beyond cut buffers)
- Input focus dispatch (ClientMessage WM_TAKE_FOCUS round-trips)
- Shared memory extensions (MIT-SHM) — not useful in-browser
- PseudoColor / indexed-color colormaps — TrueColor only
- Multi-screen / XRandR — one fixed 1024×768 screen for the foreseeable
