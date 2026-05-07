/**
 * EmX11 — the public façade returned by createEmX11().
 *
 * Wraps the internal Host and bundles the public surfaces into one
 * object:
 *
 *   emX11.fs        — staging FS manifest (api/fs.ts)
 *   emX11.display   — canvas/root window/input injection (api/display.ts)
 *   emX11.debug     — trace flags + state dumpers (api/debug.ts)
 *   emX11.spawn     — spawn a wasm process (api/process.ts)
 *   emX11.exec      — spawn + wait
 *   emX11.dlopen    — pluggable side-module loader (api/dlopen.ts)
 *   emX11._host     — @internal escape hatch, unstable
 *
 * The instance is NOT auto-published on globalThis. Host.attachToBridge()
 * does claim `globalThis.emX11._bridge` / `._caches` / `._debug` because
 * the C-side EM_JS bodies in libemx11 read them synchronously, but the
 * public surface (fs, display, spawn, ...) stays on the local instance
 * the caller binds. Callers who want DevTools access publish it
 * themselves: `globalThis.app = emX11`.
 */

import { Host } from '../host/index.js';
import type { HostOptions } from '../host/index.js';
import { cleanupOldCaches, type CacheMode } from '../loader/cache.js';
import { DebugNamespace } from './debug.js';
import { DisplayNamespace } from './display.js';
import { FSNamespace } from './fs.js';
import { ProcessImpl } from './process.js';
import { defaultDlopen } from './dlopen.js';
import type {
  CreateEmX11Options,
  DlopenAdapter,
  DlopenOptions,
  LoadedModule,
  Process,
  SpawnOptions,
  EmX11Debug,
  EmX11Display,
  EmX11FS,
} from './types.js';

export const VERSION = '0.0.1';

/** Resolve the default loader cache mode based on the build environment.
 *  In Vite dev mode (`import.meta.env.DEV`), default to `'bypass'` so
 *  rebuilt artifacts under `/build/artifacts/...` are picked up
 *  immediately. In production / preview / test builds, default to
 *  `'use'` (cache-first). Callers can always override per-instance via
 *  `createEmX11({ loaderCache })` or per-call via `SpawnOptions.cacheMode`. */
function defaultCacheModeForEnv(): CacheMode {
  /* import.meta.env is a Vite-only object. Guard so this module still
   * works in non-Vite consumers (raw bundlers, Node, vitest). */
  const env = (import.meta as unknown as { env?: { DEV?: boolean } }).env;
  return env?.DEV ? 'bypass' : 'use';
}

export class EmX11 {
  readonly fs: EmX11FS;
  readonly display: EmX11Display;
  readonly debug: EmX11Debug;
  readonly version = VERSION;

  /** @internal Escape hatch onto the internal Host. Surface unstable;
   *  consumers should migrate every reach-through to a public API as
   *  it becomes available. */
  readonly _host: Host;

  private readonly _fs: FSNamespace;
  private readonly _dlopen: DlopenAdapter;
  private readonly defaultStdout: (line: string) => void;
  private readonly defaultStderr: (line: string) => void;
  private readonly defaultCacheMode: CacheMode;

  constructor(options: CreateEmX11Options = {}) {
    const hostOptions: HostOptions = canvasOptions(options);
    this._host = new Host(hostOptions);
    this._host.attachToBridge();

    this._fs = new FSNamespace();
    this.fs = this._fs;
    this.display = new DisplayNamespace(this._host);
    this.debug = new DebugNamespace(this._host);

    this._dlopen = options.dlopen ?? defaultDlopen;
    this.defaultStdout = options.stdout ?? ((l) => console.log(l));
    this.defaultStderr = options.stderr ?? ((l) => console.warn(l));
    this.defaultCacheMode = options.loaderCache ?? defaultCacheModeForEnv();

    /* Fire-and-forget: prune any leftover caches from older em-x11
     * versions on the same origin. Doesn't block construction; if it
     * runs late, the worst case is one extra entry sitting in storage
     * until the next boot picks it up. */
    void cleanupOldCaches();

    /* Note: we deliberately do NOT mirror the public surface onto
     * globalThis.emX11. attachToBridge() above already populated the
     * private `_bridge` / `_caches` / `_debug` slots that EM_JS bodies
     * in libemx11 read synchronously; that is the entire global ABI.
     * Callers who want DevTools access publish the instance under
     * their own name (`globalThis.app = emX11`). */
  }

  /** Spawn a wasm process. Returns synchronously; await `process.ready`
   *  for boot completion, or use the Node-style event API
   *  (`process.on('exit', ...)`). */
  spawn(glueUrl: string, options: SpawnOptions = {}): Process {
    return new ProcessImpl(
      this._host,
      this._fs,
      glueUrl,
      options,
      this.defaultStdout,
      this.defaultStderr,
      this.defaultCacheMode,
    );
  }

  /** spawn + wait. Resolves with the exit code. */
  async exec(glueUrl: string, options: SpawnOptions = {}): Promise<{ code: number }> {
    const p = this.spawn(glueUrl, options);
    await p.ready;
    return p.wait();
  }

  /** Load a side module via the configured DlopenAdapter. Throws if
   *  no adapter was supplied to createEmX11. */
  dlopen(soPath: string, options?: DlopenOptions): Promise<LoadedModule> {
    return this._dlopen(soPath, options);
  }
}

export function createEmX11(options: CreateEmX11Options = {}): Promise<EmX11> {
  /* Async signature for forward-compat with future async setup
   * (IDBFS init, font preloading). The current implementation does
   * no awaits, but consumers must `await createEmX11(...)` so the
   * future change isn't a breaking one. */
  return Promise.resolve(new EmX11(options));
}

function canvasOptions(opts: CreateEmX11Options): HostOptions {
  const out: HostOptions = {};
  if (opts.parent !== undefined) out.parent = opts.parent;
  if (opts.width !== undefined) out.width = opts.width;
  if (opts.height !== undefined) out.height = opts.height;
  if (opts.canvas instanceof OffscreenCanvas) {
    out.surface = opts.canvas;
  } else if (opts.canvas) {
    out.element = opts.canvas;
  }
  return out;
}
