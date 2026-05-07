# EmX11

⚠️ This project is in early development and is not yet stable. Expect breaking changes and missing features.

A WebAssembly implementation of the X11/Xlib C API that renders X windows to a browser Canvas — no real X server required.

![Xeyes Screenshot](screenshots/xeyes.png)

Built on top of [X.Org](https://www.x.org/) (xorgproto, libX11, libXt, libXaw), compiled with Emscripten and composited onto Canvas via a TypeScript runtime.

# Prerequisites

- Linux (or WSL)
- Emscripten (latest emsdk recommended; `emcc` must be on `PATH`)
- Node.js ≥ 20, pnpm ≥ 9
- cmake ≥ 3.20, make, git

```bash
pnpm install
bash scripts/fetch-third-party.sh
```

# Build

```bash
pnpm build
```

# Run

```bash
pnpm dev
```

# Architecture

em-x11 runs as a single-threaded Host ([src/host/index.ts](src/host/index.ts))
where wasm clients share the main JS thread with the X "server" logic.
The Host owns the canvas and window tree, and wasm clients reach it
directly via `globalThis.__EMX11__` bridges.

Multiple wasm clients (e.g. twm + xeyes + xcalc) can run in the same
Host concurrently without workers — the Host routes events to the
right client via per-connection bookkeeping (see the `session` demo).
This is the same runtime used by wacl-tk and pyodide-tk.

A prior dual-mode design (multi-threaded "channel" mode with
OffscreenCanvas workers and MessagePort RPC, specified in
`docs/multi-wasm.md`) was removed at pre-alpha: it was never wired
into a demo, gave no measured perf benefit, and doubled the
maintenance cost of every bridge.

# Documentation

No API documentation yet — the project is still unstable.

# License

MIT. Third-party X.Org code under `third-party/` and `native/include/` retains its original MIT / X Consortium license.
