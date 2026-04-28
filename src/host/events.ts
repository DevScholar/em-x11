/**
 * Event dispatch + redirect routing. Mirrors xserver/dix/events.c +
 * dix/dispatch.c subset we actually need:
 *   - XSelectInput mask table per (window, connection)
 *   - SubstructureRedirect routing on XMapWindow
 *   - Synthesised Expose events when a structural change wipes pixels
 *
 * The redirect table is the load-bearing piece: a WM client's
 * XSelectInput(root, SubstructureRedirectMask) lands here, and every
 * subsequent XMapWindow on a child of root that isn't override-redirect
 * gets converted into a MapRequest pushed into the WM's queue.
 */

import type { Host } from './index.js';
import type { EmscriptenModule } from '../types/emscripten.js';
import { SubstructureRedirectMask } from './constants.js';

export class EventDispatcher {
  /** Per-window event-mask subscriptions from XSelectInput, keyed by
   *  the subscribing connection. Host consults this for substructure
   *  redirect / notify routing. Empty inner map means "no subscribers". */
  private readonly windowSubscriptions = new Map<number, Map<number, number>>();
  /** Pending callers of waitForSubstructureRedirect, keyed by the window
   *  whose redirect holder they're waiting for. Drained by onSelectInput
   *  when a SubstructureRedirect subscription lands on that window.
   *  Used by launchTwm to gate the xeyes launch on twm being ready to
   *  intercept MapRequest -- without this, Emscripten's factory Promise
   *  can resolve before twm's main() reaches its XSelectInput(root,
   *  SubstructureRedirectMask) call, letting xeyes map at (0,0) before
   *  twm ever sees a MapRequest. */
  private readonly redirectWaiters = new Map<number, ((connId: number) => void)[]>();

  constructor(private readonly host: Host) {}

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
          const waiters = this.redirectWaiters.get(id);
          if (waiters) {
            this.redirectWaiters.delete(id);
            for (const resolve of waiters) resolve(connId);
          }
        }
      }
      subs.set(connId, mask);
    }
  }

  /** Resolve when some client holds SubstructureRedirectMask on `winId`.
   *  If already held, resolves on the next microtask with the current
   *  holder. Used by launchTwm to block `launchClient(xeyes)` from
   *  running until the WM has armed its MapRequest intercept -- the
   *  Emscripten factory Promise is not a strong enough barrier because
   *  it resolves when main() first yields to JS, which can be before
   *  XSelectInput(root, SubstructureRedirectMask) completes (and even
   *  if it isn't in the current build, this barrier is the right
   *  contract long-term). Rejects after `timeoutMs` so a WM that
   *  never arms its redirect doesn't deadlock the session. */
  waitForSubstructureRedirect(winId: number, timeoutMs = 5000): Promise<number> {
    const existing = this.redirectHolderFor(winId);
    if (existing !== null) return Promise.resolve(existing);
    return new Promise((resolve, reject) => {
      const waiter = (connId: number): void => {
        clearTimeout(timer);
        resolve(connId);
      };
      const timer = setTimeout(() => {
        const list = this.redirectWaiters.get(winId);
        if (list) {
          const i = list.indexOf(waiter);
          if (i >= 0) list.splice(i, 1);
          if (list.length === 0) this.redirectWaiters.delete(winId);
        }
        reject(new Error(
          `em-x11: timed out waiting for SubstructureRedirect on win ${winId}`,
        ));
      }, timeoutMs);
      let list = this.redirectWaiters.get(winId);
      if (!list) {
        list = [];
        this.redirectWaiters.set(winId, list);
      }
      list.push(waiter);
    });
  }

  /** Look up which connection (if any) selected SubstructureRedirectMask
   *  on this window. Returns null if nobody holds it. */
  redirectHolderFor(winId: number): number | null {
    const subs = this.windowSubscriptions.get(winId);
    if (!subs) return null;
    for (const [connId, mask] of subs) {
      if (mask & SubstructureRedirectMask) return connId;
    }
    return null;
  }

  /** Push a full-window Expose to the owner of `id`, via ccall on the
   *  owner's Module. Used by WindowManager.onWindowMap and
   *  onWindowConfigure to keep Expose routing consistent regardless of
   *  which connection initiated the structural change.
   *
   *  When the owner has no Module yet (XOpenDisplay returned but
   *  launchClient hasn't resolved its factory), park the window in
   *  the connection's pending-expose set; ConnectionManager.launchClient
   *  drains it when it binds the Module.
   *  Pass `forceModule` from launchClient itself to bypass the owner
   *  lookup -- at drain time we already have the freshly-bound module
   *  in hand and the conn record reflects it. */
  pushExposeForWindow(id: number, forceModule: EmscriptenModule | null): void {
    const geom = this.host.renderer.geometryOf(id);
    if (!geom) return;
    let module: EmscriptenModule | null = forceModule;
    if (!module) {
      const ownerConnId = this.host.connection.connOf(id);
      if (ownerConnId === undefined) return;
      /* Host-owned windows (conn 0, currently just the root) have no
       * client to expose. Skip silently. */
      if (ownerConnId === 0) return;
      const conn = this.host.connection.get(ownerConnId);
      if (!conn) return;
      if (!conn.module) {
        /* Bootstrap: queue and let launchClient drain. */
        this.host.connection.deferExpose(ownerConnId, id);
        return;
      }
      module = conn.module;
    }
    module.ccall(
      'emx11_push_expose_event',
      null,
      ['number', 'number', 'number', 'number', 'number'],
      [id, 0, 0, geom.width, geom.height],
    );
  }

  /** Push a MapRequest into the holder's event queue via ccall. The
   *  holder is some other wasm Module (typically twm) currently blocked
   *  in XNextEvent; once this returns, its emscripten_sleep loop will
   *  pick up the event on the next yield. */
  dispatchMapRequest(holderConnId: number, parent: number, window: number): void {
    const holder = this.host.connection.get(holderConnId);
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

  /** Drop one connection's subscription entries on every window. Called
   *  by ConnectionManager.closeDisplay -- a WM that observed
   *  substructure on root shouldn't keep that claim after disconnect. */
  forgetConnection(connId: number): void {
    for (const subs of this.windowSubscriptions.values()) {
      subs.delete(connId);
    }
  }

  /** Drop all subscriptions on a window. Called by WindowManager on
   *  XDestroyWindow + by ConnectionManager when the owning connection
   *  closes. */
  forgetWindow(id: number): void {
    this.windowSubscriptions.delete(id);
  }
}
