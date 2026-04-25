# em-x11

A WASM implementation of the X11 / Xlib C API that targets HTML5 Canvas
instead of a real X server. Programs written against Xlib are compiled
with Emscripten, link against `em-x11`, and draw directly to a browser
canvas — there is **no** X protocol byte stream and no server process.

> Status: **early skeleton.** The pipeline compiles end-to-end and the
> bundled `hello` demo paints rectangles via a live `XFillRectangle`
> call, but the Xlib surface area is tiny. See the roadmap below.

## Why

The end goal is running **tkinter Python applications inside Pyodide**,
entirely in the browser. That path requires:

- **Tk** — widget toolkit, pure Xlib client
- **Tcl** — already ported to WASM (without Tk)
- **An Xlib implementation** — *this repository*

Every intermediate stage is its own milestone that's useful on its own.

## Architecture

Single full-page `<canvas>` + software compositing + an internal window
manager. X windows are plain records inside em-x11; the TypeScript
compositor paints them onto the one canvas, does hit-testing for mouse
events, and translates DOM events into `XEvent` structures that the C
code dequeues via `XNextEvent`.

```
┌─────────────────────────────────────────────────────────────┐
│  Browser tab                                                │
│                                                             │
│   ┌─────────────────────────────────────────┐               │
│   │  <canvas>  (full viewport)              │               │
│   └─────────────────────────────────────────┘               │
│        ▲                                                    │
│        │ 2D draw calls                                      │
│   ┌────┴────────────────────────────────────┐               │
│   │  TypeScript runtime (src/runtime/)      │               │
│   │    compositor · hit-test · event bridge │               │
│   └────┬────────────────────────────────────┘               │
│        │ globalThis.__EMX11__                               │
│   ┌────┴────────────────────────────────────┐               │
│   │  --js-library (src/bindings/*.js)       │               │
│   └────┬────────────────────────────────────┘               │
│        │ extern emx11_js_* symbols                          │
│   ┌────┴────────────────────────────────────┐               │
│   │  em-x11 C implementation (native/src/)  │               │
│   │    X11 state · event queue · API impl   │               │
│   └────┬────────────────────────────────────┘               │
│        │ <X11/Xlib.h>                                       │
│   ┌────┴────────────────────────────────────┐               │
│   │  client program (demos/, xapps/)        │               │
│   └─────────────────────────────────────────┘               │
└─────────────────────────────────────────────────────────────┘
```

The deeper architectural rationale (why Plan B, why full-page canvas,
why an internal WM, why not DOM-per-window) is worked out in the issues
and commit log.

## Layout

```
em-x11/
├── native/             # C implementation
│   ├── include/X11/    # Public Xlib headers (placeholder -> upstream)
│   └── src/            # em-x11 implementation + internal headers
├── src/                # TypeScript runtime (compositor, events, loader)
│   ├── runtime/
│   ├── bindings/       # --js-library glue (emscripten side)
│   ├── loader/
│   └── types/
├── demos/              # In-tree demos (hello, and more to come)
├── xapps/              # Target third-party X programs (xeyes, xclock)
├── third-party/        # Cleaned copies of libXt/libXaw/etc. (later)
├── references/         # READ-ONLY: XFree86/Xming/x11-docs source trees
├── CMakeLists.txt
├── package.json
├── tsconfig.json
└── vite.config.ts
```

`references/` is consulted for API shape and implementation hints, but
**no code is compiled out of it**. Code we want to use is copied into
`third-party/` or `native/include/` with an `ORIGIN.txt` recording the
source tarball and date.

## Prerequisites

**All development happens inside WSL or a real Linux environment.** The
Windows side is only used as a host for WSL; Node, pnpm, cmake, and the
Emscripten SDK must all be installed **inside WSL**, not on Windows.
Mixing Windows-side tools with WSL-side tools leads to path-translation
bugs, line-ending corruption, and permission surprises.

Inside WSL:

- **Node ≥ 20** (install via `nvm` or your distro's package manager)
- **pnpm ≥ 9** (`npm install -g pnpm` or `corepack enable`)
- **emsdk** with an activated recent Emscripten. Ensure `emcc` is on
  `PATH` in non-interactive shells — source `emsdk_env.sh` from your
  `~/.bashrc` or `~/.profile`.
- **cmake ≥ 3.20**, **make**, **git**

Verify inside WSL:

```bash
node --version     # >= v20
pnpm --version     # >= 9
emcc --version
cmake --version
```

### Recommended: keep the repo inside the WSL filesystem

Working out of `/mnt/c/...` (a Windows-mounted drive) works but is much
slower than WSL's native ext4 and has occasional permission quirks with
`chmod +x` scripts. Prefer cloning to `~/projects/em-x11/` inside WSL.

### Editor

Use VS Code with the **Remote - WSL** extension, or any editor launched
from a WSL shell. Do not open the project with a Windows-side editor
that will shell out to Windows-side `node.exe` — that's the mixing we're
avoiding.

## Build & run

All commands are run from a WSL shell, from the project root:

```bash
# One-time
pnpm install

# Build everything (C via emcc, then TS via Vite)
pnpm build

# Dev loop: rebuild C once, then watch TS
pnpm build:native
pnpm dev
```

Vite listens on `http://localhost:5173`. WSL2 forwards the port to the
Windows host automatically, so you can open the URL in any browser on
Windows.

## Roadmap

| Stage | Goal                                                                                                     |
| ----- | -------------------------------------------------------------------------------------------------------- |
| v0    | **Skeleton.** `hello` demo paints rectangles. Build pipeline end-to-end. ← *you are here*                |
| v1    | Run **xeyes** and **xclock** unmodified (requires libXt + libXaw + libXmu + libXext ported as side libs) |
| v2    | Port an X window manager (**xwm**). Implies SubstructureRedirect, Reparent, ICCCM/EWMH.                  |
| v3    | **Tcl/Tk** on top. Xft/XRender hijacked to canvas `fillText` to avoid the FreeType dependency chain.    |
| v4    | **Pyodide + tkinter.** em-x11 + Tcl + Tk exposed as Pyodide SIDE_MODULE shared libraries.                |

Each stage lists its in-scope Xlib APIs in its own issue. Treat passing
one stage as a lower bound on the next — Tk will demand APIs that xeyes
never touched (selections, XIM, Pixmap semantics, XRender).

## Known skeleton limitations

- `native/include/X11/Xlib.h` is a **hand-written placeholder**, not the
  upstream libX11 header. It will be replaced before libXt is brought
  in; see `native/include/ORIGIN.txt` for the procedure.
- `XNextEvent` relies on Asyncify (`emscripten_sleep`), which inflates
  wasm size somewhat. Apps that prefer a tight main loop can use
  `XPending` + `emscripten_set_main_loop` as the `hello` demo does.
- Per-window backing stores, dirty-rectangle tracking, proper Expose
  event generation on window occlusion, and z-order management are all
  stubbed. The compositor presently repaints everything on each change.
- No clipboard, no IME, no fonts beyond what the browser supplies.

## License

MIT. Upstream X.Org / XFree86 code that we copy into `third-party/`
or `native/include/` carries its own MIT / X Consortium license,
preserved alongside the copied files.
