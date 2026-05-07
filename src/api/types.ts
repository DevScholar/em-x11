/**
 * Public types for the em-x11 npm surface.
 *
 * Everything a consumer touches via `createEmX11()` (or the
 * `globalThis.emX11` mirror) is typed here. The internal Host /
 * manager classes under src/host are deliberately NOT re-exported;
 * EmX11._host is a typed-but-@internal escape hatch for callers
 * still mid-migration.
 */

import type {
  EmscriptenModule,
  EmscriptenModuleFactory,
} from '../types/emscripten.js';

/* -- factory ------------------------------------------------------------- */

export interface CreateEmX11Options {
  /** Existing canvas to paint into. If omitted and we're in a DOM
   *  context, em-x11 creates one and appends it to `parent` (default
   *  `document.body`). */
  canvas?: HTMLCanvasElement | OffscreenCanvas;
  /** DOM parent for an auto-created canvas. Ignored if `canvas` is
   *  provided or running in a worker. */
  parent?: HTMLElement;
  /** Logical (CSS) width of the X screen. Default 1024. */
  width?: number;
  /** Logical (CSS) height of the X screen. Default 768. */
  height?: number;
  /** Optional dynamic-loader adapter. Required for `em.dlopen()`;
   *  pyodide-tk supplies a wrapper around `loadDynlib` here. wacl-tk's
   *  static-archive build leaves it unset. */
  dlopen?: DlopenAdapter;
  /** Default stdout sink for processes spawned without their own
   *  stdout option. Each Module's `print` is routed here line-by-line.
   *  If unset, lines go to `console.log`. */
  stdout?: (line: string) => void;
  /** Default stderr sink (Module `printErr`). Defaults to
   *  `console.warn`. */
  stderr?: (line: string) => void;
}

/* -- fs ------------------------------------------------------------------ */

export type MountSpec =
  | { type: 'memfs'; target: string }
  | { type: 'idbfs'; target: string }
  /** Decompress a POSIX ustar archive into the staging manifest, then
   *  replay every regular file into each spawned process's MEMFS. */
  | { type: 'tar'; source: string | ArrayBuffer | Uint8Array; target: string };

export interface EmX11FS {
  /** Stage a file. Replayed into every future spawn's MEMFS during
   *  preRun. For an already-running process, use `process.fs.writeFile`
   *  to write into its live FS instead. */
  writeFile(path: string, data: string | Uint8Array): void;
  /** Read a previously staged file. Returns null if not present in the
   *  staging manifest (this method does NOT reach into a running
   *  process's MEMFS). */
  readFile(path: string): Uint8Array | null;
  mkdir(path: string, opts?: { recursive?: boolean }): void;
  readdir(path: string): string[];
  exists(path: string): boolean;
  rm(path: string, opts?: { recursive?: boolean }): void;
  /** Establish a mount in the staging manifest. The default mounts
   *  (`/tmp`, `/usr`, `/etc`, `/opt`, `/var`, `/home`) already exist;
   *  callers typically only invoke this for tar archives. */
  mount(spec: MountSpec): Promise<void>;
}

/* -- display ------------------------------------------------------------- */

export interface InjectMouseEvent {
  x: number;
  y: number;
  button: number;
  modifiers: number;
}

export interface InjectKeyEvent {
  keysym: number;
  modifiers: number;
  hasFocus?: boolean;
}

export interface EmX11Display {
  readonly canvas: HTMLCanvasElement | OffscreenCanvas;
  readonly width: number;
  readonly height: number;
  readonly rootWindowId: number;
  /** Resolves once a SubstructureRedirectMask has landed on `winId`
   *  (typically the root window, set by twm). Demos that compose a
   *  WM with managed clients must `await` this between spawning the
   *  WM and spawning the first managed client. */
  waitForSubstructureRedirect(winId: number, timeoutMs?: number): Promise<number>;
  /** Public input-injection API. Worker-mode hosts (pyodide-tk) relay
   *  raw DOM events from the main thread and call these to drive the
   *  X event pipeline. */
  readonly inject: {
    mouseDown(e: InjectMouseEvent): void;
    mouseUp(e: InjectMouseEvent): void;
    mouseMove(e: Omit<InjectMouseEvent, 'button'>): void;
    keyDown(e: InjectKeyEvent): void;
    keyUp(e: InjectKeyEvent): void;
    setPointer(x: number, y: number): void;
  };
}

/* -- debug --------------------------------------------------------------- */

export interface EmX11Debug {
  /** Live trace flags. Read on every tick by the relevant subsystem;
   *  toggle from DevTools (`emX11.debug.traceHit = true`) for live
   *  diagnostics. */
  traceHit: boolean;
  traceHitNext: boolean;
  traceMotion: boolean;
  traceButton: boolean;
  tracePaint: boolean;
  traceCBtn: boolean;
  traceCMot: boolean;
  traceMove: boolean;
  traceQp: boolean;
  /** Print every mapped window's bbox/shape/clipList state in z-order. */
  dumpWindows(): void;
  /** Print every registered passive button grab. */
  dumpGrabs(): void;
}

/* -- spawn / process ----------------------------------------------------- */

export interface SpawnOptions {
  /** argv excluding argv[0]. */
  argv?: string[];
  /** argv[0] / WM_CLASS / Xt application name. Emscripten's default
   *  `./this.program` makes Xt apps surface as "this.program" in their
   *  WM_NAME, so most callers want to set this explicitly. Defaults
   *  to the program URL's basename with `.js` / `.wasm` stripped. */
  thisProgram?: string;
  /** Override the .wasm URL. Inferred from the program URL by
   *  appending `.wasm` (extensionless input) or substituting
   *  `.js` → `.wasm` (`.js` input). Set this only when the binary
   *  lives at a different path than its glue. */
  wasmUrl?: string;
  /** Per-process stdout sink (lines from Emscripten `print`). Falls
   *  back to the factory-level `stdout` option, then `console.log`. */
  stdout?: (line: string) => void;
  /** Per-process stderr sink (lines from Emscripten `printErr`). */
  stderr?: (line: string) => void;
  /** Extra preRun hooks fired after em.fs replay but before main(). */
  preRun?: ((mod: EmscriptenModule) => void)[];
  /** Override the Emscripten module factory. Use this to bypass the
   *  HTTP loader entirely (test harnesses, prebundled glue). */
  factory?: EmscriptenModuleFactory;
}

/** Subset of Emscripten FS exposed on Process for live-FS interaction
 *  with a running wasm. */
export interface ProcessFS {
  writeFile(path: string, data: string | Uint8Array): void;
  readFile(path: string): Uint8Array;
  mkdir(path: string): void;
}

/** Node-style child process handle. Returned synchronously from
 *  `em.spawn` even though the underlying wasm load is async — boot
 *  progress surfaces via the `ready` promise and the `'exit'` /
 *  `'error'` events. */
export interface Process {
  readonly pid: number;
  readonly argv: string[];
  readonly thisProgram: string;
  /** Resolves once the wasm module has finished booting and the
   *  process's connection is bound. Reject if loading fails. */
  readonly ready: Promise<void>;
  /** Live MEMFS view of the running process. Only valid after
   *  `await ready`. */
  readonly fs: ProcessFS;
  /** Underlying Emscripten module. Escape hatch for cwrap'd entry
   *  points. Only valid after `await ready`. */
  readonly module: Promise<EmscriptenModule>;
  /** Wait for the process to exit. Resolves with the exit code. */
  wait(): Promise<{ code: number }>;
  /** Tear down the process: close its display connection, drop its
   *  windows, fire `'exit'`. The wasm itself can't be force-unloaded
   *  in a browser, but its X resources can. */
  kill(): void;
  on(event: 'exit', cb: (code: number) => void): void;
  on(event: 'error', cb: (err: Error) => void): void;
  off(event: 'exit' | 'error', cb: (...args: unknown[]) => void): void;
}

/* -- dlopen -------------------------------------------------------------- */

export interface DlopenOptions {
  /** Symbol scope: 'global' makes exports visible to subsequently-loaded
   *  modules (mirrors RTLD_GLOBAL). Default 'global'. */
  scope?: 'global' | 'local';
}

export interface LoadedModule {
  /** Path the module was loaded from. */
  soPath: string;
  /** Exported symbol table. Shape depends on the underlying loader. */
  exports: Record<string, (...args: unknown[]) => unknown>;
}

export type DlopenAdapter = (
  soPath: string,
  opts?: DlopenOptions,
) => Promise<LoadedModule>;
