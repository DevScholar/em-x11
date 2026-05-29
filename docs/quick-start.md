# Quick Start: porting xcalc to em-x11

This walks through the whole process of taking an upstream X.Org client —
[`xcalc`](https://gitlab.freedesktop.org/xorg/app/xcalc) — and getting it
to run in a browser tab on top of em-x11. It is written for an X
developer who is comfortable with Xlib/Xt/Xaw and `autoconf`, but does
**not** assume you know modern frontend tooling. Where a JavaScript
concept matters, it is explained in X terms.

By the end you will have a `pnpm dev` page that displays an xcalc
window, paints buttons, and responds to clicks — the same xcalc binary
you would build on a Linux desktop, just compiled to WebAssembly and
talking to a Canvas instead of an X server.

## 0. Mental model: where everything lives

When you build for a real X server, the world looks like this:

```
xcalc → libXaw → libXt → libX11 → /tmp/.X11-unix/X0 socket → Xorg → screen
```

In em-x11 there is no socket and no Xorg. Instead:

```
xcalc.wasm  →  libXaw  →  libXt  →  libX11 (em-x11)  →  EM_JS bridges  →  TS host  →  <canvas>
```

A few things follow from that:

- `libX11` is replaced by [`native/`](../native/) — our Emscripten
  reimplementation. It exposes the same Xlib symbols (`XOpenDisplay`,
  `XCreateWindow`, `XSendEvent`, …) but the bottom half calls into
  JavaScript via `EM_JS` instead of writing X protocol bytes to a
  socket.
- The "X server" is a TypeScript host living in [`src/`](../src/). It
  owns a `<canvas>`, manages a window tree, and pushes synthesised
  events back into wasm.
- Each X client is its own Emscripten module (`xcalc.js` +
  `xcalc.wasm`), loaded as a standard `MODULARIZE=1 + EXPORT_ES6=1`
  factory. The Host is passed via `Module['emx11Host']` — a flat Module
  property, per Emscripten convention.

### Three API layers

em-x11 follows Emscripten conventions — the same `-sUSE_*` flag pattern
as `-sUSE_SDL=2`. Pick the layer that fits:

| Layer | How | When |
|-------|-----|------|
| 1 — zero JS | `emcc myapp.c -sUSE_EMX11 -o myapp.html` | Simple Xlib program; the port auto-injects everything |
| 2 — single program | `initEmX11()` + spread `moduleOverrides` into the factory | You want JS-side control (canvas, dimensions, asset staging) but only one wasm program |
| 3 — multi-process | `createEmX11()` + `child_process.spawn()` | Multiple X clients sharing one display, window manager sessions |

This tutorial uses **Layer 2** for xcalc: we need JS-side control over
the canvas and dimensions. The `app-defaults/XCalc` file that Xt needs
is embedded at build time via Emscripten's `--preload-file` — the glue
loads the `.data` package automatically before `main()` runs, so no
JS-side `preRun` hook is needed.

Only the twm-session demo ([examples/twm-session/](../examples/twm-session/))
uses Layer 3. Every other example (hello, xeyes, xt-hello, glxgears,
xcalc) uses Layer 2.

## 1. Prerequisites

You need a Linux environment (WSL is fine, in fact em-x11 is developed
on WSL). Install:

- The Emscripten SDK (`emcc`, `emcmake` on `PATH`)
- Node.js ≥ 20 and pnpm ≥ 9
- `cmake` ≥ 3.20, `make`, `git`, `curl`, `tar`, `patch`

> Frontend translation: `pnpm` is the package manager (think `apt` for
> Node libraries), `vite` is the dev server (think `python -m http.server`
> with hot reload), and `node_modules/` is the equivalent of
> `/usr/lib/node_modules` but pinned per project.

Clone the repo, then from the project root:

```bash
pnpm install
```

`pnpm install` runs `scripts/fetch-third-party.sh` automatically via
a `postinstall` hook. The script downloads the X.Org tarballs we depend
on (libXt, libXaw, libXmu, libXpm) and the apps we ship as examples
(xeyes, xcalc, twm, xclock) into `ignored-area/third-party/`. That
directory is gitignored — it is fully reproducible from the script.

## 2. The em-x11 port

em-x11 ships an [emscripten-ports](https://emscripten.org/docs/compiling/Projects.html#embuilder-and-emscripten-ports)
script at [`tools/ports/emx11.py`](../tools/ports/emx11.py). Link
against it the same way you link against SDL2 or other emscripten
ports:

```bash
# Preferred: -sUSE_EMX11 (emscripten convention)
emcc myapp.c -sUSE_EMX11 -o myapp.html

# Also works: explicit port path
emcc myapp.c --use-port=emx11 -o myapp.html
```

The port handles everything — include paths, static archives, the JS
bridge library (`--js-library`), and the default Host IIFE
(`--pre-js`) for Layer 1 (zero-JS) mode. You write standard Xlib code
and emcc handles the rest.

For CMake projects, use `find_package`:

```cmake
find_package(emx11 REQUIRED)
target_link_libraries(myapp PRIVATE emx11::emx11)
```

For a minimal end-to-end example see
[examples/hello/](../examples/hello/) — a 30-line Xlib program that
opens a window and draws a rectangle. With Layer 1 it builds with no
JS: `emcc hello.c -sUSE_EMX11 -o hello.html`.

## 3. The demo's `CMakeLists.txt`

Each X client gets its own subdirectory under `examples/`. For xcalc, that
is [examples/xcalc/CMakeLists.txt](../examples/xcalc/CMakeLists.txt).

The shape is:

```cmake
set(XCALC_SRC_DIR "${CMAKE_SOURCE_DIR}/ignored-area/third-party/xcalc")

add_executable(xcalc
    ${XCALC_SRC_DIR}/xcalc.c
    ${XCALC_SRC_DIR}/actions.c
    ${XCALC_SRC_DIR}/math.c
)

target_compile_definitions(xcalc PRIVATE HAVE_CONFIG_H=1)
target_include_directories(xcalc PRIVATE ${XCALC_SRC_DIR})
target_compile_options(xcalc PRIVATE -w)

# --preload-file embeds app-defaults into a .data package that the
# Emscripten glue loads automatically before main(). No JS-side preRun
# hook needed — just this one line in CMake.
emx11_finalize_demo(xcalc
    EXPORT_NAME createXcalcModule
    LIBS Xaw Xmu Xt Xpm Xext X11
    PRELOAD_FILES "${XCALC_SRC_DIR}/app-defaults/XCalc@/usr/lib/X11/app-defaults/XCalc"
)
```

A few things to notice:

- We treat `ignored-area/third-party/xcalc/` as the source directory directly. The
  three `.c` files come straight from the tarball; we do not patch
  them.
- Third-party code is built with `-w` because our own warning flags
  (`-Wall -Wextra`) trip on patterns like unused `XtActionsRec`
  callback args.
- The link line reads like a standard Xaw program: `Xaw Xmu Xt Xpm
  Xext X11`. The `LIBS` list documents the link order for readers
  (higher-level first). The actual linking goes through the
  emscripten-ports script.
- `emx11_finalize_demo` is the helper at
  [cmake/emx11_demo.cmake](../cmake/emx11_demo.cmake). It adds the
  include path, wires the port into the emcc command line, injects the
  JS bridge library (`--js-library`) and the default Host IIFE
  (`--pre-js`), and sets the Emscripten link options (`MODULARIZE`,
  `EXPORT_ES6`, `ASYNCIFY`, `ENVIRONMENT=web,worker`,
  `ALLOW_MEMORY_GROWTH`, ...) plus the `EXPORTED_FUNCTIONS` /
  `EXPORTED_RUNTIME_METHODS` lists. `PRELOAD_FILES` translates
  directly to emcc's `--preload-file` — the standard Emscripten
  mechanism for embedding files at build time.

The helper sets the artifact location at
`build/artifacts/xcalc/xcalc.js` and `xcalc.wasm`. The dev server
serves `build/` at `/build/`.

## 4. Wire the demo into the top-level build

Open [CMakeLists.txt](../CMakeLists.txt) and add:

```cmake
add_subdirectory(examples/xcalc)
```

(For xcalc it is already there.)

## 5. Embed `app-defaults/XCalc` via `--preload-file`

This is the part that bites every Xt port and is worth dwelling on.

When you launch xcalc on a real desktop, `XtResolvePathname` looks for
`/usr/share/X11/app-defaults/XCalc` (or `/etc/X11/app-defaults/XCalc`).
That file contains the entire button layout, colours, fonts — without
it `XtGetResources` returns nothing and the calculator collapses into a
stack of unreadable Command buttons.

In Emscripten land each module starts with an empty MEMFS, so the file
is not there unless you put it there. The standard Emscripten way is
`--preload-file`: emcc packages the file into a `.data` blob alongside
the `.wasm`, and the generated JS glue loads it automatically before
`main()` runs. No JS-side `preRun` hook needed.

The CMake side is a single line passed to `emx11_finalize_demo`
([examples/xcalc/CMakeLists.txt](../examples/xcalc/CMakeLists.txt)):

```cmake
emx11_finalize_demo(xcalc
    EXPORT_NAME createXcalcModule
    LIBS Xaw Xmu Xt Xpm Xext X11
    PRELOAD_FILES "${XCALC_SRC_DIR}/app-defaults/XCalc@/usr/lib/X11/app-defaults/XCalc"
)
```

The `PRELOAD_FILES` argument accepts `<source>@<target>` pairs —
`source` is a build-machine path, `target` is the MEMFS path the file
will appear at inside the wasm. Multiple entries are allowed; each
becomes a separate `--preload-file` flag.

Because the glue handles loading, the JS side is the same three-line
pattern as hello, plus `locateFile` so Emscripten can find the `.wasm`
and `.data` files
([examples/xcalc/main.ts](../examples/xcalc/main.ts)):

```ts
import { initEmX11 } from '../../src/index.js';

const x11 = await initEmX11({ width: 800, height: 600 });

const factory = (await import('/build/artifacts/xcalc/xcalc.js')).default;
await factory({
  ...x11.moduleOverrides,
  thisProgram: 'xcalc',
  locateFile: (path: string) => `/build/artifacts/xcalc/${path}`,
});
```

Key points:

- `initEmX11()` creates the Host and returns `moduleOverrides` — an
  object with `{ emx11Host, emx11NoAutoStart: true }`. Spread it into
  the factory call so the C-side bridges find the Host.
- `thisProgram: 'xcalc'` sets `argv[0]` so `XtResolvePathname`'s `%N`
  substitution finds `XCalc`, not `Module`.
- `locateFile` is the standard Emscripten way to tell the runtime where
  `.wasm` and `.data` files live. Needed when the glue is loaded from a
  different directory than the page URL — Vite serves examples from
  `/examples/xcalc/` but artifacts live under `/build/artifacts/xcalc/`.
- `.data` blobs produced by `--preload-file` go through `locateFile` too,
  so no `preRun` hook needed.
- No `child_process.spawn`, no `emX11.fs` — just the Emscripten factory
  with Module overrides.

If you forget this step the binary still launches, the window still
appears, but you get a 0×0 Form widget with stacked Commands and no
labels. That is the canonical "I forgot app-defaults" symptom.

## 6. The demo page

An HTML entry point and a TypeScript module:

[examples/xcalc/index.html](../examples/xcalc/index.html) is one `<script
type="module">` tag pointing at `main.ts`. Vite picks up every
`examples/*/index.html` automatically as an entry.

For simple programs without assets the pattern is the same three lines.
See [examples/hello/main.ts](../examples/hello/main.ts):

```ts
import { initEmX11 } from '../../src/index.js';

const x11 = await initEmX11({ width: 1024, height: 768 });

const factory = (await import('/build/artifacts/hello/hello.js')).default;
await factory({ ...x11.moduleOverrides });
```

No `child_process`, no `fs`, no launcher — just `initEmX11` + spread
`moduleOverrides` into the factory. xcalc's `main.ts` is identical in
shape; the asset staging moved to `--preload-file` at build time, so
the JS side stays minimal.

## 7. Build and run

From the repo root:

```bash
pnpm build      # cmake configure + build + host IIFE + vite bundle
pnpm dev        # vite dev server (http://localhost:5173)
pnpm preview    # serve the production build from dist/
```

`pnpm build` runs three steps: `build:native` (cmake configure + build
all wasm artifacts), `build:host` (vite bundles the default Host IIFE
for Layer 1 mode), and `build:web` (vite bundles the demo pages).

Open `http://localhost:5173/examples/xcalc/` and you should see xcalc.

Click the buttons. If they highlight on hover and respond to clicks,
the input event path works (DOM `mousemove`/`mousedown` →
`emx11_push_button_event`/`emx11_push_motion_event` → libX11
event queue → Xt's `WaitForSomething` → xcalc's action procs).

## 8. Things that go wrong, and what they mean

- **Window appears, but it is a stack of unlabelled buttons** — you
  did not pass `PRELOAD_FILES` for `app-defaults/XCalc` in CMake. See §5.
- **All windows are pure black** — almost certainly a stale wasm
  artifact. The em-x11 host and the libemx11 inside the demo
  communicate via signature-tied EM_JS bridges; if you change a
  bridge in `native/emx11/bridges.c` you must rebuild every demo.
  `pnpm build` rebuilds everything; partial builds are the usual cause.
- **Browser tab freezes on first redraw** — you forgot
  `ASYNCIFY=1`. Without it, the moment xcalc calls `XNextEvent` and
  there is no event ready, the wasm thread spins instead of
  yielding to the JS event loop.
- **`XtResolvePathname` returns NULL even though you staged the
  file** — `argv[0]` has to be `xcalc`, otherwise the `%N`
  substitution looks for the wrong file. Pass
  `thisProgram: 'xcalc'` to the factory.
- **Build fails complaining about undefined Xrender / Xft / shape
  symbols** — em-x11's static archive ships only the X11 surface
  the demos exercise. If your client pulls in Xrender, either link
  it in or stub it. xcalc does not need any of these.
- **Port can't find em-x11 source** — set `EMX11_SRC` to the
  absolute path of the em-x11 repository.
- **`Module['emx11Host']` is undefined in the wasm** — you forgot
  to spread `x11.moduleOverrides` into the factory call. Without it,
  the C-side bridges have no Host to dispatch to, and the X server
  is effectively absent.

## 9. What to read next

- [docs/api.md](api.md) — the full `createEmX11()` / `initEmX11()` /
  `emX11.fs` / `emX11.child_process.spawn` surface, including IDBFS
  persistence and tar-mount staging.
- [docs/xorg-alignment.md](xorg-alignment.md) — what em-x11
  implements vs. what is stubbed, and where the X protocol is
  approximated.
- [src/runtime/twm-launch.ts](../src/runtime/twm-launch.ts) — a
  more involved launcher with `.twmrc` staging and a multi-client
  session (Layer 3), useful as a template if your port needs a window
  manager.
- [tools/ports/emx11.py](../tools/ports/emx11.py) — the port script
  itself, with comments explaining each hook in the emscripten-ports
  API, `-sUSE_EMX11` integration, and the `needed()` / `process_args()`
  flow.
