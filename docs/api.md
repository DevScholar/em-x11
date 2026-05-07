# em-x11 JavaScript API

> Pre-alpha. The surface described here is what `createEmX11()` returns
> today. Methods may move or rename until the package hits 0.1.

## Top-level

### `createEmX11(options?) → Promise<EmX11>`

Construct an em-x11 instance. The returned object is also mirrored
onto `globalThis.emX11` so that DevTools (`emX11.debug.dumpWindows()`)
and the C-side EM_JS bridges in `libemx11` share one namespace —
nothing em-x11 puts on the global object lives outside that single
`emX11` slot.

```ts
import { createEmX11 } from '@devscholar/em-x11';

const em = await createEmX11({ canvas: document.getElementById('x') });
const xeyes = em.spawn('/build/artifacts/xeyes/xeyes.js', {
  argv: ['xeyes'],
  thisProgram: 'xeyes',
});
xeyes.on('exit', (code) => console.log('xeyes exited', code));
```

#### `CreateEmX11Options`

| Option   | Type | Notes |
|----------|------|-------|
| `canvas`   | `HTMLCanvasElement` \| `OffscreenCanvas` | Existing canvas to paint into. Auto-created in `document.body` if omitted. |
| `parent`   | `HTMLElement` | DOM parent for an auto-created canvas. |
| `width`    | `number` | Logical width of the X screen. Default 1024. |
| `height`   | `number` | Logical height of the X screen. Default 768. |
| `dlopen`   | `DlopenAdapter` | Pluggable side-module loader for `em.dlopen()`. Required for the Pyodide path; static-archive consumers leave it unset. |
| `stdout`   | `(line: string) => void` | Default sink for spawned processes' Emscripten `print`. Falls back to `console.log`. |
| `stderr`   | `(line: string) => void` | Default sink for `printErr`. Falls back to `console.warn`. |

## `em.fs` — Linux-style staging filesystem

em-x11 is a JS library, so it has no MEMFS of its own. `em.fs` is a
**staging manifest**: every `writeFile` / `mkdir` / `mount` call
records an entry, and each spawned process replays the manifest into
its own MEMFS during the Emscripten preRun hook. Stage your X11 base
layout (twmrc, app-defaults, font config, library tarballs) once at
boot and every wasm child sees the same `/usr`, `/etc`, `/opt`.

Default mounts at construction:

| Path | Type | Notes |
|------|------|-------|
| `/tmp` `/usr` `/etc` `/opt` `/var` | MEMFS | manifest only |
| `/home` | MEMFS | IDBFS-backed persistence is a future extension |

```ts
em.fs.writeFile(path, data: string | Uint8Array): void
em.fs.readFile(path): Uint8Array | null            // staging manifest, not a live FS
em.fs.mkdir(path, { recursive? }): void
em.fs.readdir(path): string[]
em.fs.exists(path): boolean
em.fs.rm(path, { recursive? }): void
em.fs.mount(spec: MountSpec): Promise<void>
```

`MountSpec` accepts:

```ts
{ type: 'memfs', target: string }
{ type: 'idbfs', target: string }
{ type: 'tar',   source: string | ArrayBuffer | Uint8Array, target: string }
```

`tar` mounts decompress eagerly into the manifest (POSIX ustar only,
no gzip — pre-extract gzipped tars at the call site if needed). To
write into a process's *live* FS after it has booted, use
`process.fs.writeFile(...)`.

## `em.spawn(programUrl, options?) → Process`

Node-style `child_process.spawn`. Returns a `Process` synchronously
even though the underlying wasm load is async — boot completion
surfaces via `process.ready` and the `'exit'` / `'error'` events.

```ts
em.spawn(programUrl, { argv?, thisProgram?, wasmUrl?, stdout?, stderr?, preRun?, factory? })
em.exec(programUrl, opts?): Promise<{ code: number }>   // spawn + wait
```

### Program URL — extension handling

Emscripten compiles every executable to a `.js` loader **plus** a
`.wasm` binary that always sit side by side. To match Linux
mental model (`exec("/usr/bin/xeyes")` is one path, not two),
`em.spawn` accepts the path **without an extension** and resolves
both ends itself:

```ts
em.spawn('/build/artifacts/xeyes/xeyes')         // → xeyes.js + xeyes.wasm
em.spawn('/build/artifacts/xeyes/xeyes.js')      // explicit glue, wasm derived
em.spawn('/build/artifacts/xeyes/xeyes.wasm')    // explicit wasm, glue derived
```

Pass `wasmUrl` only when the binary lives at a different path than
its glue (rare).

> **Shipping your own program for em-x11:** Emscripten always emits
> the `.js` + `.wasm` pair (and sometimes a `.data` blob from
> `--preload-file`). Both files must be served side by side. The
> `.js` is a ~80 KB loader that wires up FS / syscall stubs / Asyncify
> — analogous to a Linux ELF's PT_INTERP pointing at `ld-linux.so`.
> You can't run an Emscripten executable without it.

### `SpawnOptions`

| Option | Type | Notes |
|--------|------|-------|
| `argv`        | `string[]` | argv excluding argv[0]. |
| `thisProgram` | `string` | argv[0] / WM_CLASS / Xt application name. Most callers set this — Emscripten's default `./this.program` makes Xt apps surface as "this.program" in their `WM_NAME`. Defaults to the program URL's basename with `.js` / `.wasm` stripped. |
| `wasmUrl`     | `string` | Inferred from the program URL. Set this only when the binary lives at a different path than its glue. |
| `stdout` `stderr` | `(line) => void` | Per-process sinks; fall back to factory-level sinks. |
| `preRun`      | `((mod) => void)[]` | Extra Emscripten preRun hooks fired AFTER em.fs replay but BEFORE main(). |

### `Process`

```ts
interface Process {
  readonly pid: number;
  readonly argv: string[];
  readonly thisProgram: string;
  readonly ready: Promise<void>;          // resolves once the wasm has booted
  readonly fs: ProcessFS;                 // live MEMFS view (after ready)
  readonly module: Promise<EmscriptenModule>;
  wait(): Promise<{ code: number }>;
  kill(): void;
  on(event: 'exit',  cb: (code: number) => void): void;
  on(event: 'error', cb: (err: Error)   => void): void;
  off(event: 'exit' | 'error', cb): void;
}
```

`kill()` closes the process's display connection and drops its
windows. The wasm module itself can't be force-unloaded in a browser;
its code stays linked, only the X resources go away.

## `em.display`

```ts
em.display.canvas: HTMLCanvasElement | OffscreenCanvas
em.display.width: number
em.display.height: number
em.display.rootWindowId: number
em.display.waitForSubstructureRedirect(winId, timeoutMs?): Promise<number>
```

Compositing harnesses (`session` demo, wacl-tk, pyodide-tk) await
`waitForSubstructureRedirect(em.display.rootWindowId)` between
spawning a window manager (twm) and spawning the first managed
client, so that the WM's `MapRequest` intercept is armed before the
client tries to map.

### `em.display.inject` — public input injection

Worker-mode hosts (pyodide-tk) relay raw DOM events from the main
thread and call these in the worker to drive the X event pipeline.
DOM-mode demos do not need to call `inject` at all — the input
bridge attaches its own listeners.

```ts
em.display.inject.mouseDown({ x, y, button, modifiers })
em.display.inject.mouseUp(...)
em.display.inject.mouseMove({ x, y, modifiers })
em.display.inject.keyDown({ keysym, modifiers, hasFocus? })
em.display.inject.keyUp(...)
em.display.inject.setPointer(x, y)
```

## `em.debug` — toggleable trace flags + state dumpers

```ts
em.debug.traceHit       // findWindowAt log; spammy on Motion
em.debug.traceHitNext   // one-shot — log only the very next hit-test
em.debug.traceMotion    // JS input-bridge motion log
em.debug.traceButton    // JS input-bridge button log
em.debug.tracePaint     // Renderer paint walk log
em.debug.traceCBtn      // C-side button event delivery (event.c)
em.debug.traceCMot      // C-side motion event delivery
em.debug.traceMove      // XMoveWindow / XConfigureWindow
em.debug.traceQp        // XQueryPointer from Xaw shims

em.debug.dumpWindows()  // print every mapped window's bbox/shape/clip state
em.debug.dumpGrabs()    // print every registered passive button grab
```

All flags are writable and read every tick; toggle them live from
DevTools (`emX11.debug.traceHit = true`).

## `em.dlopen(soPath, options?) → Promise<LoadedModule>`

Pluggable side-module loader. em-x11 itself doesn't ship a dynamic
linker — Emscripten's loader is tied to the Module that hosts the
side modules, and Pyodide already has a battle-tested `loadDynlib`
with NEEDED auto-cascade. Pass an adapter to `createEmX11`:

```ts
const em = await createEmX11({
  dlopen: async (soPath) => {
    await py._api.loadDynlib(soPath, { global: true });
    const exports = py._module.LDSO.loadedLibsByName[soPath].exports;
    return { soPath, exports };
  },
});
await em.dlopen('/usr/lib/libXft.so');
```

Without an adapter `em.dlopen()` throws.

## Internal escape hatch

`em._host` exposes the underlying `Host` (`@internal`, unstable).
Use it only for things the public API doesn't cover yet. Today, the
pyodide-tk worker reaches through it to call
`em._host.connection.setDefaultModule(moduleSurface)` — the binding
that says "this Pyodide-loaded `libemx11.so` is the wasm side of the
next `XOpenDisplay`". A public method for that path will land
alongside a richer `em.dlopen` flow.

## The global mirror

`globalThis.emX11` carries the full instance plus three implementation
slots that the C side reads:

| Slot | Owner | Purpose |
|------|-------|---------|
| `emX11._bridge` | `Host.attachToBridge()` | Facade EM_JS bodies dispatch into. |
| `emX11._caches` | bridges themselves | Lazy scratchpads (font measure ctx, font/text caches, property stash). |
| `emX11._debug`  | `attachToBridge()` + `em.debug` setters | Backing store for trace flags. |

Underscore-prefixed slots are private implementation detail; touch
them only via the typed `em.*` surface above.
