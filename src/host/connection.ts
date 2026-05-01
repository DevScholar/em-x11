/**
 * Connection manager. One Connection per wasm client (XOpenDisplay).
 * Tracks the bound EmscriptenModule for ccall-back routing, the XID
 * range allocated to the client (x11protocol.txt §869/§935), and the
 * windows the client owns so they can be torn down at XCloseDisplay.
 *
 * Also coordinates the launch handoff: launchClient awaits the
 * Emscripten factory, while the wasm's main() runs synchronously
 * inside that await and calls XOpenDisplay early. The pendingLaunch
 * slot lets openDisplay tell launchClient which connection id the
 * new module ended up with.
 */

import type { Host } from './index.js';
import { loadWasm, type LoadOptions } from '../loader/wasm.js';
import type { EmscriptenModule } from '../types/emscripten.js';
import { XID_PER_CONN, XID_MASK } from './constants.js';

/** The slice of an Emscripten Module the host actually invokes through.
 *  EmscriptenModule satisfies this; the Pyodide path supplies a shim
 *  that resolves names through LDSO.loadedLibsByName since side-module
 *  exports don't live on the main `_module`. Signature mirrors the
 *  local EmscriptenModule.ccall so the static archive (wacl-tk) path
 *  stays a drop-in. */
export interface ModuleCcallSurface {
  ccall(
    name: string,
    returnType: string | null,
    argTypes: string[],
    args: unknown[],
  ): unknown;
}

export interface Connection {
  connId: number;
  xidBase: number;
  xidMask: number;
  /** Registered after launchClient loads the wasm. Null for a
   *  "headless" connection (test harness or legacy demos that open
   *  their own wasm without going through launchClient). */
  module: ModuleCcallSurface | null;
  /** XIDs created by this connection; kept so we can clean them up
   *  when the client disconnects. */
  ownedWindows: Set<number>;
}

/** Build a ModuleCcallSurface from a Pyodide-loaded side module's
 *  exports (typically `pyodide._module.LDSO.loadedLibsByName[soPath].exports`).
 *  The host's ccalls are all numbers-in / void-or-number-out, so we
 *  just call the export with the args and return the value; no string
 *  marshalling needed. */
export function makeSideModuleSurface(
  exports: Record<string, (...args: unknown[]) => unknown>,
): ModuleCcallSurface {
  return {
    ccall(name, _ret, _argTypes, args) {
      const fn = exports[name];
      if (typeof fn !== 'function') {
        throw new Error(
          `em-x11: side-module export '${name}' not found (have ${Object.keys(exports).filter((k) => k.startsWith('emx11')).join(', ')})`,
        );
      }
      return fn(...args);
    },
  };
}

/* Handoff slot used by launchClient so Host.openDisplay (called from
 * inside the wasm's main()) can find which Module instance is
 * currently booting. Because launchClient awaits serially, only one
 * launch can be pending at a time. */
interface PendingLaunch {
  connId: number;
}

export class ConnectionManager {
  private readonly connections = new Map<number, Connection>();
  private readonly windowToConn = new Map<number, number>();
  /** Exposes deferred during the bootstrap window between a connection's
   *  XOpenDisplay and the matching launchClient resolution. While the
   *  client's main() runs (including its first XMapWindow), conn.module
   *  is still null so a Host->C ccall would no-op. We stash the window
   *  IDs here and drain them in launchClient once `conn.module` lands.
   *  Keyed by connId; inner Set holds windows pending an Expose. */
  private readonly pendingExposes = new Map<number, Set<number>>();
  private nextConnId = 0;
  private pendingLaunch: PendingLaunch | null = null;
  /** Surface auto-bound to every future open() conn. Set by bindModule
   *  when called without an explicit connId — covers the Pyodide case
   *  where one wasm Module hosts multiple Tcl interps and each
   *  tkinter.Tk() opens its own display. Without this, only the first
   *  display gets routed and later widgets paint into the void. */
  private defaultModule: ModuleCcallSurface | null = null;

  constructor(private readonly host: Host) {}

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
    /* Drain Exposes that were deferred while conn.module was null.
     * onWindowMap / onWindowConfigure parked them here; now that the
     * client's queue is reachable via ccall, push them through. The
     * client's main() has already suspended in XNextEvent's
     * emscripten_sleep, so the pushed events wake it on the next yield
     * and its handler paints the window content. */
    const deferred = this.pendingExposes.get(pending.connId);
    if (deferred) {
      for (const winId of deferred) {
        this.host.events.pushExposeForWindow(winId, module);
      }
      this.pendingExposes.delete(pending.connId);
    }
    return { connId: pending.connId, module };
  }

  open(): { connId: number; xidBase: number; xidMask: number } {
    const connId = ++this.nextConnId;
    const xidBase = connId * XID_PER_CONN;
    const xidMask = XID_MASK;
    this.connections.set(connId, {
      connId,
      xidBase,
      xidMask,
      module: this.defaultModule,
      ownedWindows: new Set(),
    });
    if (this.pendingLaunch) this.pendingLaunch.connId = connId;
    return { connId, xidBase, xidMask };
  }

  close(connId: number): void {
    const conn = this.connections.get(connId);
    if (!conn) return;
    /* Drop every window this connection owned. Renderer cleans up
     * its side; windowToConn, subscriptions, and override_redirect
     * drop their entries. Pixmaps/atoms still live in their global
     * tables -- a future sweep will collect those too. */
    for (const winId of conn.ownedWindows) {
      this.host.renderer.destroyWindow(winId);
      this.windowToConn.delete(winId);
      this.host.events.forgetWindow(winId);
      this.host.window.forgetWindow(winId);
      this.host.property.deleteAllForWindow(winId);
    }
    this.host.events.forgetConnection(connId);
    this.connections.delete(connId);
  }

  /* -- accessors used by sibling managers -------------------------------- */

  get(connId: number): Connection | undefined {
    return this.connections.get(connId);
  }

  /** Owning connection id for a window, or undefined if untracked. */
  connOf(winId: number): number | undefined {
    return this.windowToConn.get(winId);
  }

  /** Iterator over all live connections. Used by InputBridge to find a
   *  fallback module for events on Host-owned (root) windows. */
  values(): IterableIterator<Connection> {
    return this.connections.values();
  }

  /** WindowManager.onCreate calls this to attach a new XID to its
   *  owning connection. Includes the windowToConn back-pointer used
   *  for routing inbound events / Exposes by ownership. */
  recordOwnership(connId: number, winId: number): void {
    const conn = this.connections.get(connId);
    if (conn) {
      conn.ownedWindows.add(winId);
      this.windowToConn.set(winId, connId);
    }
  }

  /** WindowManager.onDestroy: remove the (winId -> connId) mapping
   *  and the conn's ownedWindows entry, but DON'T touch render state
   *  here -- the caller handles renderer.destroyWindow. */
  dropOwnership(winId: number): void {
    const connId = this.windowToConn.get(winId);
    if (connId !== undefined) {
      const conn = this.connections.get(connId);
      conn?.ownedWindows.delete(winId);
      this.windowToConn.delete(winId);
    }
  }

  /** Special-case binding for windows the Host itself owns (root). The
   *  Host conn is `0`, which is reserved -- no real client can ever
   *  open() into that slot since nextConnId starts at 1. */
  bindWindowToConn(winId: number, connId: number): void {
    this.windowToConn.set(winId, connId);
  }

  /** Park an Expose to be sent to the connection's module once
   *  launchClient binds it. Called from EventDispatcher when the
   *  owner's Module isn't installed yet. */
  deferExpose(ownerConnId: number, winId: number): void {
    let pending = this.pendingExposes.get(ownerConnId);
    if (!pending) {
      pending = new Set();
      this.pendingExposes.set(ownerConnId, pending);
    }
    pending.add(winId);
  }

  /** Pyodide / dlopen path: bind an already-loaded EmscriptenModule to
   *  a connection. Unlike launchClient, the Host did not load this wasm;
   *  Pyodide did, and we just hand its Module over. Caller is expected
   *  to invoke this AFTER libemx11.so is dlopen'd but BEFORE any code
   *  that calls XOpenDisplay (e.g. _tkinter.create with wantTk=1).
   *
   *  If `connId` is omitted we bind the most-recently-opened connection
   *  (single-client common case). Drains any deferred Exposes the same
   *  way launchClient does, so widgets that mapped before the Module
   *  was bound still receive their first paint. */
  bindModule(module: ModuleCcallSurface, connId?: number): number {
    let id = connId;
    if (id === undefined) {
      if (this.nextConnId === 0) {
        throw new Error('em-x11: bindModule called before any XOpenDisplay');
      }
      id = this.nextConnId;
      // No explicit connId → caller is the "single-Module hosts many
      // displays" case (Pyodide / dlopen). Set default so future opens
      // pick up this surface automatically.
      this.defaultModule = module;
    }
    const conn = this.connections.get(id);
    if (!conn) {
      throw new Error(`em-x11: bindModule: no connection with id ${id}`);
    }
    conn.module = module;
    const deferred = this.pendingExposes.get(id);
    if (deferred) {
      for (const winId of deferred) {
        this.host.events.pushExposeForWindow(winId, module);
      }
      this.pendingExposes.delete(id);
    }
    return id;
  }

  /** Pyodide pre-bind: stash the Module as the default for any future
   *  XOpenDisplay, BEFORE any connection exists. Use this when a single
   *  wasm Module will host the very first XOpenDisplay (e.g. when going
   *  straight to `tkinter.Tk()` rather than calling `_tkinter.create`
   *  manually first). After the first open, future connections from
   *  the same Module also inherit defaultModule. Idempotent.
   *
   *  Without this, you'd need an XOpenDisplay-first-then-bindModule
   *  dance that requires an already-opened display, which forces a
   *  redundant `_tkinter.create` step and produces TWO Tk roots --
   *  one of which (typically tkinter.Tk's 200x200 default) ends up
   *  obscuring the other. */
  setDefaultModule(module: ModuleCcallSurface): void {
    this.defaultModule = module;
  }
}
