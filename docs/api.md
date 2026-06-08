# em-x11 JavaScript API

> Pre-alpha. The surface described here is what the package exports today.
> Methods may move or rename until the package hits 0.1.

## Two API layers

em-x11 follows Emscripten conventions — the same `-sUSE_*` flag pattern
as `-sUSE_SDL=2`. Pick the layer that fits:

| Layer | Entry point | When |
|-------|------------|------|
| 1 — zero JS | `emcc myapp.c -sUSE_EMX11 -o myapp.html` | Simple Xlib program; the port auto-injects the JS library and a default Host. No user JS required. Pass `thisProgram`, `emx11Width`/`emx11Height`, and `locateFile` as Module properties. |
| 2 — createEmX11 | `createEmX11()` | Multi-process sessions, JS-side asset staging via `emX11.fs`, custom canvas/display, dlopen adapter. |

Layer 1 covers every single-program example (hello, xeyes, glxgears, xt-hello, xcalc). Layer 2 is for multi-process use-cases like twm-session and the Pyodide integration.

## Top-level exports

```ts
import {
  createEmX11,
  EmX11,
  VERSION,

  // Types
  type CreateEmX11Options,
  type EmX11Debug,
  type EmX11Display,
  type EmX11FS,
  type Process,
  type ProcessFS,
  type SpawnOptions,
  type DlopenAdapter,
  type DlopenOptions,
  type LoadedModule,
  type InjectKeyEvent,
  type InjectMouseEvent,
  type MountSpec,
  type TextInputRemoteHandle,

  // IME cross-thread bridge
  createDomTextInputBridge,
} from '@devscholar/em-x11';
```

### `createEmX11(options?) → Promise<EmX11>`

Creates a Host, attaches it to `Module['emx11Host']`, and returns the
full surface. Two usage modes from one function:

**Single-program mode (Layer 1 — zero JS)** — the default Host IIFE
auto-creates a canvas; just import and call the Emscripten factory:

```html
<script type="module">
  const factory = (await import('/artifacts/hello/hello.js')).default;
  await factory();
</script>
```

For custom dimensions, pass `emx11Width` / `emx11Height`:

```html
<script type="module">
  const factory = (await import('/artifacts/xcalc/xcalc.js')).default;
  await factory({
    thisProgram: 'xcalc',
    emx11Width: 800,
    emx11Height: 600,
    locateFile: (path) => `/artifacts/xcalc/${path}`,
  });
</script>
```

**Single-program mode (Layer 2 — createEmX11)** — spread `moduleOverrides`
into the Emscripten factory for JS-side control:

```ts
import { createEmX11 } from '@devscholar/em-x11';

const x11 = await createEmX11({ width: 1024, height: 768 });

const factory = (await import('/artifacts/hello/hello.js')).default;
await factory({ ...x11.moduleOverrides });
```

For programs that need assets staged into MEMFS before `main()` runs,
use Emscripten's `--preload-file` at build time. The glue loads the
`.data` package automatically — no JS-side `preRun` hook needed.

In CMake, pass the file to `emx11_finalize_demo` via `PRELOAD_FILES`:

```cmake
emx11_finalize_demo(xcalc
    EXPORT_NAME createXcalcModule
    LIBS Xaw Xmu Xt Xpm Xext X11
    PRELOAD_FILES "${XCALC_SRC_DIR}/app-defaults/XCalc@/usr/lib/X11/app-defaults/XCalc"
)
```

The JS side stays minimal — same pattern as hello, plus `locateFile`:

```html
<script type="module">
  const factory = (await import('/artifacts/xcalc/xcalc.js')).default;
  await factory({
    thisProgram: 'xcalc',
    emx11Width: 800,
    emx11Height: 600,
    locateFile: (path) => `/artifacts/xcalc/${path}`,
  });
</script>
```

**Multi-process mode** — use `child_process.spawn()` for multiple X clients
(twm + xeyes + xcalc sharing one display):

```ts
import { createEmX11 } from '@devscholar/em-x11';

const emX11 = await createEmX11({ width: 1024, height: 768 });
const xeyes = emX11.child_process.spawn('/artifacts/xeyes/xeyes', {
  thisProgram: 'xeyes',
});
await xeyes.ready;
```

#### `CreateEmX11Options`

| Option | Type | Notes |
|--------|------|-------|
| `canvas`   | `HTMLCanvasElement \| OffscreenCanvas` | Existing canvas to paint into. Auto-created in `document.body` if omitted. |
| `parent`   | `HTMLElement` | DOM parent for an auto-created canvas. |
| `width`    | `number` | Logical width of the X screen. Default 1024. |
| `height`   | `number` | Logical height of the X screen. Default 768. |
| `dlopen`   | `DlopenAdapter` | Pluggable side-module loader for `emX11.dlopen()`. Required for the Pyodide path. |
| `stdout`   | `(line: string) => void` | Default sink for spawned processes' `print`. Falls back to `console.log`. |
| `stderr`   | `(line: string) => void` | Default sink for `printErr`. Falls back to `console.warn`. |
| `loaderCache` | `'use' \| 'bypass' \| 'refresh'` | Cache Storage policy for `.wasm` binary fetches. Default `'bypass'` in Vite dev mode, `'use'` otherwise. |

## `EmX11` instance

```ts
interface EmX11 {
  readonly fs: EmX11FS;
  readonly display: EmX11Display;
  readonly debug: EmX11Debug;
  readonly child_process: EmX11ChildProcess;
  readonly version: string;
  /** Spread into the Emscripten factory call for single-program mode.
   *  Sets Module['emx11Host'] and suppresses the default Host auto-start. */
  readonly moduleOverrides: { emx11Host: Host; emx11NoAutoStart: true };
  readonly _host: Host;  // @internal
  dlopen(soPath: string, options?: DlopenOptions): Promise<LoadedModule>;
}
```

`EmX11` is the single public façade returned by `createEmX11()`.

## `emX11.fs` — Linux-style staging filesystem

`emX11.fs` is a **staging manifest**: every `writeFileSync` / `mkdirSync`
/ `mount` call records an entry, and each spawned process replays the
manifest into its own MEMFS during the Emscripten `preRun` hook. Stage
shared files once at boot and every wasm child sees the same `/usr`,
`/etc`, `/opt`.

For single-program asset staging, use Emscripten's `--preload-file` at build
time — see the `createEmX11` section above.

Default mounts at construction:

| Path | Type | Notes |
|------|------|-------|
| `/tmp` `/usr` `/etc` `/opt` `/var` | MEMFS | manifest only |
| `/home` | MEMFS | IDBFS-backed persistence is a future extension |

```ts
emX11.fs.writeFileSync(path, data: string | Uint8Array): void
emX11.fs.readFileSync(path): Uint8Array | null
emX11.fs.mkdirSync(path, { recursive? }): void
emX11.fs.readdirSync(path): string[]
emX11.fs.existsSync(path): boolean
emX11.fs.rmSync(path, { recursive? }): void
emX11.fs.mount(spec: MountSpec): Promise<void>
```

`MountSpec` accepts:

```ts
{ type: 'memfs', target: string }
{ type: 'idbfs', target: string }
{ type: 'tar',   source: string | ArrayBuffer | Uint8Array, target: string }
```

`tar` mounts decompress eagerly into the manifest (POSIX ustar only, no
gzip — pre-extract gzipped tars at the call site if needed). To write
into a process's *live* FS after it has booted, use
`process.fs.writeFileSync(...)`.

## `emX11.child_process` — spawn and exec wasm processes

```ts
emX11.child_process.spawn(programUrl, opts?): Process
emX11.child_process.exec(programUrl, opts?): Promise<{ code: number }>
```

`spawn` returns a `Process` synchronously; boot completion surfaces via
`process.ready` and the `'exit'` / `'error'` events. `exec` is spawn +
wait.

### Program URL — extension handling

Emscripten compiles every executable to a `.js` loader **plus** a
`.wasm` binary side by side. `spawn` accepts the path **without an
extension** and resolves both ends:

```ts
emX11.child_process.spawn('/artifacts/xeyes/xeyes')       // → xeyes.js + xeyes.wasm
emX11.child_process.spawn('/artifacts/xeyes/xeyes.js')    // explicit glue, wasm derived
emX11.child_process.spawn('/artifacts/xeyes/xeyes.wasm')  // explicit wasm, glue derived
```

> The `.js` glue is an ~80 KB loader that wires up FS / syscall stubs /
> JSPI — analogous to a Linux ELF's PT_INTERP pointing at
> `ld-linux.so`. You can't run an Emscripten executable without it.

### `SpawnOptions`

| Option | Type | Notes |
|--------|------|-------|
| `argv`        | `string[]` | argv excluding argv[0]. |
| `thisProgram` | `string` | argv[0] / WM_CLASS / Xt application name. Defaults to the program URL's basename. |
| `wasmUrl`     | `string` | Inferred from the program URL. Set only when the binary lives at a different path. |
| `stdout` `stderr` | `(line) => void` | Per-process sinks; fall back to factory-level sinks. |
| `preRun`      | `((mod) => void)[]` | Extra preRun hooks fired AFTER `emX11.fs` replay but BEFORE `main()`. |

### `Process`

```ts
interface Process {
  readonly pid: number;
  readonly argv: string[];
  readonly thisProgram: string;
  readonly ready: Promise<void>;
  readonly fs: ProcessFS;                 // live MEMFS view (after ready)
  readonly module: Promise<EmscriptenModule>;
  wait(): Promise<{ code: number }>;
  kill(): void;
  on(event: 'exit',  cb: (code: number) => void): void;
  on(event: 'error', cb: (err: Error)   => void): void;
  off(event: 'exit' | 'error', cb): void;
}

interface ProcessFS {
  writeFileSync(path: string, data: string | Uint8Array): void;
  readFileSync(path: string): Uint8Array;
  mkdirSync(path: string): void;
}
```

`kill()` closes the process's display connection and drops its windows.
The wasm module itself can't be force-unloaded in a browser.

## `x11.display`

```ts
display.canvas: HTMLCanvasElement | OffscreenCanvas
display.width: number
display.height: number
display.rootWindowId: number
display.waitForSubstructureRedirect(winId, timeoutMs?): Promise<number>
```

`waitForSubstructureRedirect` is used by multi-client harnesses
(twm-session) to wait for the WM to arm `SubstructureRedirectMask`
before spawning managed clients.

### `display.inject` — public input injection

Worker-mode hosts (pyodide-tk) relay raw DOM events from the main thread
and call these to drive the X event pipeline. DOM-mode programs do not
need `inject` — the input bridge attaches its own listeners.

```ts
display.inject.mouseDown({ x, y, button, modifiers })
display.inject.mouseUp(...)
display.inject.mouseMove({ x, y, modifiers })
display.inject.keyDown({ keysym, modifiers, hasFocus? })
display.inject.keyUp(...)
display.inject.setPointer(x, y)
```

## `x11.debug`

```ts
debug.traceHit       // findWindowAt log; spammy on Motion
debug.traceHitNext   // one-shot — log only the very next hit-test
debug.traceMotion    // JS input-bridge motion log
debug.traceButton    // JS input-bridge button log
debug.tracePaint     // Renderer paint walk log
debug.traceCBtn      // C-side button event delivery (event.c)
debug.traceCMot      // C-side motion event delivery
debug.traceMove      // XMoveWindow / XConfigureWindow
debug.traceQp        // XQueryPointer from Xaw shims

debug.dumpWindows()  // print every mapped window's bbox/shape/clip state
debug.dumpGrabs()    // print every registered passive button grab
```

All flags are writable and read every tick. Toggle them live from
DevTools after publishing the instance:

```ts
globalThis.app = await createEmX11({ canvas });
app.debug.traceHit = true;
```

## `emX11.dlopen(soPath, options?) → Promise<LoadedModule>`

Pluggable side-module loader. Pass an adapter to `createEmX11`:

```ts
const emX11 = await createEmX11({
  dlopen: async (soPath) => {
    await py._api.loadDynlib(soPath, { global: true });
    const exports = py._module.LDSO.loadedLibsByName[soPath].exports;
    return { soPath, exports };
  },
});
await emX11.dlopen('/usr/lib/libXft.so');
```

Without an adapter `emX11.dlopen()` throws.

## The `Module['emx11Host']` slot

em-x11 uses a flat Module property — `Module['emx11Host']` — to pass
the Host from JS into the C-side bridges. This follows the Emscripten
convention of using `Module['...']` for configuration (like
`Module['canvas']` or `Module['preRun']`).

| Slot | Set by | Purpose |
|------|--------|---------|
| `Module['emx11Host']` | `createEmX11()` via `moduleOverrides` or `attachToBridge()` | The Host object; every EM_JS bridge dispatches into it. |
| `Module['emx11Caches']` | `attachToBridge()` | Lazy scratchpads (font measure ctx, property stashes). |
| `Module['emx11Debug']` | `attachToBridge()` + debug setters | Backing store for trace flags. Mirrored into the `$EmX11Host` closure. |
| `Module['emx11NoAutoStart']` | Caller (via `moduleOverrides`) | When `true`, suppresses the default Host auto-creation in Layer 1 mode. Set automatically by `createEmX11`. |

The JS library (`library_emx11.js`) reads these during `$EmX11Host.init()`,
which fires at Emscripten startup. When `Module['emx11Host']` is set
(Layer 2), it uses the caller's Host. When absent and
`emx11NoAutoStart` is unset (Layer 1), the default Host IIFE
(`EmX11DefaultHost.create(Module)`) auto-creates one.

## Caching

`emX11.child_process.spawn()` fetches a `.wasm` binary every spawn; on
the second visit those bytes don't have to come from the network. em-x11
routes wasm fetches through the
[Cache Storage API](https://developer.mozilla.org/docs/Web/API/Cache),
keyed by URL, under the cache name `em-x11-loader`.

| Mode | Behaviour |
|------|-----------|
| `'use'`     | Cache-first. Hit → use cached bytes; miss → fetch + populate cache. **Default in production.** |
| `'bypass'`  | Skip Cache Storage; plain `fetch`. **Default in Vite dev mode.** |
| `'refresh'` | Force a fresh fetch and overwrite the cache entry. |

Manual invalidation:

```js
await caches.delete('em-x11-loader');
```

If `globalThis.caches` is unavailable (Cache Storage requires a secure
context — `https://` or `localhost`), the loader silently falls back to
plain `fetch` regardless of `loaderCache`.

## Internal escape hatch

`EmX11._host` exposes the underlying `Host` (`@internal`, unstable).
Use only for things the public API doesn't cover yet.

## Build artifacts — split archives (mirrors real X)

em-x11 produces a set of static archives that match real X's library
split, so consumers link with the standard flags:

```
-lX11 -lXext -lXrender -lXft -lfontconfig -lGLX
```

| Archive | Real X analogue | Contents |
|---------|----------------|----------|
| `libX11.a` | `libX11.so` | Xlib core, Xrm, XIM, bridges, notifier, events, windows, GC, pixmaps, cursors, XKB |
| `libXext.a` | `libXext.so` | SHAPE extension |
| `libXrender.a` | `libXrender.so` | RENDER extension |
| `libfontconfig.a` | `libfontconfig.so` | fontconfig shim |
| `libXft.a` | `libXft.so` | Xft (client-side font rendering) |
| `libGLX.a` | `libGLX.so` | GLX 1.4 (WebGL-backed) |

The archives are compiled with **`-fPIC`**. This is the same requirement
real X applies to its shared-client-libraries (`libX11.so`, `libXext.so`,
…), not to client programs. The flag belongs on the library, not on
programs that link against it. It is load-bearing for consumers that
repackage the archives into an Emscripten side module:

```makefile
# pyodide-tk pattern: repackage split archives into one .so
emcc -sSIDE_MODULE=1 -o libemx11.so \
    -Wl,--whole-archive libX11.a libXext.a libXrender.a libXft.a libfontconfig.a \
    -Wl,--no-whole-archive libtcl8.6.so
```

Without `-fPIC`, wasm-ld rejects `R_WASM_MEMORY_ADDR_*` relocations
when creating a side module — non-PIC objects cannot form a shared
library on any platform.

The parallel build also produces a single `libemx11.so` side module
(`cmake -DEMX11_BUILD_SIDE_MODULE=ON`) for consumers that prefer a
pre-linked artifact over the `--whole-archive` repackaging pattern.
