/**
 * Process — Node-style child handle around a wasm + its connection.
 *
 * Returned synchronously from `em.spawn()`; boot completion surfaces
 * via `process.ready` and the `'exit'` / `'error'` events. Modeled on
 * Node's `child_process.spawn`:
 *
 *   - `pid` mirrors Node's process pid (we use connId, since one
 *     wasm Module always opens exactly one display in our world).
 *   - `stdout` / `stderr` are routed through user-supplied callbacks
 *     instead of being live streams. ReadableStream<string> is the
 *     plan eventually but a callback is what every existing consumer
 *     (tcldide, demos) actually needs, so we keep the surface small.
 *   - `kill()` closes the display and tears down its windows. The
 *     wasm Module itself can't be force-unloaded in a browser; its
 *     code stays linked, only the X resources go away.
 */

import type { Host } from '../host/index.js';
import type { ConnectionManager } from '../host/connection.js';
import type {
  EmscriptenModule,
  EmscriptenModuleFactory,
} from '../types/emscripten.js';
import type { CacheMode } from '../loader/cache.js';
import type { FSNamespace } from './fs.js';
import type { Process, ProcessFS, SpawnOptions } from './types.js';

type ExitCb = (code: number) => void;
type ErrorCb = (err: Error) => void;

export class ProcessImpl implements Process {
  readonly argv: string[];
  readonly thisProgram: string;
  readonly ready: Promise<void>;

  private _pid = 0;
  private _module: EmscriptenModule | null = null;
  private readonly bootPromise: Promise<{ connId: number; module: EmscriptenModule }>;
  private exitCode: number | null = null;
  private exitError: Error | null = null;
  private readonly exitListeners: ExitCb[] = [];
  private readonly errorListeners: ErrorCb[] = [];
  private readonly waiters: ((code: number) => void)[] = [];

  constructor(
    private readonly host: Host,
    fs: FSNamespace,
    glueUrl: string,
    opts: SpawnOptions,
    defaultStdout: (line: string) => void,
    defaultStderr: (line: string) => void,
    defaultCacheMode: CacheMode,
  ) {
    this.argv = opts.argv ?? [];
    this.thisProgram = opts.thisProgram ?? deriveProgName(glueUrl);

    const { glueUrl: resolvedGlue, wasmUrl: resolvedWasm } = resolveProgramUrls(
      glueUrl,
      opts.wasmUrl,
    );
    const stdout = opts.stdout ?? defaultStdout;
    const stderr = opts.stderr ?? defaultStderr;
    const fsPreRun = fs.buildPreRun();
    const userPreRun = opts.preRun ?? [];
    const cacheMode: CacheMode = opts.cacheMode ?? defaultCacheMode;

    /* Wrap launchClient so we can capture the Module, install stdio
     * routes, and resolve `ready`. The host's connection.launchClient
     * already does the load+attach dance; we layer Process semantics
     * on top. */
    const launchOpts: LaunchProcessOptions = {
      glueUrl: resolvedGlue,
      wasmUrl: resolvedWasm,
      arguments: this.argv,
      thisProgram: this.thisProgram,
      cacheMode,
      print: stdout,
      printErr: stderr,
      preRun: [fsPreRun, ...userPreRun],
    };
    if (opts.factory !== undefined) launchOpts.factory = opts.factory;
    this.bootPromise = launchProcess(this.host.connection, launchOpts);

    this.ready = this.bootPromise.then(
      ({ connId, module }) => {
        this._pid = connId;
        this._module = module;
      },
      (err: unknown) => {
        this.exitError = coerceError(err);
        for (const cb of this.errorListeners) cb(this.exitError);
        throw this.exitError;
      },
    );
  }

  get pid(): number {
    return this._pid;
  }

  get module(): Promise<EmscriptenModule> {
    return this.bootPromise.then(({ module }) => module);
  }

  get fs(): ProcessFS {
    return {
      writeFileSync: (path, data) => {
        const m = this.assertModule();
        m.FS!.writeFile(path, data);
      },
      readFileSync: (path) => {
        const m = this.assertModule();
        const data = m.FS!.readFile(path, { encoding: 'binary' });
        return data as Uint8Array;
      },
      mkdirSync: (path) => {
        const m = this.assertModule();
        m.FS!.mkdir(path);
      },
    };
  }

  wait(): Promise<{ code: number }> {
    if (this.exitCode !== null) return Promise.resolve({ code: this.exitCode });
    return new Promise((resolve) => {
      this.waiters.push((code) => resolve({ code }));
    });
  }

  kill(): void {
    if (this.exitCode !== null) return;
    if (this._pid !== 0) {
      this.host.connection.close(this._pid);
    }
    this.signalExit(0);
  }

  on(event: 'exit', cb: (code: number) => void): void;
  on(event: 'error', cb: (err: Error) => void): void;
  on(event: 'exit' | 'error', cb: (...args: never[]) => void): void {
    if (event === 'exit') {
      this.exitListeners.push(cb as ExitCb);
      if (this.exitCode !== null) (cb as ExitCb)(this.exitCode);
    } else {
      this.errorListeners.push(cb as ErrorCb);
      if (this.exitError) (cb as ErrorCb)(this.exitError);
    }
  }

  off(event: 'exit' | 'error', cb: (...args: never[]) => void): void {
    const list = event === 'exit' ? this.exitListeners : this.errorListeners;
    const idx = list.indexOf(cb as ExitCb & ErrorCb);
    if (idx >= 0) list.splice(idx, 1);
  }

  private assertModule(): EmscriptenModule {
    if (!this._module) {
      throw new Error('em-x11: process.fs / process.module accessed before await ready');
    }
    return this._module;
  }

  private signalExit(code: number): void {
    if (this.exitCode !== null) return;
    this.exitCode = code;
    for (const cb of this.exitListeners) cb(code);
    for (const w of this.waiters) w(code);
    this.waiters.length = 0;
  }
}

function deriveProgName(programUrl: string): string {
  const slash = programUrl.lastIndexOf('/');
  const base = slash >= 0 ? programUrl.slice(slash + 1) : programUrl;
  return base.replace(/\.(js|wasm)$/, '');
}

/** Resolve a program URL into the (.js glue, .wasm binary) pair Emscripten
 *  needs. Accepts three input shapes so callers can match their mental
 *  model — Linux-style extensionless ("/usr/bin/xeyes"), explicit glue
 *  (".js"), or wasm-first (".wasm"). The wasm-first form is rare but we
 *  accept it because the artifact pair always lives side-by-side.
 *
 *  An explicit `wasmUrl` always wins; we only synthesize when omitted. */
function resolveProgramUrls(
  programUrl: string,
  explicitWasmUrl: string | undefined,
): { glueUrl: string; wasmUrl: string } {
  let glueUrl: string;
  let wasmUrl: string;
  if (programUrl.endsWith('.js')) {
    glueUrl = programUrl;
    wasmUrl = programUrl.slice(0, -3) + '.wasm';
  } else if (programUrl.endsWith('.wasm')) {
    glueUrl = programUrl.slice(0, -5) + '.js';
    wasmUrl = programUrl;
  } else {
    glueUrl = programUrl + '.js';
    wasmUrl = programUrl + '.wasm';
  }
  if (explicitWasmUrl !== undefined) wasmUrl = explicitWasmUrl;
  return { glueUrl, wasmUrl };
}

/** Coerce arbitrary thrown values into an Error with a readable
 *  message. Emscripten's ErrnoError stringifies as "[object Object]"
 *  via plain String(); we surface its `.errno` and `.message`
 *  instead. Generic objects fall back to JSON.stringify so the
 *  console message is at least inspectable. */
function coerceError(err: unknown): Error {
  if (err instanceof Error) return err;
  if (err && typeof err === 'object') {
    const o = err as { errno?: number; message?: string; node?: unknown };
    if (typeof o.errno === 'number') {
      return new Error(`Emscripten FS errno=${o.errno} (${o.message ?? 'FS error'})`);
    }
    try {
      return new Error(JSON.stringify(err));
    } catch {
      return new Error(Object.prototype.toString.call(err));
    }
  }
  return new Error(String(err));
}

interface LaunchProcessOptions {
  glueUrl: string;
  wasmUrl: string;
  arguments: string[];
  thisProgram: string;
  preRun: ((mod: EmscriptenModule) => void)[];
  cacheMode: CacheMode;
  print: (line: string) => void;
  printErr: (line: string) => void;
  factory?: EmscriptenModuleFactory;
}

/** Connection manager.launchClient + an optional factory override. We
 *  duplicate enough of loadWasm here to honour `opts.factory`; in the
 *  default path we delegate. */
async function launchProcess(
  connection: ConnectionManager,
  opts: LaunchProcessOptions,
): Promise<{ connId: number; module: EmscriptenModule }> {
  if (!opts.factory) {
    return connection.launchClient({
      glueUrl: opts.glueUrl,
      wasmUrl: opts.wasmUrl,
      arguments: opts.arguments,
      thisProgram: opts.thisProgram,
      preRun: opts.preRun,
      cacheMode: opts.cacheMode,
      print: opts.print,
      printErr: opts.printErr,
    });
  }
  /* Factory-override path: skip the dynamic import and pass our own
   * factory directly to a thin wrapper that reuses the connection
   * manager's pendingLaunch slot. Not exercised yet by any consumer;
   * the surface exists so test harnesses can stub. */
  throw new Error('em-x11: SpawnOptions.factory is not implemented yet');
}
