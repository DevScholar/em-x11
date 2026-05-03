/**
 * Generic Client Worker entry. One instance runs inside each per-wasm
 * Worker spawned by the orchestrator. Loads glue + wasm, installs the
 * EM_JS globals (`__EMX11_CHANNEL__`, `__EMX11_SAB__`, `__EMX11_CONN__`,
 * `__EMX11_XID_TO_SLOT__`) BEFORE the wasm factory runs so bridges
 * fire correctly during module init, then dispatches incoming
 * `XEvent.*` / `Slot.*` messages from the Server Worker.
 *
 * Why globals not imports: EM_JS bodies are inlined as JS strings into
 * the wasm custom section at compile time. They reach their
 * collaborators only through `globalThis.__EMX11_*` -- the
 * customSection embed has no access to the consumer's module scope.
 * Same pattern libemx11 uses in legacy mode with `__EMX11__`.
 */

/// <reference lib="webworker" />

import { RpcChannel } from '../rpc/channel.js';
import type {
  BootstrapClient,
  ServerToClientXEvent,
  ServerToClientSlot,
} from '../rpc/protocol.js';
import { attachSab, type SabViews } from '../rpc/sab.js';
import type { EmscriptenModule } from '../../types/emscripten.js';

declare const self: DedicatedWorkerGlobalScope;

/* Hoist the globals we want the bridges to see. Installed here BEFORE
 * the wasm factory runs. */
declare global {
  /* eslint-disable no-var */
  var __EMX11_CHANNEL__: RpcChannel | undefined;
  var __EMX11_SAB__: SabViews | undefined;
  var __EMX11_CONN__: {
    connId: number;
    xidBase: number;
    xidMask: number;
    rootWindow: number;
  } | undefined;
  var __EMX11_XID_TO_SLOT__: Uint32Array | undefined;
  var __EMX11_MODULE__: EmscriptenModule | undefined;
  /* eslint-enable no-var */
}

self.addEventListener('message', bootstrapOnce);

function bootstrapOnce(ev: MessageEvent): void {
  const data = ev.data as BootstrapClient;
  if (!data || data.kind !== 'BootstrapClient') return;
  self.removeEventListener('message', bootstrapOnce);

  const channel = new RpcChannel(data.serverPort);
  const sabViews = attachSab(data.sab);

  /* XID-to-slot lookup table. XIDs are per-conn via xidBase+xidMask,
   * so a dense Uint32Array indexed by `(xid - xidBase)` is at most
   * xidMask+1 entries. xidMask is typically 0x001FFFFF = ~2M ids,
   * which is ~8MB. That's too big to allocate eagerly -- in practice
   * clients rarely allocate more than a few hundred XIDs. Size the
   * table to 65536 (256KB) and grow on Slot.Assigned if needed. */
  const xidToSlot = new Uint32Array(65536);

  globalThis.__EMX11_CHANNEL__ = channel;
  globalThis.__EMX11_SAB__ = sabViews;
  globalThis.__EMX11_CONN__ = {
    connId: data.connId,
    xidBase: data.xidBase,
    xidMask: data.xidMask,
    rootWindow: /* injected below */ 0,
  };
  globalThis.__EMX11_XID_TO_SLOT__ = xidToSlot;

  channel.on<ServerToClientSlot>('Slot.Assigned', (msg) => {
    if (msg.kind !== 'Slot.Assigned') return;
    const base = data.xidBase >>> 0;
    const rel = (msg.winId >>> 0) - base;
    if (rel >= xidToSlot.length) {
      /* Grow the table. 2x until it fits. Cap at xidMask+1. */
      const maxLen = (data.xidMask >>> 0) + 1;
      let newLen = xidToSlot.length;
      while (newLen <= rel && newLen < maxLen) newLen *= 2;
      if (newLen > maxLen) newLen = maxLen;
      if (newLen <= rel) return;    /* exceeds xidMask; shouldn't happen */
      const grown = new Uint32Array(newLen);
      grown.set(xidToSlot);
      globalThis.__EMX11_XID_TO_SLOT__ = grown;
      grown[rel] = msg.slot;
      return;
    }
    xidToSlot[rel] = msg.slot;
  });
  channel.on<ServerToClientSlot>('Slot.Freed', (msg) => {
    if (msg.kind !== 'Slot.Freed') return;
    const rel = (msg.winId >>> 0) - (data.xidBase >>> 0);
    const table = globalThis.__EMX11_XID_TO_SLOT__;
    if (table && rel >= 0 && rel < table.length) table[rel] = 0;
  });

  /* XEvent delivery: Server Worker posts these on our port; we turn
   * around and call the wasm's push_* API via ccall. Module is set by
   * the factory below. Buffer early arrivals until Module is ready. */
  const eventBacklog: ServerToClientXEvent[] = [];
  const deliver = (msg: ServerToClientXEvent): void => {
    const m = globalThis.__EMX11_MODULE__;
    if (!m) { eventBacklog.push(msg); return; }
    routeXEvent(m, msg);
  };
  channel.on<ServerToClientXEvent>('XEvent.Button', deliver);
  channel.on<ServerToClientXEvent>('XEvent.Motion', deliver);
  channel.on<ServerToClientXEvent>('XEvent.Key', deliver);
  channel.on<ServerToClientXEvent>('XEvent.Expose', deliver);
  channel.on<ServerToClientXEvent>('XEvent.MapRequest', deliver);
  channel.on<ServerToClientXEvent>('XEvent.ReparentNotify', deliver);

  /* Root-window value from the BootstrapClient message. The server
   * assigns a stable root xid (typically 1); clients use it for
   * XDefaultRootWindow / XRootWindow. Injected here so `open_display`
   * bridge can echo it back synchronously without an RPC. */
  (async () => {
    /* Ask server for the root window in case main didn't inject it in
     * BootstrapClient (simplification vs. a 7-field bootstrap). This
     * fires once at startup; cheap. */
    try {
      const r = await channel.call<{ rootWindow: number }>({
        kind: 'Display.Open',
      });
      const conn = globalThis.__EMX11_CONN__!;
      conn.rootWindow = r.rootWindow >>> 0;
    } catch (e) {
      console.warn('[emx11:client] Display.Open RPC failed:', e);
    }

    /* Now load the wasm. glue is ESM (EXPORT_ES6=1), so dynamic import
     * resolves its default to the factory. `locateFile` points the
     * factory at the sibling .wasm URL. */
    try {
      const glue = await import(/* @vite-ignore */ data.glueUrl);
      const factory = glue.default as (opts: {
        locateFile?: (p: string) => string;
        arguments?: string[];
      }) => Promise<EmscriptenModule>;
      const module = await factory({
        locateFile: (p) => (p.endsWith('.wasm') ? data.wasmUrl : p),
        ...(data.arguments !== undefined ? { arguments: data.arguments } : {}),
      });
      globalThis.__EMX11_MODULE__ = module;
      /* Flush any XEvents that arrived before Module finished loading. */
      for (const ev of eventBacklog) routeXEvent(module, ev);
      eventBacklog.length = 0;
    } catch (e) {
      console.error('[emx11:client] wasm load failed:', e);
    }
  })();
}

/** Translate a server-originated XEvent message into the corresponding
 *  `emx11_push_*_event` ccall. Mirrors the arg order in event.c /
 *  event_queue.c. */
function routeXEvent(m: EmscriptenModule, msg: ServerToClientXEvent): void {
  switch (msg.kind) {
    case 'XEvent.Button':
      m.ccall(
        'emx11_push_button_event',
        null,
        ['number', 'number', 'number', 'number', 'number', 'number', 'number', 'number'],
        [msg.xType, msg.window, msg.lx, msg.ly, msg.x_root, msg.y_root, msg.button, msg.state],
      );
      break;
    case 'XEvent.Motion':
      m.ccall(
        'emx11_push_motion_event',
        null,
        ['number', 'number', 'number', 'number', 'number', 'number'],
        [msg.window, msg.x, msg.y, msg.x_root, msg.y_root, msg.state],
      );
      break;
    case 'XEvent.Key':
      m.ccall(
        'emx11_push_key_event',
        null,
        ['number', 'number', 'number', 'number', 'number', 'number'],
        [msg.xType, msg.window, msg.keysym, msg.state, msg.x, msg.y],
      );
      break;
    case 'XEvent.Expose':
      m.ccall(
        'emx11_push_expose_event',
        null,
        ['number', 'number', 'number', 'number', 'number'],
        [msg.window, msg.x, msg.y, msg.w, msg.h],
      );
      break;
    case 'XEvent.MapRequest':
      m.ccall(
        'emx11_push_map_request',
        null,
        ['number', 'number'],
        [msg.parent, msg.window],
      );
      break;
    case 'XEvent.ReparentNotify':
      m.ccall(
        'emx11_push_reparent_notify',
        null,
        ['number', 'number', 'number', 'number'],
        [msg.window, msg.parent, msg.x, msg.y],
      );
      break;
  }
}
