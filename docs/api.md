# em-x11 JavaScript API

> Pre-alpha. The surface described here is what `createEmX11()` returns
> today. Methods may move or rename until the package hits 0.1.

## Top-level

### `createEmX11(options?) → Promise<EmX11>`

Construct an em-x11 instance. The returned object is **not** auto-published
on `globalThis`; bind it to whatever variable name you like. (em-x11 does
reserve the `globalThis.emX11` slot for a small set of C-side ABI hooks —
see [The `globalThis.emX11` slot](#the-globalthisemx11-slot) below.)

```ts
import { createEmX11 } from '@devscholar/em-x11';

const emX11 = await createEmX11({ canvas: document.getElementById('x') });
const xeyes = emX11.spawn('/build/artifacts/xeyes/xeyes.js', {
  argv: ['xeyes'],
  thisProgram: 'xeyes',
});
xeyes.on('exit', (code) => console.log('xeyes exited', code));
```

> If you want DevTools access, attach the instance yourself:
> `globalThis.app = emX11`.

#### `CreateEmX11Options`

| Option   | Type | Notes |
|----------|------|-------|
| `canvas`   | `HTMLCanvasElement` \| `OffscreenCanvas` | Existing canvas to paint into. Auto-created in `document.body` if omitted. |
| `parent`   | `HTMLElement` | DOM parent for an auto-created canvas. |
| `width`    | `number` | Logical width of the X screen. Default 1024. |
| `height`   | `number` | Logical height of the X screen. Default 768. |
| `dlopen`   | `DlopenAdapter` | Pluggable side-module loader for `emX11.dlopen()`. Required for the Pyodide path; static-archive consumers leave it unset. |
| `stdout`   | `(line: string) => void` | Default sink for spawned processes' Emscripten `print`. Falls back to `console.log`. |
| `stderr`   | `(line: string) => void` | Default sink for `printErr`. Falls back to `console.warn`. |
| `loaderCache` | `'use' \| 'bypass' \| 'refresh'` | Cache Storage policy for `emX11.spawn`'s `.js` glue and `.wasm` binary fetches. Default `'bypass'` in Vite dev mode, `'use'` (cache-first) otherwise. See [Caching](#caching). |

## `emX11.fs` — Linux-style staging filesystem

em-x11 is a JS library, so it has no MEMFS of its own. `emX11.fs` is a
**staging manifest**: every `writeFileSync` / `mkdirSync` / `mount` call
records an entry, and each spawned process replays the manifest into
its own MEMFS during the Emscripten preRun hook. Stage your X11 base
layout (twmrc, app-defaults, font config, library tarballs) once at
boot and every wasm child sees the same `/usr`, `/etc`, `/opt`.

### Why the `Sync` suffix?

The manifest is a JS-side `Map<string, bytes>`, so writes / reads / dir
ops finish synchronously without ever crossing into a MEMFS. We use
the `*Sync` suffix to match Node's `fs` convention — Node's
`fs.writeFile` / `fs.readFile` / `fs.mkdir` are **asynchronous**, and
the synchronous variants carry the `Sync` suffix. Naming our sync
methods after the async ones would mislead anyone arriving with the
Node mental model. `mount()` is genuinely async (it may `fetch` a tar)
and keeps a name without the suffix.

Default mounts at construction:

| Path | Type | Notes |
|------|------|-------|
| `/tmp` `/usr` `/etc` `/opt` `/var` | MEMFS | manifest only |
| `/home` | MEMFS | IDBFS-backed persistence is a future extension |

```ts
emX11.fs.writeFileSync(path, data: string | Uint8Array): void
emX11.fs.readFileSync(path): Uint8Array | null            // staging manifest, not a live FS
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

`tar` mounts decompress eagerly into the manifest (POSIX ustar only,
no gzip — pre-extract gzipped tars at the call site if needed). To
write into a process's *live* FS after it has booted, use
`process.fs.writeFileSync(...)`.

## `emX11.spawn(programUrl, options?) → Process`

Node-style `child_process.spawn`. Returns a `Process` synchronously
even though the underlying wasm load is async — boot completion
surfaces via `process.ready` and the `'exit'` / `'error'` events.

```ts
emX11.spawn(programUrl, { argv?, thisProgram?, wasmUrl?, stdout?, stderr?, preRun?, factory? })
emX11.exec(programUrl, opts?): Promise<{ code: number }>   // spawn + wait
```

### Program URL — extension handling

Emscripten compiles every executable to a `.js` loader **plus** a
`.wasm` binary that always sit side by side. To match Linux
mental model (`exec("/usr/bin/xeyes")` is one path, not two),
`emX11.spawn` accepts the path **without an extension** and resolves
both ends itself:

```ts
emX11.spawn('/build/artifacts/xeyes/xeyes')         // → xeyes.js + xeyes.wasm
emX11.spawn('/build/artifacts/xeyes/xeyes.js')      // explicit glue, wasm derived
emX11.spawn('/build/artifacts/xeyes/xeyes.wasm')    // explicit wasm, glue derived
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
| `preRun`      | `((mod) => void)[]` | Extra Emscripten preRun hooks fired AFTER emX11.fs replay but BEFORE main(). |

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

interface ProcessFS {
  writeFileSync(path: string, data: string | Uint8Array): void;
  readFileSync(path: string): Uint8Array;
  mkdirSync(path: string): void;
}
```

`kill()` closes the process's display connection and drops its
windows. The wasm module itself can't be force-unloaded in a browser;
its code stays linked, only the X resources go away.

## `emX11.display`

```ts
emX11.display.canvas: HTMLCanvasElement | OffscreenCanvas
emX11.display.width: number
emX11.display.height: number
emX11.display.rootWindowId: number
emX11.display.waitForSubstructureRedirect(winId, timeoutMs?): Promise<number>
```

Compositing harnesses (`session` demo, wacl-tk, pyodide-tk) await
`waitForSubstructureRedirect(emX11.display.rootWindowId)` between
spawning a window manager (twm) and spawning the first managed
client, so that the WM's `MapRequest` intercept is armed before the
client tries to map.

### `emX11.display.inject` — public input injection

Worker-mode hosts (pyodide-tk) relay raw DOM events from the main
thread and call these in the worker to drive the X event pipeline.
DOM-mode demos do not need to call `inject` at all — the input
bridge attaches its own listeners.

```ts
emX11.display.inject.mouseDown({ x, y, button, modifiers })
emX11.display.inject.mouseUp(...)
emX11.display.inject.mouseMove({ x, y, modifiers })
emX11.display.inject.keyDown({ keysym, modifiers, hasFocus? })
emX11.display.inject.keyUp(...)
emX11.display.inject.setPointer(x, y)
```

## `emX11.debug` — toggleable trace flags + state dumpers

```ts
emX11.debug.traceHit       // findWindowAt log; spammy on Motion
emX11.debug.traceHitNext   // one-shot — log only the very next hit-test
emX11.debug.traceMotion    // JS input-bridge motion log
emX11.debug.traceButton    // JS input-bridge button log
emX11.debug.tracePaint     // Renderer paint walk log
emX11.debug.traceCBtn      // C-side button event delivery (event.c)
emX11.debug.traceCMot      // C-side motion event delivery
emX11.debug.traceMove      // XMoveWindow / XConfigureWindow
emX11.debug.traceQp        // XQueryPointer from Xaw shims

emX11.debug.dumpWindows()  // print every mapped window's bbox/shape/clip state
emX11.debug.dumpGrabs()    // print every registered passive button grab
```

All flags are writable and read every tick; toggle them live from
DevTools — but only if you've published the instance yourself, e.g.
`globalThis.app = emX11; app.debug.traceHit = true`.

## `emX11.dlopen(soPath, options?) → Promise<LoadedModule>`

Pluggable side-module loader. em-x11 itself doesn't ship a dynamic
linker — Emscripten's loader is tied to the Module that hosts the
side modules, and Pyodide already has a battle-tested `loadDynlib`
with NEEDED auto-cascade. Pass an adapter to `createEmX11`:

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

## Caching

`emX11.spawn()` fetches a `.js` glue and a `.wasm` binary every time;
on the second visit those bytes don't have to come from the network.
em-x11 routes both fetches through the
[Cache Storage API](https://developer.mozilla.org/docs/Web/API/Cache),
keyed by URL, under the cache name `em-x11-loader-v1`.

| Mode      | Behaviour |
|-----------|-----------|
| `'use'`     | Cache-first. Hit → use cached bytes; miss → fetch + populate cache. **Default in production.** |
| `'bypass'`  | Skip Cache Storage; plain `fetch`. **Default in Vite dev mode** (`import.meta.env.DEV`) so artifact rebuilds are picked up immediately. |
| `'refresh'` | Force a fresh fetch and overwrite the cache entry. Use after deploying new artifacts. |

The cache name is versioned (`-v1`); when em-x11 bumps the loader
ABI the version changes and stale caches from older releases are
deleted at boot. Manual invalidation is one DevTools line:

```js
await caches.delete('em-x11-loader-v1');
```

If `globalThis.caches` is unavailable (Cache Storage requires a
secure context — `https://` or `localhost`), the loader silently
falls back to plain `fetch` regardless of `loaderCache`.

## Internal escape hatch

`emX11._host` exposes the underlying `Host` (`@internal`, unstable).
Use it only for things the public API doesn't cover yet. Today, the
pyodide-tk worker reaches through it to call
`emX11._host.connection.setDefaultModule(moduleSurface)` — the binding
that says "this Pyodide-loaded `libemx11.so` is the wasm side of the
next `XOpenDisplay`". A public method for that path will land
alongside a richer `emX11.dlopen` flow.

## The `globalThis.emX11` slot

em-x11 reserves a single global property — `globalThis.emX11` —
for three implementation slots that the C side of `libemx11` reads
synchronously from EM_JS bodies. **The instance returned by
`createEmX11()` is not published here.** That's a deliberate change
from earlier pre-releases: callers bind the instance to whatever
local they like and choose for themselves whether to expose it on
`globalThis` for DevTools.

| Slot | Owner | Purpose |
|------|-------|---------|
| `globalThis.emX11._bridge` | `Host.attachToBridge()` | Facade EM_JS bodies dispatch into. |
| `globalThis.emX11._caches` | bridges themselves | Lazy scratchpads (font measure ctx, font/text caches, property stash). |
| `globalThis.emX11._debug`  | `attachToBridge()` + `emX11.debug` setters | Backing store for trace flags. |

Underscore-prefixed slots are private implementation detail; do not
reach into them. To poke trace flags from DevTools, publish your own
instance: `globalThis.app = emX11; app.debug.traceHit = true`.
