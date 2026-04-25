# em-x11

A WASM implementation of the X11 / Xlib C API that targets HTML5 Canvas
instead of a real X server. Programs written against Xlib are compiled
with Emscripten, link against `em-x11`, and draw directly to a browser
canvas — there is **no** X protocol byte stream and no server process.

> Status: **libXt + libXaw linked and running.** The in-tree demos
> cover raw Xlib (`hello` — filled rectangles), libXt + Core child
> widgets with event dispatch (`xt-hello`), and libXaw Label on top
> of Xt/Xmu/Xpm (`xaw-hello`). Broader Xlib coverage still has large
> stub patches — see the roadmap below.

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
│   ├── include/X11/    # Public Xlib headers (upstream xorgproto + libX11)
│   └── src/            # em-x11 implementation + internal headers
├── src/                # TypeScript runtime (compositor, events, loader)
│   ├── runtime/
│   ├── bindings/       # --js-library glue (emscripten side)
│   ├── loader/
│   └── types/
├── demos/              # In-tree demos (hello, xt-hello, xaw-hello)
├── xapps/              # Target third-party X programs (xeyes, xclock)
├── third-party/        # libXt / libXaw / libXmu / libXpm (gitignored;
│                       # rehydrated by scripts/fetch-third-party.sh)
├── scripts/            # Build-support scripts + third-party overlays
│                       # (CMakeLists / config.h / patches for third-party)
├── references/         # READ-ONLY: XFree86/Xming/x11-docs source trees
│                       # and upstream tarball cache under _tarballs/
├── CMakeLists.txt
├── package.json
├── tsconfig.json
└── vite.config.ts
```

`references/` is consulted for API shape and implementation hints, but
**no code is compiled out of it**. Upstream libraries we actually build
against (libXt, libXaw, libXmu, libXpm) are re-hydrated into
`third-party/` by `scripts/fetch-third-party.sh` from X.Org tarballs,
using cached copies under `references/_tarballs/` when available.
`third-party/` itself is gitignored to keep the checkout small; our
meta files (per-library `CMakeLists.txt`, `config.h`, `ORIGIN.txt`,
plus patches and pre-generated sources) live under `scripts/third-party/`
and get overlaid on the upstream tree after extraction.

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
bash scripts/fetch-third-party.sh   # populate third-party/ from X.Org tarballs

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
| v0    | **Skeleton.** `hello` demo paints rectangles. Build pipeline end-to-end. *done*                          |
| v1    | Run **xeyes** and **xclock** unmodified (libXt + libXaw + libXmu + libXpm built, Xext covered by stubs). libXt and libXaw link and render a Label; xeyes/xclock bring-up in progress. ← *you are here* |
| v2    | Port an X window manager (**xwm**). Implies SubstructureRedirect, Reparent, ICCCM/EWMH.                  |
| v3    | **Tcl/Tk** on top. Xft/XRender hijacked to canvas `fillText` to avoid the FreeType dependency chain.     |
| v4    | **Pyodide + tkinter.** em-x11 + Tcl + Tk exposed as Pyodide SIDE_MODULE shared libraries.                |

Each stage lists its in-scope Xlib APIs in its own issue. Treat passing
one stage as a lower bound on the next — Tk will demand APIs that xeyes
never touched (selections, XIM, Pixmap semantics, XRender).

## Known limitations

- Public headers under `native/include/X11/` are unmodified copies from
  upstream xorgproto + libX11 + libXext. See `native/include/ORIGIN.txt`
  for the exact tarball versions and the refresh procedure.
- Large swathes of Xlib are link-time stubs. `native/src/xt_stubs.c` and
  `native/src/xaw_stubs.c` document which symbols are fake-but-linkable
  vs. really implemented. Pixmap/XCopyArea/XCopyPlane in particular are
  no-ops; anything that tries to blit an offscreen surface will silently
  do nothing.
- `XNextEvent` relies on Asyncify (`emscripten_sleep`), which inflates
  wasm size somewhat. Apps that prefer a tight main loop can use
  `XPending` + `emscripten_set_main_loop` as the `hello` demo does.
- Per-window backing stores, dirty-rectangle tracking, proper Expose
  event generation on window occlusion, and z-order management are all
  stubbed. The compositor presently repaints everything on each change.
- No clipboard, no IME. Fonts are whatever the browser's canvas can
  render; XFontSet is a single-font wrapper with UTF-8 glyph coverage
  via `canvas.fillText` rather than a real per-charset bundle.

## License

MIT. Upstream X.Org / XFree86 code that we copy into `third-party/`
or `native/include/` carries its own MIT / X Consortium license,
preserved alongside the copied files.
