/**
 * Global host singleton.
 *
 * Installed on `globalThis.__EMX11__` before the first wasm module starts so
 * the C-side JS library functions (src/bindings/emx11.library.js) can reach
 * into the TS runtime without going through Emscripten's Module object.
 *
 * Since Step 2 the Host is also the "X server": each wasm client calls
 * XOpenDisplay, gets a connection id back, and from then on the Host
 * tracks which windows that connection owns so input events and (in
 * Step 3) redirected X requests route to the right wasm Module.
 */

import { RootCanvas } from './canvas.js';
import { pixelToCssColor } from './canvas.js';
import type { RootCanvasOptions } from './canvas.js';
import { Compositor, arcPath } from './compositor.js';
import { loadWasm, type LoadOptions } from '../loader/wasm.js';
import { keyEventToKeysym, modifiersFromEvent } from './keymap.js';
import type {
  EmX11Host,
  EmscriptenModule,
  Point,
  ShapeRect,
} from '../types/emscripten.js';

export type HostOptions = RootCanvasOptions;

interface Pixmap {
  canvas: OffscreenCanvas;
  ctx: OffscreenCanvasRenderingContext2D;
  width: number;
  height: number;
  depth: number;
}

/* Per-window property value. dix/property.c holds property storage
 * on the server's WindowPtr keyed by atom; we mirror that layout at
 * Host level so any client can reach it by (XID, atom). The `data`
 * field is raw bytes -- length = nitems * format/8. */
interface PropertyEntry {
  type: number;
  format: 8 | 16 | 32;
  nitems: number;
  data: Uint8Array;
}

/* X11 property modes (X.h). Matches the `mode` argument of
 * XChangeProperty verbatim. */
const PropModeReplace = 0;
const PropModePrepend = 1;
const PropModeAppend = 2;

/* X11 AnyPropertyType sentinel (Xatom.h). */
const AnyPropertyType = 0;

/* Per-connection bookkeeping. Each wasm Module that calls XOpenDisplay
 * gets one of these. Step 2 hangs owned windows and the ccall-back
 * Module reference off it; Step 3 will add substructure-redirect claims
 * and per-client event subscriptions. */
interface Connection {
  connId: number;
  xidBase: number;
  xidMask: number;
  /** Registered after launchClient loads the wasm. Null for a
   *  "headless" connection (test harness or legacy demos that open
   *  their own wasm without going through launchClient). */
  module: EmscriptenModule | null;
  /** XIDs created by this connection; kept so we can clean them up
   *  when the client disconnects. */
  ownedWindows: Set<number>;
}

/* Handoff slot used by launchClient so Host.openDisplay (called from
 * inside the wasm's main()) can find which Module instance is
 * currently booting. Because launchClient awaits serially, only one
 * launch can be pending at a time. */
interface PendingLaunch {
  connId: number;
}

const MAX_PIXMAP_EDGE = 4096;

/* XID layout: top 3 bits always 0 (x11protocol.txt §935). We dedicate
 * the low 0x00200000 slot (conn_id=0, 2 M IDs) to Host-owned resources
 * -- root window, default cursor, the weave pixmap -- so no client can
 * ever forge one. Real connections start at conn_id=1. */
const XID_RANGE_BITS = 21;
const XID_PER_CONN = 1 << XID_RANGE_BITS;         // 0x00200000
const XID_MASK = XID_PER_CONN - 1;                // 0x001FFFFF

/* Well-known XIDs for Host-owned resources. Clients learn the root
 * window's XID through the get_root_window bridge during XOpenDisplay
 * and put a local shadow entry in their EmxWindow table pointing at
 * it -- the Host keeps the authoritative compositor record. */
const HOST_ROOT_ID = 0x00000001;
const HOST_WEAVE_PIXMAP_ID = 0x00000002;

/* X11 event-mask bits we act on directly (x11protocol.txt §847).
 * These are the literal mask values clients pass to XSelectInput. */
const SubstructureRedirectMask = 1 << 20;
// const SubstructureNotifyMask  = 1 << 19;   // TODO: dispatch in later step

/* X event-type numerics we send to the C side via emx11_push_*_event. */
const X_ButtonPress = 4;
const X_ButtonRelease = 5;
const X_KeyPress = 2;
const X_KeyRelease = 3;

export class Host implements EmX11Host {
  readonly canvas: RootCanvas;
  readonly compositor: Compositor;
  private pointerX: number;
  private pointerY: number;
  private readonly pixmaps = new Map<number, Pixmap>();
  private readonly connections = new Map<number, Connection>();
  private readonly windowToConn = new Map<number, number>();
  /** Per-window event-mask subscriptions from XSelectInput, keyed by
   *  the subscribing connection. Host consults this for substructure
   *  redirect / notify routing. Empty inner map means "no subscribers". */
  private readonly windowSubscriptions = new Map<number, Map<number, number>>();
  /** override_redirect flag per window (CWOverrideRedirect). True means
   *  "WM stays out" -- Host skips the redirect path for this window. */
  private readonly overrideRedirect = new Map<number, boolean>();
  /** Window properties, keyed by (XID, atom). Per dix/property.c,
   *  property storage hangs off the server's WindowPtr and is keyed by
   *  XID + atom -- independent of which connection set it. Moving the
   *  store here from per-connection C tables is what lets a WM read the
   *  managed client's WM_NAME / WM_HINTS / WM_PROTOCOLS at all.
   *
   *  Caveat: atom IDs themselves are still per-connection in C (each
   *  wasm module has its own atom counter). For predefined atoms
   *  (XA_WM_NAME=39, XA_WM_CLASS=67, etc.) the values agree trivially.
   *  For custom atoms like WM_PROTOCOLS / WM_DELETE_WINDOW the numbers
   *  can diverge between connections and queries will miss. That's a
   *  separate step (move the atom table to Host too). */
  private readonly properties = new Map<number, Map<number, PropertyEntry>>();
  private nextConnId = 0;
  private pendingLaunch: PendingLaunch | null = null;
  /** Last window that received a ButtonPress, by connection id. Key
   *  events route here until another ButtonPress moves focus. */
  private focusedWindow: number | null = null;

  constructor(options: HostOptions = {}) {
    this.canvas = new RootCanvas(options);
    this.compositor = new Compositor(this.canvas, (id) => {
      const pm = this.pixmaps.get(id);
      return pm ? pm.canvas : null;
    });

    /* Default the pointer to the canvas centre so the first XQueryPointer
     * after XtRealizeWidget -- which fires before the user has had a
     * chance to move the mouse -- returns something sensible instead of
     * the top-left corner. xeyes in particular snaps its pupils here
     * immediately. */
    this.pointerX = (this.canvas.element.clientWidth / 2) | 0;
    this.pointerY = (this.canvas.element.clientHeight / 2) | 0;

    /* Track the last-seen pointer position at the host level so polling
     * callers (XQueryPointer; xeyes uses this every 50ms via an Xt timer)
     * can read it without going through the event bridge's hit test.
     * We listen on `window` rather than the canvas so mouse motion
     * outside the canvas (over browser chrome, over another page region)
     * still updates the cached position -- pupils that track the mouse
     * off-canvas look better than pupils that freeze on exit. */
    window.addEventListener('mousemove', (e) => {
      const rect = this.canvas.element.getBoundingClientRect();
      this.pointerX = (e.clientX - rect.left) | 0;
      this.pointerY = (e.clientY - rect.top) | 0;
    });

    this.attachInputBridge();
    this.installSharedRoot();
  }

  /** Create the single root window at Host-owned XID `HOST_ROOT_ID` and
   *  attach the classic X weave as its background_pixmap. Called once
   *  from the constructor; every client's XOpenDisplay then hands back
   *  this same XID rather than minting a per-connection root (which
   *  caused background clicks to hit the last-loaded wasm's root in
   *  Step 2). The compositor's flat window list still works because
   *  this root inserts first and stays at the bottom of z-order. */
  private installSharedRoot(): void {
    const w = this.canvas.cssWidth;
    const h = this.canvas.cssHeight;
    this.compositor.addWindow(HOST_ROOT_ID, 0, 0, 0, w, h, 0xFFFFFF);
    this.compositor.mapWindow(HOST_ROOT_ID);
    this.windowToConn.set(HOST_ROOT_ID, 0);            // conn_id=0 = Host

    /* Weave: 2×2 OffscreenCanvas, pure black + pure white on the
     * diagonal. Historically this dithered to gray on CRTs at the
     * period's DPI; on HiDPI displays you see the checker clearly,
     * which is authentic to the era. */
    const weave = new OffscreenCanvas(2, 2);
    const wctx = weave.getContext('2d');
    if (wctx) {
      wctx.fillStyle = '#000';
      wctx.fillRect(0, 0, 2, 2);
      wctx.fillStyle = '#FFF';
      wctx.fillRect(0, 0, 1, 1);
      wctx.fillRect(1, 1, 1, 1);
    }
    this.pixmaps.set(HOST_WEAVE_PIXMAP_ID, {
      canvas: weave,
      ctx: wctx!,
      width: 2,
      height: 2,
      depth: 24,
    });
    this.compositor.setWindowBackgroundPixmap(HOST_ROOT_ID, HOST_WEAVE_PIXMAP_ID);
  }

  /** XID of the one shared root window. Queried by every client's
   *  XOpenDisplay through the emx11_js_get_root_window bridge. */
  getRootWindow(): number {
    return HOST_ROOT_ID;
  }

  install(): void {
    globalThis.__EMX11__ = this;
  }

  /** Load a wasm client and bind its Module to the connection that
   *  Host.openDisplay allocates during the module's startup path.
   *
   *  Sequencing (the subtle part): wasm main() runs during
   *  `loadWasm`'s awaited factory. main() calls XOpenDisplay very
   *  early, which synchronously re-enters Host.openDisplay. We stash
   *  a PendingLaunch slot before the await so openDisplay can record
   *  the new connId into it; once loadWasm resolves we have the
   *  Module and both sides of the pair, and we stitch them together.
   *
   *  Launches are serialized — if two callers race, the second will
   *  see pendingLaunch and throw. In practice callers await each
   *  launchClient in order. */
  async launchClient(opts: LoadOptions): Promise<{ connId: number; module: EmscriptenModule }> {
    if (this.pendingLaunch) {
      throw new Error('em-x11: launchClient is not reentrant; await the previous launch first');
    }
    const pending: PendingLaunch = { connId: 0 };
    this.pendingLaunch = pending;
    let module: EmscriptenModule;
    try {
      module = await loadWasm(opts);
    } finally {
      this.pendingLaunch = null;
    }
    const conn = this.connections.get(pending.connId);
    if (!conn) {
      throw new Error(
        `em-x11: wasm at ${opts.glueUrl} finished loading without calling XOpenDisplay`,
      );
    }
    conn.module = module;
    return { connId: pending.connId, module };
  }

  getPointerXY(): Point {
    return { x: this.pointerX, y: this.pointerY };
  }

  /** Cross-connection XGetWindowAttributes fallback. When twm queries
   *  xeyes's shell for geometry, twm's local shadow table has no entry
   *  for it (a WM doesn't mirror other clients' windows). Xlib calls
   *  the emx11_js_get_window_attrs bridge, which lands here.
   *  dix/window.c treats window state as XID-keyed server state; this
   *  accessor is how we present that to a querying client. */
  getWindowAttrs(id: number): {
    x: number; y: number; width: number; height: number;
    mapped: boolean; overrideRedirect: boolean; borderWidth: number;
  } | null {
    const geom = this.compositor.attrsOf(id);
    if (!geom) return null;
    return {
      x: geom.x, y: geom.y, width: geom.width, height: geom.height,
      mapped: geom.mapped,
      overrideRedirect: this.overrideRedirect.get(id) ?? false,
      /* Host doesn't currently track border_width; twm ignores it for
       * frame sizing (it reads tmp_win->old_bw from XGetGeometry) so 0
       * is adequate here. Revisit if another WM needs it. */
      borderWidth: 0,
    };
  }

  /** XChangeProperty -- server-authoritative path (dix/property.c
   *  dixChangeWindowProperty). Stores by (window XID, atom) so any
   *  client can read it back regardless of who wrote it.
   *  @returns true on success, false on BadMatch (format/type mismatch
   *  with existing entry under Append/Prepend). */
  changeProperty(
    window: number, atom: number, type: number,
    format: 8 | 16 | 32, mode: number,
    data: Uint8Array,
  ): boolean {
    if (!this.compositor.attrsOf(window)) {
      /* BadWindow -- caller returns BadWindow. In real X this goes
       * through the resource manager; here the compositor is our
       * source of truth for "does this window exist". */
      return false;
    }
    let table = this.properties.get(window);
    if (!table) {
      table = new Map();
      this.properties.set(window, table);
    }
    const existing = table.get(atom);
    if (!existing || mode === PropModeReplace) {
      const entry: PropertyEntry = {
        type,
        format,
        nitems: (data.length * 8) / format,
        data: new Uint8Array(data),
      };
      table.set(atom, entry);
      return true;
    }
    /* Append / Prepend: format and type must match existing. */
    if (existing.format !== format || existing.type !== type) return false;
    if (data.length === 0) return true;
    const merged = new Uint8Array(existing.data.length + data.length);
    if (mode === PropModeAppend) {
      merged.set(existing.data, 0);
      merged.set(data, existing.data.length);
    } else {
      /* Prepend */
      merged.set(data, 0);
      merged.set(existing.data, data.length);
    }
    existing.data = merged;
    existing.nitems = (merged.length * 8) / format;
    return true;
  }

  /** XGetWindowProperty -- server-authoritative path (dix/property.c
   *  ProcGetProperty). Returns meta + a data slice corresponding to
   *  long_offset / long_length, both in 32-bit units per X protocol.
   *  `null` means BadWindow (unknown XID).
   *  When the atom doesn't exist, returns found=false with other
   *  fields zeroed -- matches Xlib's Success with type=None.
   *  When the stored type doesn't match reqType (and reqType isn't
   *  AnyPropertyType), returns found=false BUT with actualType/format
   *  populated so the caller can report them. */
  peekProperty(
    window: number, atom: number, reqType: number,
    longOffset: number, longLength: number, deleteFlag: boolean,
  ): {
    found: boolean; type: number; format: number;
    nitems: number; bytesAfter: number; data: Uint8Array;
  } | null {
    if (!this.compositor.attrsOf(window)) return null;
    const table = this.properties.get(window);
    const entry = table?.get(atom);
    if (!entry) {
      return { found: false, type: 0, format: 0, nitems: 0, bytesAfter: 0,
               data: new Uint8Array(0) };
    }
    if (reqType !== AnyPropertyType && reqType !== entry.type) {
      return { found: false, type: entry.type, format: entry.format,
               nitems: 0, bytesAfter: 0, data: new Uint8Array(0) };
    }
    /* X protocol: long_offset and long_length are in 32-bit units,
     * regardless of format. Data slice in bytes = (total_bytes - 4*offset)
     * clamped to 4*length. */
    const totalBytes = entry.data.length;
    const startByte = Math.min(Math.max(longOffset, 0) * 4, totalBytes);
    const wantBytes = Math.max(longLength, 0) * 4;
    const availBytes = totalBytes - startByte;
    const sliceBytes = Math.min(availBytes, wantBytes);
    const data = entry.data.subarray(startByte, startByte + sliceBytes);
    const bytesAfter = availBytes - sliceBytes;
    const unit = entry.format / 8;
    const nitemsReturned = unit > 0 ? sliceBytes / unit : 0;

    if (deleteFlag && bytesAfter === 0 && startByte === 0) {
      table!.delete(atom);
      if (table!.size === 0) this.properties.delete(window);
    }
    return {
      found: true, type: entry.type, format: entry.format,
      nitems: nitemsReturned, bytesAfter,
      data: new Uint8Array(data),
    };
  }

  /** XDeleteProperty (dix/property.c DeleteProperty). */
  deleteProperty(window: number, atom: number): void {
    const table = this.properties.get(window);
    if (!table) return;
    table.delete(atom);
    if (table.size === 0) this.properties.delete(window);
  }

  /** XListProperties (dix/property.c ProcListProperties). */
  listProperties(window: number): number[] {
    const table = this.properties.get(window);
    if (!table) return [];
    return Array.from(table.keys());
  }

  onInit(_screenWidth: number, _screenHeight: number): void {
    /* C expects us to adopt its idea of screen size, but the browser is the
     * authority. We ignore the hint and the C side will be corrected the
     * next time it queries through XDisplayWidth/Height once we wire that
     * back through. TODO: plumb the correction call. */
  }

  openDisplay(): { connId: number; xidBase: number; xidMask: number } {
    const connId = ++this.nextConnId;
    const xidBase = connId * XID_PER_CONN;
    const xidMask = XID_MASK;
    this.connections.set(connId, {
      connId,
      xidBase,
      xidMask,
      module: null,
      ownedWindows: new Set(),
    });
    if (this.pendingLaunch) this.pendingLaunch.connId = connId;
    return { connId, xidBase, xidMask };
  }

  closeDisplay(connId: number): void {
    const conn = this.connections.get(connId);
    if (!conn) return;
    /* Drop every window this connection owned. Compositor cleans up
     * its side; windowToConn, subscriptions, and override_redirect
     * drop their entries. Pixmaps/atoms still live in their global
     * tables -- Step 2b will sweep those. */
    for (const winId of conn.ownedWindows) {
      this.compositor.destroyWindow(winId);
      this.windowToConn.delete(winId);
      this.windowSubscriptions.delete(winId);
      this.overrideRedirect.delete(winId);
      this.properties.delete(winId);
    }
    /* Also drop this connection's subscription entries on OTHER
     * clients' windows (a WM that observed substructure on root
     * shouldn't keep that claim after disconnecting). */
    for (const subs of this.windowSubscriptions.values()) {
      subs.delete(connId);
    }
    this.connections.delete(connId);
  }

  onWindowCreate(
    connId: number,
    id: number,
    parent: number,
    x: number,
    y: number,
    width: number,
    height: number,
    background: number,
  ): void {
    const conn = this.connections.get(connId);
    if (conn) {
      conn.ownedWindows.add(id);
      this.windowToConn.set(id, connId);
    }
    this.compositor.addWindow(id, parent, x, y, width, height, background);
  }

  onWindowConfigure(id: number, x: number, y: number, w: number, h: number): void {
    this.compositor.configureWindow(id, x, y, w, h);
  }

  onWindowMap(connId: number, id: number): void {
    /* SubstructureRedirect decision (x11protocol.txt §1592):
     *   Redirect applies iff the window's PARENT has a connection
     *   that selected SubstructureRedirectMask on it, AND the caller
     *   is a different connection, AND override_redirect is False.
     * Otherwise proceed with the actual map. The redirect path is
     * dormant in current demos -- no client subscribes to it -- but
     * the plumbing stays so a future WM (Host-embedded or another
     * X-client WM port) can light it up without touching Host. */
    const parent = this.compositor.parentOf(id);
    const holderConnId = parent !== 0 ? this.redirectHolderFor(parent) : null;
    const overrideRedirect = this.overrideRedirect.get(id) ?? false;
    if (
      holderConnId !== null &&
      holderConnId !== connId &&
      !overrideRedirect
    ) {
      this.dispatchMapRequest(holderConnId, parent, id);
      return;
    }
    this.compositor.mapWindow(id);
    /* No Expose synthesis here: that lives in C's XMapWindow. Doing
     * it Host-side via mod.ccall would race the launchClient handoff
     * (conn.module is null during the initial map that happens inside
     * loadWasm's awaited factory, before launchClient stitches the
     * Module reference onto the Connection). Re-add a Host-side path
     * only if we revive SubstructureRedirect, where the map might be
     * served by a different connection than the owner. */
  }

  onWindowUnmap(_connId: number, id: number): void {
    /* Unmap isn't redirected in X -- only Map / Configure / Circulate
     * go through SubstructureRedirect. Unmap is always immediate. */
    this.compositor.unmapWindow(id);
  }

  onWindowDestroy(id: number): void {
    const connId = this.windowToConn.get(id);
    if (connId !== undefined) {
      const conn = this.connections.get(connId);
      conn?.ownedWindows.delete(id);
      this.windowToConn.delete(id);
    }
    this.windowSubscriptions.delete(id);
    this.overrideRedirect.delete(id);
    this.properties.delete(id);           /* dix/property.c::DeleteAllWindowProperties */
    this.compositor.destroyWindow(id);
  }

  onSelectInput(connId: number, id: number, mask: number): void {
    let subs = this.windowSubscriptions.get(id);
    if (!subs) {
      subs = new Map();
      this.windowSubscriptions.set(id, subs);
    }
    if (mask === 0) {
      subs.delete(connId);
      if (subs.size === 0) this.windowSubscriptions.delete(id);
    } else {
      /* x11protocol.txt §1477: at most one client may select
       * SubstructureRedirectMask on a given window. If another
       * connection already holds it, strip the bit from this request
       * and log -- matches X server's BadAccess reply as a soft warning. */
      if (mask & SubstructureRedirectMask) {
        for (const [existingConn, existingMask] of subs) {
          if (
            existingConn !== connId &&
            existingMask & SubstructureRedirectMask
          ) {
            console.warn(
              `em-x11: conn ${connId} requested SubstructureRedirect on win ` +
                `${id} but conn ${existingConn} already holds it; ignoring`,
            );
            mask &= ~SubstructureRedirectMask;
            break;
          }
        }
        if (mask & SubstructureRedirectMask) {
          console.info(
            `em-x11: conn ${connId} now holds SubstructureRedirect on win ${id}`,
          );
        }
      }
      subs.set(connId, mask);
    }
  }

  onSetOverrideRedirect(id: number, flag: boolean): void {
    if (flag) this.overrideRedirect.set(id, true);
    else this.overrideRedirect.delete(id);
  }

  onReparentWindow(id: number, parent: number, x: number, y: number): void {
    this.compositor.reparentWindow(id, parent, x, y);
  }

  /** Look up which connection (if any) selected SubstructureRedirectMask
   *  on this window. Returns null if nobody holds it. */
  private redirectHolderFor(winId: number): number | null {
    const subs = this.windowSubscriptions.get(winId);
    if (!subs) return null;
    for (const [connId, mask] of subs) {
      if (mask & SubstructureRedirectMask) return connId;
    }
    return null;
  }

  /** Push a MapRequest into the holder's event queue via ccall. The
   *  holder is some other wasm Module (typically twm) currently blocked
   *  in XNextEvent; once this returns, its emscripten_sleep loop will
   *  pick up the event on the next yield. */
  private dispatchMapRequest(
    holderConnId: number,
    parent: number,
    window: number,
  ): void {
    const holder = this.connections.get(holderConnId);
    const mod = holder?.module;
    if (!mod) {
      console.warn(
        `em-x11: MapRequest for win ${window} but redirect holder conn ` +
          `${holderConnId} has no Module yet; dropping`,
      );
      return;
    }
    mod.ccall(
      'emx11_push_map_request',
      null,
      ['number', 'number'],
      [parent, window],
    );
  }

  onWindowSetBgPixmap(id: number, pmId: number): void {
    this.compositor.setWindowBackgroundPixmap(id, pmId);
  }

  onClearArea(id: number, x: number, y: number, w: number, h: number): void {
    this.compositor.clearArea(id, x, y, w, h);
  }

  onFillRect(id: number, x: number, y: number, w: number, h: number, color: number): void {
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapFillRect(pm, x, y, w, h, color);
      return;
    }
    this.compositor.fillRect(id, x, y, w, h, color);
  }

  onDrawLine(
    id: number,
    x1: number,
    y1: number,
    x2: number,
    y2: number,
    color: number,
    lineWidth: number,
  ): void {
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapLine(pm, x1, y1, x2, y2, color, lineWidth);
      return;
    }
    this.compositor.drawLine(id, x1, y1, x2, y2, color, lineWidth);
  }

  onDrawArc(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1: number,
    angle2: number,
    color: number,
    lineWidth: number,
  ): void {
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapArc(pm, x, y, w, h, angle1, angle2, color, lineWidth);
      return;
    }
    this.compositor.drawArc(id, x, y, w, h, angle1, angle2, color, lineWidth);
  }

  onFillArc(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1: number,
    angle2: number,
    color: number,
  ): void {
    const pm = this.pixmaps.get(id);
    if (pm) {
      this.paintPixmapArc(pm, x, y, w, h, angle1, angle2, color, null);
      return;
    }
    this.compositor.fillArc(id, x, y, w, h, angle1, angle2, color);
  }

  onFillPolygon(id: number, points: Point[], shape: number, mode: number, color: number): void {
    this.compositor.fillPolygon(id, points, shape, mode, color);
  }

  onDrawPoints(id: number, points: Point[], mode: number, color: number): void {
    this.compositor.drawPoints(id, points, mode, color);
  }

  onDrawString(
    id: number,
    x: number,
    y: number,
    font: string,
    text: string,
    fgColor: number,
    bgColor: number,
    imageMode: number,
  ): void {
    this.compositor.drawString(id, x, y, font, text, fgColor, bgColor, imageMode !== 0);
  }

  onFlush(): void {
    /* No-op: the compositor already presents through requestAnimationFrame.
     * Kept as a hook for future synchronous-flush scenarios. */
  }

  onWindowShape(id: number, rects: ShapeRect[]): void {
    this.compositor.setWindowShape(id, rects);
  }

  /* -- Pixmap lifecycle --------------------------------------------------- */

  onPixmapCreate(id: number, width: number, height: number, depth: number): void {
    if (width <= 0 || height <= 0) return;
    if (width > MAX_PIXMAP_EDGE || height > MAX_PIXMAP_EDGE) {
      console.warn(
        `em-x11: refusing pixmap ${id} at ${width}x${height} (cap ${MAX_PIXMAP_EDGE})`,
      );
      return;
    }
    const oc = new OffscreenCanvas(width, height);
    const ctx = oc.getContext('2d');
    if (!ctx) return;
    this.pixmaps.set(id, { canvas: oc, ctx, width, height, depth });
  }

  onPixmapDestroy(id: number): void {
    this.pixmaps.delete(id);
  }

  /* -- SHAPE: decode pixmap bits into a bounding region ------------------ */

  onShapeCombineMask(
    destId: number,
    srcId: number,
    xOff: number,
    yOff: number,
    _op: number,
  ): void {
    const pm = this.pixmaps.get(srcId);
    if (!pm) return;

    const image = pm.ctx.getImageData(0, 0, pm.width, pm.height);
    const data = image.data;
    const rects: ShapeRect[] = [];

    /* Row-wise run-length encoding: each horizontal run of "set" pixels
     * becomes one 1-pixel-tall rectangle. Two eyes @ 200x200 generate
     * roughly 2 * 2 * radius rects; fine for the compositor's clip path.
     * We could do better with a proper region coalescing pass, but every
     * frame re-applies the clip, so O(h) rects per shape change is OK. */
    for (let y = 0; y < pm.height; y++) {
      let runStart = -1;
      for (let x = 0; x < pm.width; x++) {
        const alpha = data[(y * pm.width + x) * 4 + 3];
        if (alpha !== undefined && alpha > 0) {
          if (runStart < 0) runStart = x;
        } else if (runStart >= 0) {
          rects.push({ x: runStart + xOff, y: y + yOff, w: x - runStart, h: 1 });
          runStart = -1;
        }
      }
      if (runStart >= 0) {
        rects.push({
          x: runStart + xOff,
          y: y + yOff,
          w: pm.width - runStart,
          h: 1,
        });
      }
    }

    this.compositor.setWindowShape(destId, rects);
  }

  /* -- Input event bridge ------------------------------------------------
   *
   * Moved from src/runtime/events.ts into Host at Step 2 because the
   * target Module is no longer fixed: we resolve it per event via the
   * window's owning connection. A window with no registered Module (a
   * legacy headless connection) is treated as "no target" and the
   * event is dropped cleanly.
   */

  private attachInputBridge(): void {
    const el = this.canvas.element;
    el.addEventListener('mousedown', (e) => this.onMouseButton(e, X_ButtonPress));
    el.addEventListener('mouseup', (e) => this.onMouseButton(e, X_ButtonRelease));
    el.addEventListener('mousemove', (e) => this.onMouseMove(e));
    el.addEventListener('contextmenu', (e) => e.preventDefault());
    el.addEventListener('mousedown', () => el.focus());

    window.addEventListener('keydown', (e) => this.onKey(e, X_KeyPress));
    window.addEventListener('keyup', (e) => this.onKey(e, X_KeyRelease));
  }

  private cssPoint(e: MouseEvent): { x: number; y: number } {
    const rect = this.canvas.element.getBoundingClientRect();
    return { x: e.clientX - rect.left, y: e.clientY - rect.top };
  }

  /** Resolve the Module that owns a window. Returns null if the window
   *  isn't tracked, if the owning connection has no Module (legacy
   *  headless case), or if the connection was closed.
   *
   *  Host-owned windows (conn_id=0, currently just the shared root)
   *  have no "owner" Module. As a temporary fallback we route their
   *  events to the first real connection -- which in the session-demo
   *  launch convention is twm, the window manager. Step B replaces
   *  this with an XSelectInput subscription table on the Host side so
   *  MapRequest / SubstructureRedirect events are dispatched to the
   *  actual holder(s), not a positional heuristic. */
  private moduleForWindow(winId: number): EmscriptenModule | null {
    const connId = this.windowToConn.get(winId);
    if (connId === undefined) return null;
    if (connId === 0) {
      for (const conn of this.connections.values()) {
        if (conn.module) return conn.module;
      }
      return null;
    }
    const conn = this.connections.get(connId);
    return conn?.module ?? null;
  }

  private onMouseButton(e: MouseEvent, xType: number): void {
    const { x, y } = this.cssPoint(e);
    const win = this.compositor.findWindowAt(x, y);
    if (win === null) return;
    const module = this.moduleForWindow(win);
    if (!module) return;
    if (xType === X_ButtonPress) this.focusedWindow = win;
    module.ccall(
      'emx11_push_button_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
      [xType, win, x, y, x, y, e.button + 1, modifiersFromEvent(e)],
    );
  }

  private onMouseMove(e: MouseEvent): void {
    const { x, y } = this.cssPoint(e);
    const win = this.compositor.findWindowAt(x, y);
    if (win === null) return;
    const module = this.moduleForWindow(win);
    if (!module) return;
    module.ccall(
      'emx11_push_motion_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [win, x, y, x, y, modifiersFromEvent(e)],
    );
  }

  private onKey(e: KeyboardEvent, xType: number): void {
    if (this.focusedWindow === null) return;
    const module = this.moduleForWindow(this.focusedWindow);
    if (!module) return;
    const keysym = keyEventToKeysym(e);
    if (keysym === 0) return;
    if (document.activeElement === this.canvas.element) e.preventDefault();

    module.ccall(
      'emx11_push_key_event',
      null,
      ['number', 'number', 'number', 'number', 'number', 'number'],
      [xType, this.focusedWindow, keysym, modifiersFromEvent(e), 0, 0],
    );
  }

  /* -- Pixmap drawing helpers -------------------------------------------- */

  /** Depth-1 pixmaps treat foreground==0 as "bit off" (transparent) and
   *  anything else as "bit on" (opaque). Color pixmaps paint the value
   *  directly as RGB. */
  private paintPixmapFillRect(
    pm: Pixmap,
    x: number,
    y: number,
    w: number,
    h: number,
    color: number,
  ): void {
    if (pm.depth === 1 && color === 0) {
      pm.ctx.clearRect(x, y, w, h);
      return;
    }
    pm.ctx.fillStyle = pixelToCssColor(color);
    pm.ctx.fillRect(x, y, w, h);
  }

  private paintPixmapArc(
    pm: Pixmap,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1: number,
    angle2: number,
    color: number,
    lineWidth: number | null,
  ): void {
    pm.ctx.save();
    if (pm.depth === 1 && color === 0) {
      /* "Erase" -- paint the arc with destination-out so any previously
       * set bits inside the ellipse become unset. */
      pm.ctx.globalCompositeOperation = 'destination-out';
      pm.ctx.fillStyle = '#000';
    } else if (lineWidth === null) {
      pm.ctx.fillStyle = pixelToCssColor(color);
    } else {
      pm.ctx.strokeStyle = pixelToCssColor(color);
      pm.ctx.lineWidth = lineWidth || 1;
    }
    arcPath(pm.ctx, x, y, w, h, angle1, angle2);
    if (lineWidth === null) pm.ctx.fill();
    else pm.ctx.stroke();
    pm.ctx.restore();
  }

  private paintPixmapLine(
    pm: Pixmap,
    x1: number,
    y1: number,
    x2: number,
    y2: number,
    color: number,
    lineWidth: number,
  ): void {
    pm.ctx.save();
    if (pm.depth === 1 && color === 0) {
      pm.ctx.globalCompositeOperation = 'destination-out';
      pm.ctx.strokeStyle = '#000';
    } else {
      pm.ctx.strokeStyle = pixelToCssColor(color);
    }
    pm.ctx.lineWidth = lineWidth || 1;
    pm.ctx.beginPath();
    pm.ctx.moveTo(x1 + 0.5, y1 + 0.5);
    pm.ctx.lineTo(x2 + 0.5, y2 + 0.5);
    pm.ctx.stroke();
    pm.ctx.restore();
  }
}
