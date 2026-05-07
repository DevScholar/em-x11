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
- Every X client is its own Emscripten module (`xcalc.js` +
  `xcalc.wasm`). They are loaded with `emX11.spawn()` and share the host
  the way real X clients share an Xorg.
- There is no global `/usr/lib/X11`. Each spawned module gets a fresh
  in-memory filesystem (Emscripten's MEMFS). em-x11 stages files into a
  manifest (`emX11.fs`) at boot and replays it into every child's MEMFS
  before `main()` runs.

If you keep that picture in mind, the porting work below should feel
familiar — it is mostly the same `./configure && make` story, with a
JS shim at the very edge.

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
bash scripts/fetch-third-party.sh
```

`fetch-third-party.sh` downloads the X.Org tarballs we depend on
(libXt, libXaw, libXmu, libXpm) and the apps we ship as demos
(xeyes, xcalc, twm, xclock) into `third-party/`. That directory is
gitignored — it is fully reproducible from the script.

## 2. Add xcalc to the fetch script

Open [scripts/fetch-third-party.sh](../scripts/fetch-third-party.sh)
and look at the `LIBS` array. xcalc is already there:

```
"xcalc   xcalc   1.1.3   https://www.x.org/releases/individual/app  app"
```

Two layouts are supported: `lib` (for libraries that ship `src/` and
`include/` subdirectories) and `app` (for the flat tarball layout that
X.Org applications use, with all `.c`/`.h` files at the root). xcalc
uses `app`, so the whole extracted tree gets mirrored verbatim into
`third-party/xcalc/`.

For a brand new client you would add a row to the array, run the
script, and you are done with this step.

## 3. Overlay files: `config.h` and `ORIGIN.txt`

Upstream xcalc is autotools-driven. Inside an `./configure` build it
generates `config.h` from `config.h.in`. We do not run autoconf, so we
hand-write a minimal replacement and **overlay** it on top of the
extracted tarball every time the fetch script runs.

The overlay lives at [scripts/third-party/xcalc/config.h](../scripts/third-party/xcalc/config.h).
It is short:

```c
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H   1
/* ...the usual STDC_HEADERS pile... */
#define HAVE_STRLCPY    1   /* musl exposes it; otherwise xcalc's
                               static fallback collides */
#define PACKAGE         "xcalc"
#define PACKAGE_VERSION "1.1.3"
/* etc. */
```

The rule of thumb: claim only the headers Emscripten's musl-derived
libc actually has, and do not claim any optional features
(`HAVE_LANGINFO_H`, `HAVE_GETPAGESIZE`, …). If a feature is missing
the upstream code falls back to a portable path, which is exactly what
we want.

`ORIGIN.txt` is just metadata recording the upstream URL and version,
for license/audit purposes. Copy the xeyes one if you are starting
fresh.

## 4. The demo's `CMakeLists.txt`

Each X client gets its own subdirectory under `demos/`. For xcalc, that
is [demos/xcalc/CMakeLists.txt](../demos/xcalc/CMakeLists.txt).
Read it once before you copy from it; the comments are load-bearing.

The shape is:

```cmake
set(XCALC_SRC_DIR "${CMAKE_SOURCE_DIR}/third-party/xcalc")

add_executable(xcalc
    ${XCALC_SRC_DIR}/xcalc.c
    ${XCALC_SRC_DIR}/actions.c
    ${XCALC_SRC_DIR}/math.c
)

target_compile_definitions(xcalc PRIVATE HAVE_CONFIG_H=1)
target_include_directories(xcalc PRIVATE ${XCALC_SRC_DIR})
target_link_libraries(xcalc PRIVATE Xaw Xmu Xpm Xt emx11_static)
target_compile_options(xcalc PRIVATE -w)
```

A few things to notice:

- We treat `third-party/xcalc/` as the source directory directly. The
  three `.c` files come straight from the tarball; we do not patch
  them.
- Third-party code is built with `-w` because our own warning flags
  (`-Wall -Wextra`) trip on patterns like unused `XtActionsRec`
  callback args. We only want strict warnings on em-x11's own code.
- The link target is `emx11_static`, not `X11`. That is em-x11's
  static archive of every Xlib symbol xcalc will need.

The interesting part is the link options:

```cmake
target_link_options(xcalc PRIVATE
    "SHELL:-s MODULARIZE=1"
    "SHELL:-s EXPORT_ES6=1"
    "SHELL:-s EXPORT_NAME=createXcalcModule"
    "SHELL:-s ASYNCIFY=1"
    "SHELL:-s ENVIRONMENT=web,worker"
    "SHELL:-s ALLOW_MEMORY_GROWTH=1"
    "SHELL:-s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','UTF8ToString','FS']"
    "SHELL:-s EXPORTED_FUNCTIONS=['_main','_emx11_push_button_event',
        '_emx11_push_motion_event','_emx11_push_key_event',
        '_emx11_push_expose_event','_emx11_push_map_request',
        '_emx11_push_reparent_notify']"
)
```

What each line buys you, in X-developer terms:

| Flag | Purpose |
|------|---------|
| `MODULARIZE=1`, `EXPORT_ES6=1`, `EXPORT_NAME=createXcalcModule` | Wrap the wasm in a function the host can call (`spawn`) instead of running on script load. Multiple modules can coexist, the way multiple X clients can connect to one server. |
| `ASYNCIFY=1` | Lets blocking Xlib calls (`XNextEvent`) yield back to the JS event loop. Without it, `XPending`/`XNextEvent` would deadlock the browser tab. Required for any client whose main loop blocks on input. |
| `ENVIRONMENT=web,worker` | Build for both the page and Web Workers. Some host configurations spawn the X clients in a worker thread. |
| `ALLOW_MEMORY_GROWTH=1` | Equivalent to giving xcalc a growable heap instead of a fixed-size arena. |
| `EXPORTED_FUNCTIONS=['_main', '_emx11_push_*']` | The `emx11_push_*` symbols are how the JS host *injects* X events into the client. They live in `libemx11`, so we must keep them in the final binary even though `xcalc.c` does not call them. Think of these as the wasm-side equivalent of `XPutBackEvent`. |
| `EXPORTED_RUNTIME_METHODS=['FS', ...]` | We need `FS` because the launcher writes `app-defaults/XCalc` into the live MEMFS. Other clients can omit it. |

Finally the artifact location:

```cmake
set_target_properties(xcalc PROPERTIES
    SUFFIX ".js"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/artifacts/xcalc"
)
```

This produces `build/artifacts/xcalc/xcalc.js` and `xcalc.wasm`. The
dev server is configured to serve `build/` at `/build/`, so the URL
the JS host loads is `/build/artifacts/xcalc/xcalc.js`.

## 5. Wire the demo into the top-level build

Open [CMakeLists.txt](../CMakeLists.txt) and add:

```cmake
add_subdirectory(demos/xcalc)
```

(For xcalc it is already there.) Without that line, the new
`CMakeLists.txt` is dead code.

## 6. Stage `app-defaults/XCalc`

This is the part that bites every Xt port and is worth dwelling on.

When you launch xcalc on a real desktop, `XtResolvePathname` looks for
`/usr/share/X11/app-defaults/XCalc` (or `/etc/X11/app-defaults/XCalc`,
or wherever your distribution puts it). That file contains the entire
button layout, colours, fonts — without it `XtGetResources` returns
nothing, every widget realises with its compile-time default geometry,
and the calculator collapses into a stack of unreadable Command
buttons.

In Emscripten land each spawned module starts with an empty MEMFS, so
the file is not there unless you put it there. There are two layers
to this:

1. **Stage the bytes into `emX11.fs` once**, at boot. `emX11.fs` is a
   *manifest* maintained by the host; every spawned process replays it
   into its own MEMFS during the Emscripten `preRun` hook. Stage once,
   every child sees it.
2. **Ship the file with your demo bundle.** We import it as a string at
   build time using Vite's `?raw` query suffix. That bakes the contents
   into the JS bundle so the host does not have to fetch it at runtime.

That gives you the launcher in
[src/runtime/xcalc-launch.ts](../src/runtime/xcalc-launch.ts):

```ts
import xcalcAppDefaults from '../../third-party/xcalc/app-defaults/XCalc?raw';
import type { EmX11 } from '../api/emx11.js';

export async function launchXcalc(emX11: EmX11): Promise<Process> {
  emX11.fs.writeFileSync('/usr/lib/X11/app-defaults/XCalc', xcalcAppDefaults);
  const p = emX11.spawn('/build/artifacts/xcalc/xcalc', { thisProgram: 'xcalc' });
  await p.ready;
  return p;
}
```

`thisProgram: 'xcalc'` sets `argv[0]` so that
`XtResolvePathname`'s `%N` (program class name) substitution finds
`XCalc`, not `Module`.

> Frontend translation: `import x from './foo?raw'` is Vite-specific
> syntax that says "read the file at build time, give me its text as
> a string." The Linux equivalent would be embedding the file via
> `objcopy --add-section`.

If you forget this step the binary still launches, the window still
appears, but you get a 0×0 Form widget with stacked Commands and no
labels. That is the canonical "I forgot app-defaults" symptom.

## 7. The demo page

A demo is just an HTML entry point and a TypeScript module:

[demos/xcalc/index.html](../demos/xcalc/index.html) is one `<script
type="module">` tag pointing at `main.ts`. Vite picks up every
`demos/*/index.html` automatically as an entry, so just placing the
file there makes the dev server publish a route.

[demos/xcalc/main.ts](../demos/xcalc/main.ts) is six lines:

```ts
import { createEmX11 } from '../../src/index.js';
import { launchXcalc } from '../../src/runtime/xcalc-launch.js';

const emX11 = await createEmX11({ width: 800, height: 600 });
await launchXcalc(emX11);
```

`createEmX11` boots the host (creates the canvas, registers DOM input
listeners, primes `emX11.fs`'s default mounts at `/tmp /usr /etc /opt
/var /home`). `launchXcalc` does the steps from §6.

## 8. Build and run

From the repo root:

```bash
pnpm build      # configures + builds wasm artifacts (calls emcmake/emmake under the hood)
pnpm dev        # vite dev server with hot reload
```

Open `http://localhost:5173/demos/xcalc/` and you should see xcalc.

Click the buttons. If they highlight on hover and respond to clicks,
the input event path works (DOM `mousemove`/`mousedown` →
`emx11_push_button_event`/`emx11_push_motion_event` → libX11
event queue → Xt's `WaitForSomething` → xcalc's action procs). If
the layout looks right but clicks do nothing, suspect that you forgot
one of the `_emx11_push_*` exports.

## 9. Things that go wrong, and what they mean

- **Window appears, but it is a stack of unlabelled buttons** — you
  did not stage `app-defaults/XCalc`. See §6.
- **All windows are pure black** — almost certainly a stale wasm
  artifact. The em-x11 host and the libemx11 inside the demo
  communicate via signature-tied EM_JS bridges; if you change a
  bridge in `native/src/bridges.c` you must rebuild every demo
  before the host's new shape matches. `pnpm build` rebuilds
  everything; partial builds are the usual cause.
- **Browser tab freezes on first redraw** — you forgot
  `ASYNCIFY=1`. Without it, the moment xcalc calls `XNextEvent` and
  there is no event ready, the wasm thread spins instead of
  yielding to the JS event loop.
- **`XtResolvePathname` returns NULL even though you staged the
  file** — `argv[0]` has to be `xcalc` (or `XCalc` for the class
  name), otherwise the `%N` substitution looks for the wrong file.
  Pass `thisProgram: 'xcalc'` to `emX11.spawn`.
- **Build fails complaining about undefined Xrender / Xft / shape
  symbols** — em-x11's static archive ships only the X11 surface
  the demos exercise. If your client pulls in Xrender, either link
  it in (see how Tk wires up `--enable-xft`) or stub it. xcalc does
  not need any of these.

## 10. What to read next

- [docs/api.md](api.md) — the full `createEmX11()` / `emX11.fs` /
  `emX11.spawn` surface, including IDBFS persistence and tar-mount
  staging for clients with bigger asset trees.
- [docs/xorg-alignment.md](xorg-alignment.md) — what em-x11
  implements vs. what is stubbed, and where the X protocol is
  approximated.
- [src/runtime/twm-launch.ts](../src/runtime/twm-launch.ts) — a
  more involved launcher with `.twmrc` staging and a multi-client
  session, useful as a template if your port needs a window
  manager.
