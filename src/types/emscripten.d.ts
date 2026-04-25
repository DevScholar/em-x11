/**
 * Ambient types for Emscripten runtime bits that @types/emscripten doesn't
 * cover cleanly when em-x11 is built with `-s MODULARIZE=1 -s EXPORT_ES6=1`.
 */

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
  onWindowCreate(
    id: number,
    x: number,
    y: number,
    width: number,
    height: number,
    background: number,
  ): void;
  onWindowMap(id: number): void;
  onWindowUnmap(id: number): void;
  onWindowDestroy(id: number): void;
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
}

declare global {
  // eslint-disable-next-line no-var
  var __EMX11__: EmX11Host | undefined;
}
