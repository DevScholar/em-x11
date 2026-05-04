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
  StagedMemfsFile,
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
  /** Map XID → SAB slot for EVERY window currently mirrored (not just
   *  windows this client created). Server broadcasts Slot.Assigned to
   *  every client so a WM's cross-connection XGetWindowAttributes /
   *  XGetGeometry call resolves to the right slot. A dense Uint32Array
   *  only covers one conn's XID range and would fail on cross-conn
   *  lookups (the twm→xeyes MapRequest path). */
  var __EMX11_XID_TO_SLOT__: Map<number, number> | undefined;
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

  /* Global XID → slot map. Server pushes Slot.Assigned for every
   * window Create (from any conn), Slot.Freed on every Destroy. */
  const xidToSlot = new Map<number, number>();

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
    xidToSlot.set(msg.winId >>> 0, msg.slot);
  });
  channel.on<ServerToClientSlot>('Slot.Freed', (msg) => {
    if (msg.kind !== 'Slot.Freed') return;
    xidToSlot.delete(msg.winId >>> 0);
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
  void (async () => {
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
     * factory at the sibling .wasm URL. Inject a `preRun` hook that
     * stages any files the caller listed in BootstrapClient.stagedFiles
     * (twmrc, app-defaults/*, etc.). */
    try {
      const glue = (await import(/* @vite-ignore */ data.glueUrl)) as {
        default: (opts: {
          locateFile?: (p: string) => string;
          arguments?: string[];
          thisProgram?: string;
          preRun?: ((mod: EmscriptenModule) => void)[];
        }) => Promise<EmscriptenModule>;
      };
      const factory = glue.default;
      const preRunHooks: ((mod: EmscriptenModule) => void)[] = [];
      if (data.stagedFiles && data.stagedFiles.length > 0) {
        preRunHooks.push(makeStagingPreRun(data.stagedFiles));
      }
      const module = await factory({
        locateFile: (p) => (p.endsWith('.wasm') ? data.wasmUrl : p),
        ...(data.arguments !== undefined ? { arguments: data.arguments } : {}),
        ...(data.thisProgram !== undefined ? { thisProgram: data.thisProgram } : {}),
        ...(preRunHooks.length > 0 ? { preRun: preRunHooks } : {}),
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

/** Build a preRun hook that mkdir -p's each parent dir and writes every
 *  file in `files` to MEMFS. */
function makeStagingPreRun(
  files: StagedMemfsFile[],
): (mod: EmscriptenModule) => void {
  return (mod) => {
    const fs = mod.FS;
    if (!fs) {
      throw new Error(
        '[emx11:client] wasm has no FS — was it built without Emscripten ' +
          'filesystem support?',
      );
    }
    const dirs = new Set<string>();
    for (const f of files) {
      const parts = f.path.split('/').filter((p) => p.length > 0);
      for (let i = 1; i < parts.length; i++) {
        dirs.add('/' + parts.slice(0, i).join('/'));
      }
    }
    const ordered = [...dirs].sort((a, b) => a.length - b.length);
    for (const dir of ordered) {
      try {
        fs.mkdir(dir);
      } catch (e) {
        const msg = (e as Error).message ?? '';
        if (!msg.includes('exist') && !msg.includes('EEXIST')) throw e;
      }
    }
    for (const f of files) {
      fs.writeFile(f.path, f.contents);
    }
  };
}
