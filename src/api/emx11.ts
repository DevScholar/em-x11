/**
 * EmX11 — the public façade returned by createEmX11().
 *
 * Wraps the internal Host and bundles the public surfaces into one
 * object:
 *
 *   em.fs        — staging FS manifest (api/fs.ts)
 *   em.display   — canvas/root window/input injection (api/display.ts)
 *   em.debug     — trace flags + state dumpers (api/debug.ts)
 *   em.spawn     — spawn a wasm process (api/process.ts)
 *   em.exec      — spawn + wait
 *   em.dlopen    — pluggable side-module loader (api/dlopen.ts)
 *   em._host     — @internal escape hatch, unstable
 *
 * The constructor ALSO mirrors every public surface onto
 * `globalThis.emX11` so DevTools and the EM_JS bridges all see one
 * shared namespace. `_bridge` / `_caches` / `_debug` come along for
 * the ride from Host.attachToBridge().
 */

import { Host } from '../host/index.js';
import type { HostOptions } from '../host/index.js';
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

    /* Mirror the public surface onto globalThis.emX11 so DevTools
     * (`emX11.debug.dumpWindows()`) and the EM_JS bridges all read
     * one object. attachToBridge has already created the slot; we
     * just decorate it. */
    const slot = globalThis.emX11!;
    slot.fs = this.fs;
    slot.display = this.display;
    slot.debug = this.debug;
    slot.spawn = this.spawn.bind(this);
    slot.exec = this.exec.bind(this);
    slot.dlopen = this.dlopen.bind(this);
    slot.version = this.version;
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
