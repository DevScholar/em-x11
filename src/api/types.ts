/**
 * Public types for the em-x11 npm surface.
 *
 * Everything a consumer touches via `initEmX11()` or `createEmX11()`
 * is typed here. The internal Host / manager classes under src/host
 * are deliberately NOT re-exported; EmX11Session._host and
 * EmX11._host are typed-but-@internal escape hatches for callers
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
  /** When `canvas` is not provided, called to resolve the canvas at
   *  construction time. pyodide-tk bridges `pyodide.canvas.getCanvas2D()`
   *  through this so the Pyodide canvas API is the single source of truth.
   *  If both `canvas` and `resolveCanvas` are set, `canvas` wins. */
  resolveCanvas?: () => HTMLCanvasElement | OffscreenCanvas | undefined;
  /** DOM parent for an auto-created canvas. Ignored if `canvas` is
   *  provided or running in a worker. */
  parent?: HTMLElement;
  /** Logical (CSS) width of the X screen. Default 1024. */
  width?: number;
  /** Logical (CSS) height of the X screen. Default 768. */
  height?: number;
  /** Optional dynamic-loader adapter. Required for `em.dlopen()`;
   *  pyodide-tk supplies a wrapper around `loadDynlib` here. tcldide's
   *  static-archive build leaves it unset. */
  dlopen?: DlopenAdapter;
  /** Default stdout sink for processes spawned without their own
   *  stdout option. Each Module's `print` is routed here line-by-line.
   *  If unset, lines go to `console.log`. */
  stdout?: (line: string) => void;
  /** Default stderr sink (Module `printErr`). Defaults to
   *  `console.warn`. */
  stderr?: (line: string) => void;
  /** Cache Storage policy for `emX11.child_process.spawn`'s `.js` glue
   *  and `.wasm` binary fetches. Default is `'use'` (cache-first) in
   *  production and `'bypass'` in Vite dev mode (`import.meta.env.DEV`),
   *  so artifact rebuilds are picked up immediately during development
   *  without needing to manually clear the cache.
   *
   *  - `'use'`     — cache-first; populate on miss
   *  - `'bypass'`  — never touch Cache Storage; plain fetch
   *  - `'refresh'` — force a fetch and overwrite the cache entry
   *
   *  Cache lives under the name `em-x11-loader`. Manual reset:
   *  `await caches.delete('em-x11-loader')` from DevTools. */
  loaderCache?: 'use' | 'bypass' | 'refresh';
  /** Plug a transport into the XIM textarea overlay. Set this in
   *  worker-mode hosts (pyodide-tk) so XSetICFocus / Tk_SetCaretPos
   *  reach a DOM textarea on the main thread. The host computes
   *  window-tree absolute caret pixels and passes them to
   *  `positionHint` so the remote can position the textarea without
   *  shadowing the X window tree.
   *
   *  Pair with `createDomTextInputBridge` on the main thread. Leave
   *  unset for the in-process path -- TextInputOverlay then creates
   *  its own textarea automatically. */
  textInputRemote?: TextInputRemoteHandle;
}

/** Worker-side handle the host calls when XSetICFocus / Tk_SetCaretPos
 *  fires. The implementation typically posts a message to the main
 *  thread, where createDomTextInputBridge applies it to a real
 *  textarea. */
export interface TextInputRemoteHandle {
  setFocus(window: number): void;
  clearFocus(): void;
  setSpot(window: number, x: number, y: number): void;
  /** Root-relative absolute caret pixel position computed by the host.
   *  The remote uses this to position its textarea so the OS IME
   *  candidate window anchors near the X widget caret. */
  positionHint(absX: number, absY: number): void;
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
   *  preRun. For an already-running process, use `process.fs.writeFileSync`
   *  to write into its live FS instead.
   *
   *  Note the `Sync` suffix: this manifest is a JS-side `Map`, so
   *  the call returns synchronously. The suffix mirrors Node's `fs`
   *  convention where `writeFile` is async and `writeFileSync` is sync. */
  writeFileSync(path: string, data: string | Uint8Array): void;
  /** Read a previously staged file. Returns null if not present in the
   *  staging manifest (this method does NOT reach into a running
   *  process's MEMFS). */
  readFileSync(path: string): Uint8Array | null;
  mkdirSync(path: string, opts?: { recursive?: boolean }): void;
  readdirSync(path: string): string[];
  existsSync(path: string): boolean;
  rmSync(path: string, opts?: { recursive?: boolean }): void;
  /** Establish a mount in the staging manifest. The default mounts
   *  (`/tmp`, `/usr`, `/etc`, `/opt`, `/var`, `/home`) already exist;
   *  callers typically only invoke this for tar archives. Async
   *  because tar mounts may `fetch()` their source. */
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
  /** Physical key (evdev keycode from KeyboardEvent.code via
   *  KEYCODE_EVDEV table). Stable across keyboard layouts. Pass 0 for
   *  synthetic injections that don't correspond to a real key. */
  keycode: number;
  modifiers: number;
  hasFocus?: boolean;
  /** UTF-8 string the browser produced for this key. Plain ASCII typing
   *  fills this from KeyboardEvent.key (length-1 char); non-printable
   *  keys, modifiers, and IME 'Process' events leave it empty.
   *  Forwarded into emx11_set_pending_key_text so Xutf8LookupString
   *  returns the typed bytes for Tk's tkUnixKey.c handler. */
  text?: string;
}

export interface InjectWheelEvent {
  x: number;
  y: number;
  deltaY: number;
  modifiers: number;
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
    /** Synthetic text-only KeyPress, paired with a KeyRelease. Used
     *  for IME composition results and clipboard paste -- anything
     *  carrying text but no useful keysym. Worker-mode hosts forward
     *  composition results from the main-thread textarea bridge here. */
    textKey(text: string): void;
    wheel(e: InjectWheelEvent): void;
  };
  /** Plug the Tk/Tcl event-loop wake target into em-x11's notifier.
   *  libemx11 forwards the standardised Tcl_SetNotifier setTimerProc
   *  + alertNotifierProc signals here; hosts implement them on top
   *  of whatever scheduling primitive they have (setTimeout in the
   *  pyodide-tk worker, rAF in tcldide main thread, Atomics.notify
   *  if cross-thread).
   *
   *  Without an installed wake, libemx11's bridges are silent no-ops
   *  -- callers without a pump (test harnesses, demos that never
   *  enter Tk's event loop) don't need to install anything.
   *
   *  Pass null to uninstall (e.g. before disposing the host). */
  installEventLoopWake(wake: EventLoopWake | null): void;
  /** Keyboard layout introspection + OS-key capture. See
   *  src/host/keyboard-layout.ts and src/host/keyboard-lock.ts. */
  readonly keyboard: EmX11Keyboard;
}

/** Layout-aware keyboard surface. Two independent capabilities:
 *
 *   - getLayoutMap(): the user's current physical-key labels (so apps
 *     can draw a virtual keyboard, configure a "press a key to remap"
 *     UI, or just confirm what AZERTY-Q produces). Wraps
 *     navigator.keyboard.getLayoutMap() with graceful fallback to an
 *     empty Map on non-Chromium browsers.
 *
 *   - lock: capture OS-level shortcut keys (Alt+Tab, Super, Esc) while
 *     in fullscreen. Off by default; demos opt in. Auto-engages on
 *     fullscreenchange.
 */
export interface EmX11Keyboard {
  /** Resolves to KeyboardEvent.code -> label string (e.g. "KeyQ" -> "a"
   *  on a French AZERTY user's keyboard). Empty map on browsers without
   *  the Keyboard API (Firefox, Safari, mobile). Cached after first
   *  call; pass forceRefresh to invalidate. */
  getLayoutMap(forceRefresh?: boolean): Promise<Map<string, string>>;
  /** OS-key capture controls. Opt-in; off until enable() is called.
   *  Requires the page to enter fullscreen for the lock to actually
   *  take effect (Web Keyboard Lock security model). */
  readonly lock: EmX11KeyboardLock;
}

export interface EmX11KeyboardLock {
  /** Engage on the next fullscreen entry (or right now if already in
   *  fullscreen). Pass `keys` to whitelist KeyboardEvent.code values;
   *  empty/omitted captures every available key. */
  enable(opts?: { keys?: string[] }): void;
  /** Stop auto-engaging on fullscreen + release the lock if currently
   *  held. Safe to call when not enabled. */
  disable(): void;
  /** True when navigator.keyboard.lock is available in this browser. */
  isAvailable(): boolean;
  /** True when the OS lock is currently held. */
  isLocked(): boolean;
}

/** Wake target a host installs into em-x11 so the standardised Tcl
 *  notifier ABI (setTimerProc / alertNotifierProc) reaches the host's
 *  event-loop scheduler. */
export interface EventLoopWake {
  /** Schedule the next pump tick at +ms relative. `ms < 0` means
   *  "no timer scheduled" (Tcl had no pending after-events). `ms === 0`
   *  means "fire as soon as possible". The host should cancel any
   *  previous timer first; this is an absolute set, not a chain. */
  onTimer(ms: number): void;
  /** Wake the pump now, regardless of the current timer schedule.
   *  Equivalent to Tcl_AlertNotifier on Linux. */
  onAlert(): void;
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

/* -- child_process ------------------------------------------------------- */

export interface EmX11ChildProcess {
  /** Node-style spawn. Returns synchronously; await `process.ready` for
   *  boot completion, or listen via `process.on('exit', ...)`. */
  spawn(glueUrl: string, options?: SpawnOptions): Process;
  /** spawn + wait. Resolves with the exit code. */
  exec(glueUrl: string, options?: SpawnOptions): Promise<{ code: number }>;
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
  /** Per-spawn override of the factory-level `loaderCache` policy.
   *  Useful for forcing one program's bytes to be re-downloaded
   *  (`'refresh'`) without changing the default for the rest of the
   *  session. */
  cacheMode?: 'use' | 'bypass' | 'refresh';
}

/** Subset of Emscripten FS exposed on Process for live-FS interaction
 *  with a running wasm. Sync-suffixed for the same reason as EmX11FS:
 *  these are synchronous calls, and Node's unsuffixed names are async. */
export interface ProcessFS {
  writeFileSync(path: string, data: string | Uint8Array): void;
  readFileSync(path: string): Uint8Array;
  mkdirSync(path: string): void;
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
