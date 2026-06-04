/**
 * EmX11 — the public façade returned by createEmX11().
 *
 * Wraps the internal Host and bundles the public surfaces into one
 * object:
 *
 *   emX11.fs              — staging FS manifest (api/fs.ts)
 *   emX11.display         — canvas/root window/input injection (api/display.ts)
 *   emX11.debug           — trace flags + state dumpers (api/debug.ts)
 *   emX11.child_process   — spawn / exec wasm processes (api/child_process.ts)
 *   emX11.dlopen          — pluggable side-module loader (api/dlopen.ts)
 *   emX11._host           — @internal escape hatch, unstable
 *
 * The instance writes the Host to Module['emx11Host'] (flat Module
 * property, per emscripten convention) via attachToBridge(), so the
 * C-side bridges in libemx11 reach it via `Module['emx11Host']`.
 * The public surface (fs, display, child_process, ...) stays on the
 * local instance the caller binds.
 */

import { Host } from '../host/index.js';
import type { HostOptions } from '../host/index.js';
import { cleanupOldCaches, type CacheMode } from '../loader/cache.js';
import { DebugNamespace } from './debug.js';
import { DisplayNamespace } from './display.js';
import { FSNamespace } from './fs.js';
import { ChildProcessNamespace } from './child_process.js';
import { defaultDlopen } from './dlopen.js';
import type {
  CreateEmX11Options,
  DlopenAdapter,
  DlopenOptions,
  LoadedModule,
  EmX11ChildProcess,
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
  readonly child_process: EmX11ChildProcess;
  readonly version = VERSION;

  /** Module overrides to spread into the Emscripten factory call for
   *  single-instance mode. Passes the Host to Module['emx11Host'] and
   *  suppresses the default Host auto-start.
   *
   *    const factory = (await import('./myapp.js')).default;
   *    await factory({ ...x11.moduleOverrides, ...otherOverrides });
   *
   *  For multi-instance mode, use x11.child_process.spawn() instead. */
  readonly moduleOverrides: { emx11Host: Host; emx11NoAutoStart: true };

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

    /* Worker-mode hosts plug a remote in here so XSetICFocus /
     * Tk_SetCaretPos cross over to a DOM textarea on the main thread.
     * Direct (DOM-side) hosts leave this null and TextInputOverlay
     * uses its own textarea. */
    if (options.textInputRemote) {
      this._host.textInput.setRemote(options.textInputRemote);
    }

    this._fs = new FSNamespace();
    this.fs = this._fs;
    this.display = new DisplayNamespace(this._host);
    this.debug = new DebugNamespace(this._host);
    this.moduleOverrides = {
      emx11Host: this._host,
      emx11NoAutoStart: true,
    };

    this._dlopen = options.dlopen ?? defaultDlopen;
    this.defaultStdout = options.stdout ?? ((l) => console.log(l));
    this.defaultStderr = options.stderr ?? ((l) => console.warn(l));
    this.defaultCacheMode = options.loaderCache ?? defaultCacheModeForEnv();

    this.child_process = new ChildProcessNamespace(
      this._host,
      this._fs,
      this.defaultStdout,
      this.defaultStderr,
      this.defaultCacheMode,
    );

    /* Fire-and-forget: prune any leftover caches from older em-x11
     * versions on the same origin. Doesn't block construction; if it
     * runs late, the worst case is one extra entry sitting in storage
     * until the next boot picks it up. */
    void cleanupOldCaches();

    /* attachToBridge() above already wrote the Host to
     * Module['emx11Host'] (and Module['emx11Caches'] /
     * Module['emx11Debug']) so the C-side bridges can reach it.
     * The public surface (fs, display, child_process) stays on the
     * local instance. */
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

  const canvas = opts.canvas ?? opts.resolveCanvas?.();
  if (canvas instanceof OffscreenCanvas) {
    out.surface = canvas;
  } else if (canvas) {
    out.element = canvas;
  }
  return out;
}
