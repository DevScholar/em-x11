/**
 * Ambient types for Emscripten runtime bits that @types/emscripten doesn't
 * cover cleanly when em-x11 is built with `-s MODULARIZE=1 -s EXPORT_ES6=1`.
 */

/**
 * MEMFS handle exposed by Emscripten's filesystem support. We only use
 * writeFile (to stage twmrc and other config files before main runs);
 * the rest of the FS API is intentionally not typed here.
 */
export interface EmscriptenFS {
  writeFile(path: string, data: string | Uint8Array): void;
  mkdir?(path: string): void;
}

export interface EmscriptenModule {
  HEAPU8: Uint8Array;
  HEAP32: Int32Array;
  HEAPU32: Uint32Array;
  HEAPF64: Float64Array;

  _malloc(size: number): number;
  _free(ptr: number): void;

  ccall(
    name: string,
    returnType: string | null,
    argTypes: string[],
    args: unknown[],
  ): unknown;

  cwrap<F extends (...args: never[]) => unknown>(
    name: string,
    returnType: string | null,
    argTypes: string[],
  ): F;

  UTF8ToString(ptr: number, maxBytesToRead?: number): string;
  stringToUTF8(str: string, outPtr: number, maxBytesToWrite: number): void;
  lengthBytesUTF8(str: string): number;

  addFunction?(fn: (...args: unknown[]) => unknown, signature: string): number;
  removeFunction?(fn: number): void;

  /** Process argv (excluding argv[0], which Emscripten sets to ./this.program).
   *  Settable via the factory argument; read by the wasm's main(). */
  arguments?: string[];
  /** Hooks fired between FS init and main(). Use for staging files into
   *  MEMFS so the program sees them at startup (e.g. config files). */
  preRun?: ((mod: EmscriptenModule) => void)[];
  /** MEMFS handle. Available inside preRun and afterwards; not on the
   *  factory-arg side. */
  FS?: EmscriptenFS;
}

/**
 * Signature of the factory produced by Emscripten's `MODULARIZE=1 EXPORT_ES6=1`.
 * Each demo wasm exports one of these as its default.
 */
export type EmscriptenModuleFactory<M extends EmscriptenModule = EmscriptenModule> = (
  moduleArg?: Partial<M>,
) => Promise<M>;

export interface ShapeRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface Point {
  x: number;
  y: number;
}

/**
 * The em-x11 host object, installed on `globalThis` before wasm starts so
 * that C code (via src/bindings/emx11.library.js) can reach TS-side state.
 * Populated by src/runtime/host.ts.
 */
export interface EmX11Host {
  onInit(screenWidth: number, screenHeight: number): void;
  /** XOpenDisplay entry. Allocates a connection id (used to route events
   *  to the caller's wasm Module in the multi-client world) and grants
   *  an XID range per x11protocol.txt §869/§935. Every XID the C side
   *  later hands out is `xidBase | (counter & xidMask)`, and ranges
   *  across connections never overlap. */
  openDisplay(): { connId: number; xidBase: number; xidMask: number };
  /** XCloseDisplay entry. Host drops the connection and (eventually, in
   *  Step 2) releases any windows / pixmaps / atoms it owned. */
  closeDisplay(connId: number): void;
  /** Shared root window's XID. Every client's XOpenDisplay asks Host
   *  for this rather than creating a per-connection root; one root,
   *  one set of compositor entries, one place to hang the
   *  SubstructureRedirect holder in Step 3. */
  getRootWindow(): number;
  onWindowCreate(
    connId: number,
    id: number,
    parent: number,
    x: number,
    y: number,
    width: number,
    height: number,
    borderWidth: number,
    borderPixel: number,
    background: number,
  ): void;
  onWindowSetBorder(id: number, borderWidth: number, borderPixel: number): void;
  onWindowSetBg(id: number, background: number): void;
  onWindowSetBg(id: number, background: number): void;
  /** Geometry-only update for an existing window (XMoveWindow /
   *  XResizeWindow / XConfigureWindow). Leaves parent, shape, and
   *  background_pixmap alone. */
  onWindowConfigure(id: number, x: number, y: number, w: number, h: number): void;
  /** XMapWindow entry. `connId` is the caller's connection so Host can
   *  enforce the SubstructureRedirect "caller == holder bypass" rule
   *  (x11protocol.txt §1592). Host internally decides whether to
   *  actually map (no redirect / OR set / caller == holder) or to
   *  synthesize MapRequest to the redirect holder. */
  onWindowMap(connId: number, id: number): void;
  onWindowUnmap(connId: number, id: number): void;
  onWindowDestroy(id: number): void;
  /** XSelectInput mirror. Host stores the mask per (window, caller)
   *  and enforces at-most-one SubstructureRedirectMask per window
   *  (x11protocol.txt §1477). */
  onSelectInput(connId: number, id: number, mask: number): void;
  /** XChangeWindowAttributes(CWOverrideRedirect) mirror. OR=True marks
   *  the window as WM-invisible for redirect purposes (popup menus,
   *  tooltips, twm's own decoration frames). */
  onSetOverrideRedirect(id: number, flag: boolean): void;
  /** XReparentWindow -- update parent link and position in the new
   *  parent's coord space. Forwarded even for windows the caller
   *  doesn't own (twm takes xeyes's shell as a child of its frame). */
  onReparentWindow(id: number, parent: number, x: number, y: number): void;
  /** Bind a Pixmap as the window's tiled background, or unbind when
   *  pmId === 0 (revert to solid background_pixel). The compositor
   *  paints with `createPattern(pixmap.canvas, 'repeat')` from then on. */
  onWindowSetBgPixmap(id: number, pmId: number): void;
  /** XClearWindow / XClearArea entry. The compositor picks solid vs
   *  pattern based on the window's current background_pixmap. */
  onClearArea(id: number, x: number, y: number, w: number, h: number): void;
  onFillRect(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    color: number,
  ): void;
  onDrawLine(
    id: number,
    x1: number,
    y1: number,
    x2: number,
    y2: number,
    color: number,
    lineWidth: number,
  ): void;
  /** X arc: (x, y) is the bounding box top-left, (w, h) is the bounding
   *  box size, angle1/angle2 are in 1/64ths of a degree, counterclockwise
   *  from 3 o'clock. */
  onDrawArc(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1: number,
    angle2: number,
    color: number,
    lineWidth: number,
  ): void;
  onFillArc(
    id: number,
    x: number,
    y: number,
    w: number,
    h: number,
    angle1: number,
    angle2: number,
    color: number,
  ): void;
  /** Polygon fill. Points already resolved to absolute coordinates
   *  window-local. `shape` is Complex/Nonconvex/Convex; `mode` is
   *  CoordModeOrigin (we always pre-resolve to this). */
  onFillPolygon(
    id: number,
    points: Point[],
    shape: number,
    mode: number,
    color: number,
  ): void;
  onDrawPoints(
    id: number,
    points: Point[],
    mode: number,
    color: number,
  ): void;
  /** Draw a text run with the given CSS font shorthand. When imageMode is
   *  non-zero, the text's bounding box is filled with bgColor first
   *  (XDrawImageString semantics). */
  onDrawString(
    id: number,
    x: number,
    y: number,
    font: string,
    text: string,
    fgColor: number,
    bgColor: number,
    imageMode: number,
  ): void;
  onFlush(): void;
  /** SHAPE extension: replace the window's bounding region. An empty
   *  array means "no shape" -- render the whole window rectangle. */
  onWindowShape(id: number, rects: ShapeRect[]): void;
  /** Latest pointer position in canvas CSS pixels. Read by XQueryPointer;
   *  updated by Host on every canvas mousemove regardless of hit-test. */
  getPointerXY(): Point;
  /** Pixmap lifecycle. Each Pixmap is backed by an OffscreenCanvas on the
   *  host; create installs the id, destroy drops the reference. depth=1
   *  pixmaps are the SHAPE-mask path; other depths are accepted but only
   *  XFillRectangle / XFillArc are currently routed to their ctx. */
  onPixmapCreate(id: number, width: number, height: number, depth: number): void;
  onPixmapDestroy(id: number): void;
  /** XShapeCombineMask: read the source pixmap's bits, convert to
   *  bounding rectangles, and install on the destination window. */
  onShapeCombineMask(
    destId: number,
    srcId: number,
    xOff: number,
    yOff: number,
    op: number,
  ): void;
  /** XInternAtom -- Host-allocated id for atoms above the 1..68
   *  predefined range. Same name always resolves to the same id across
   *  connections, which fixes the WM_PROTOCOLS / WM_DELETE_WINDOW
   *  divergence that used to happen when each wasm module owned its
   *  own counter. Returns 0 (None) when onlyIfExists is true and the
   *  name has never been seen. */
  internAtom(name: string, onlyIfExists: boolean): number;
  /** XGetAtomName for Host-allocated atoms. Returns null when the id
   *  is unknown (caller maps that to NULL / BadAtom). */
  getAtomName(atom: number): string | null;
  /** XCopyArea: blit between any two Drawables (Window or Pixmap).
   *  Host picks the path (pixmap canvas vs root canvas rectangle)
   *  based on which ids are Pixmap-registered. Tk's double-buffering
   *  flow is Pixmap→Window. */
  onCopyArea(
    srcId: number,
    dstId: number,
    srcX: number,
    srcY: number,
    w: number,
    h: number,
    dstX: number,
    dstY: number,
  ): void;
  /** XCopyPlane: simplified to a depth-1 source pixmap whose alpha is
   *  the plane. Set bits paint with fg; unset bits paint with bg if
   *  applyBg is true (GXcopy + opaque stipple). */
  onCopyPlane(
    srcId: number,
    dstId: number,
    srcX: number,
    srcY: number,
    w: number,
    h: number,
    dstX: number,
    dstY: number,
    plane: number,
    fg: number,
    bg: number,
    applyBg: boolean,
  ): void;
  /** XPutImage: blit raw pixel data into a Drawable. format==0 (XYBitmap)
   *  + depth==1 paints as fg/bg stencil; format==2 (ZPixmap) expects
   *  32bpp BGRA in the byte stream matching display.c's format0. */
  onPutImage(
    dstId: number,
    dstX: number,
    dstY: number,
    w: number,
    h: number,
    format: number,
    depth: number,
    bytesPerLine: number,
    data: Uint8Array,
    fg: number,
    bg: number,
  ): void;
}

declare global {
  // eslint-disable-next-line no-var
  var __EMX11__: EmX11Host | undefined;
}
