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
  readFile(path: string, opts?: { encoding?: 'binary' | 'utf8' }): Uint8Array | string;
  mkdir(path: string): void;
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

  /** Factory-time hook that overrides where Emscripten fetches sibling
   *  assets (.wasm, --preload-file .data blobs). Only meaningful when
   *  passed to the Module factory; not present on the runtime instance.
   *  Typed here because `Partial<EmscriptenModule>` is what the factory
   *  signature accepts as its argument. */
  locateFile?: (path: string, prefix: string) => string;

  /** Factory-time hook that bypasses Emscripten's default
   *  `WebAssembly.instantiateStreaming(fetch(wasmUrl))` so the caller
   *  can supply pre-fetched bytes (cache, prefetch, custom transport).
   *  Must call `success(instance, module?)` once instantiation is done;
   *  the return value is ignored by Emscripten in the typical async
   *  pattern. Returning `{}` (or any non-undefined value) tells
   *  Emscripten "I'm handling it asynchronously, do not also try to
   *  instantiate yourself." */
  instantiateWasm?: (
    imports: WebAssembly.Imports,
    success: (instance: WebAssembly.Instance, module?: WebAssembly.Module) => void,
  ) => unknown;

  /** Process argv (excluding argv[0], which Emscripten sets to ./this.program).
   *  Settable via the factory argument; read by the wasm's main(). */
  arguments?: string[] | undefined;
  /** Override Emscripten's default argv[0] (`./this.program`). Xt apps
   *  derive their application name / WM_CLASS / default WM_NAME from
   *  `basename(argv[0])`, so leaving it at the default makes twm render
   *  every window's title as "this.program". Set to e.g. "xeyes". */
  thisProgram?: string | undefined;
  /** Hooks fired between FS init and main(). Use for staging files into
   *  MEMFS so the program sees them at startup (e.g. config files). */
  preRun?: ((mod: EmscriptenModule) => void)[] | undefined;
  /** MEMFS handle. Available inside preRun and afterwards; not on the
   *  factory-arg side. */
  FS?: EmscriptenFS;
  /** When false, exit() / main-return runs atexit + Module.onExit before
   *  abandoning the runtime. Emscripten defaults to true (no cleanup),
   *  which leaves wasm-owned host resources stranded after exit(). */
  noExitRuntime?: boolean;
  /** Override Emscripten's `quit_` (the throw used by exit/abort).
   *  Captured at runtime init from this slot, so must be passed in the
   *  factory args -- setting it post-init has no effect. */
  quit?: (status: number, toThrow: unknown) => void;
  /** Fired by Emscripten's _proc_exit when noExitRuntime=false. Used by
   *  ConnectionManager.launchClient to trigger window cleanup when the
   *  client process exits without an explicit XCloseDisplay. */
  onExit?: (status: number) => void;
  /** stdout / stderr sinks. Recent Emscripten captures these at runtime
   *  init and exposes `Module.print` / `Module.printErr` as getter-only
   *  accessors over a closure variable, so post-init assignment throws.
   *  Pass via the factory arg instead. */
  print?: (msg: string) => void;
  printErr?: (msg: string) => void;
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
 * The em-x11 Host interface. The TypeScript Host class in
 * src/host/index.ts implements this so the C-side EM_JS bridges
 * (native/emx11/bridges.c) and the JS library
 * (native/src/lib/library_emx11.js) share one dispatch table.
 *
 * The Host is passed to the wasm Module via Module['emx11Host']
 * (flat Module property, per emscripten convention), set by
 * initEmX11() / createEmX11() through moduleOverrides.
 * library_emx11.js reads it in $EmX11Host.init() at startup.
 */
/** Legacy type bundle for the internal Host slots. These are now set
 * on Module['emx11Host'] / Module['emx11Caches'] / Module['emx11Debug']
 * (flat Module properties per emscripten convention), not on a
 * globalThis namespace. The types remain so Host and the JS library
 * share the same shape; only the access path changed. */
export interface EmX11Global {
  /** Host facade dispatched into by every EM_JS body.
   *  Set via Module['emx11Host'] by initEmX11 / createEmX11. */
  _bridge?: EmX11Host;
  /** Bridge-owned scratchpads (font measure ctx, font cache, text
   *  cache, property stash). Lazy-initialised by the bridges
   *  themselves; the JS side only reads these for diagnostics. */
  _caches?: {
    measureCtx?: CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D | null;
    fontCache?: Map<string, { ascent: number; descent: number; maxW: number; widths: Int32Array }>;
    textCache?: Map<string, number>;
    propStash?: Uint8Array | null;
  };
  /** Runtime trace flags toggled from JS / DevTools. Read by the
   *  hit-test walker, paint walker, input bridge, and a handful of
   *  C-side EM_ASM gates. The bridges don't write these; only DevTools
   *  / api/debug.ts do. All boolean; default false. */
  _debug?: {
    /** Log every findWindowAt call. Spammy on Motion. */
    traceHit: boolean;
    /** One-shot: log only the very next findWindowAt call, then the
     *  helper auto-clears it. Use from DevTools right before clicking
     *  the mystery point. */
    traceHitNext: boolean;
    /** JS-side input bridge motion log (every canvas mousemove). */
    traceMotion: boolean;
    /** JS-side input bridge button log (down/up). */
    traceButton: boolean;
    /** Renderer paint walk log (compositor + window-tree clip math). */
    tracePaint: boolean;
    /** C-side button event delivery log (event.c). */
    traceCBtn: boolean;
    /** C-side motion event delivery log (event.c). */
    traceCMot: boolean;
    /** XMoveWindow / XConfigureWindow log (window.c). */
    traceMove: boolean;
    /** XQueryPointer log output. */
    traceQp: boolean;
  };
}

/**
 * The em-x11 host bridge facade, installed under Module['emx11Host']
 * by Host.attachToBridge(). The C side calls into this via EM_JS bodies
 * in native/emx11/bridges.c (side-module path) or via the JS library
 * in native/src/lib/library_emx11.js (static-link path).
 */
export interface EmX11Host {
  onInit(screenWidth: number, screenHeight: number): void;
  /** XOpenDisplay entry. Allocates a connection id (used to route events
   *  to the caller's wasm Module in the multi-client world) and grants
   *  an XID range per x11protocol.txt §869/§935. Every XID the C side
   *  later hands out is `xidBase | (counter & xidMask)`, and ranges
   *  across connections never overlap. */
  openDisplay(rawModule: { ccall: (...args: unknown[]) => unknown }): { connId: number; xidBase: number; xidMask: number };
  /** XCloseDisplay entry. Host drops the connection and releases any
   *  windows / pixmaps / atoms it owned. */
  closeDisplay(connId: number): void;
  /** Shared root window's XID. Every client's XOpenDisplay asks Host
   *  for this rather than creating a per-connection root; one root,
   *  one set of compositor entries, one place to hang the
   *  SubstructureRedirect holder in Step 3. */
  getRootWindow(): number;
  /** XQueryTree cross-conn: enumerate mapped children of `parent` in
   *  bottom-to-top stacking order. Returns [] for an unknown / leaf
   *  parent. Used by twm's RestartPreviousState walk after F_RESTART
   *  respawn so the new wasm finds the still-mapped xeyes/xcalc
   *  shells (xorg's XQueryTree would return them; our stub used to
   *  return 0, leaving twm convinced root was empty). */
  getWindowChildren(parent: number): number[];
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
    /** Background state, mirroring xserver's backgroundState
     *  (xserver/dix/window.c around line 1185):
     *    0 = None             (no auto-paint)
     *    1 = BackgroundPixel  (bgValue is a pixel)
     *    2 = BackgroundPixmap (bgValue is a Pixmap XID)
     *  ParentRelative is not yet plumbed; XCreateWindow collapses it
     *  to None at the C bridge. */
    bgType: number,
    bgValue: number,
  ): void;
  onWindowSetBorder(id: number, borderWidth: number, borderPixel: number): void;
  /** Update the window's background state. bgType matches onWindowCreate. */
  onWindowSetBg(id: number, bgType: number, bgValue: number): void;
  /** Geometry-only update for an existing window (XMoveWindow /
   *  XResizeWindow / XConfigureWindow). Leaves parent, shape, and
   *  background_pixmap alone. `connId` is the caller's connection so
   *  Host can avoid double-delivering a ConfigureNotify back to the
   *  caller when caller == owner (Tk/Xt resizing their own toplevel). */
  onWindowConfigure(connId: number, id: number, x: number, y: number, w: number, h: number): void;
  /** XMapWindow entry. `connId` is the caller's connection so Host can
   *  enforce the SubstructureRedirect "caller == holder bypass" rule
   *  (x11protocol.txt §1592). Host internally decides whether to
   *  actually map (no redirect / OR set / caller == holder) or to
   *  synthesize MapRequest to the redirect holder. */
  onWindowMap(connId: number, id: number): void;
  onWindowUnmap(connId: number, id: number): void;
  onWindowDestroy(id: number): void;
  /** XRaiseWindow: move window to top of sibling stacking order. */
  onWindowRaise(id: number): void;
  /** XSelectInput mirror. Host stores the mask per (window, caller)
   *  and enforces at-most-one SubstructureRedirectMask per window
   *  (x11protocol.txt §1477). */
  onSelectInput(connId: number, id: number, mask: number): void;
  /** XChangeWindowAttributes(CWBitGravity) mirror. Controls how the
   *  window's backing pixmap is treated on resize: NorthWestGravity (1)
   *  preserves the top-left, ForgetGravity (0, default) discards and
   *  re-Exposes the entire content rect. Without this, Xaw widgets
   *  (default ForgetGravity) doubled their labels on resize. */
  onWindowSetBitGravity(id: number, gravity: number): void;
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
  /** XGrabButton -> install a passive button grab. host walks parent
   *  chain at ButtonPress time and redirects to the deepest grab window.
   *  AnyButton (0) / AnyModifier (1<<15) act as wildcards. ownerEvents,
   *  eventMask, pointerMode, keyboardMode, confineTo, cursor are accepted
   *  for signature fidelity; the host's minimal impl honours only the
   *  routing (no sync-mode replay queue, so XAllowEvents is a stub). */
  onGrabButton(
    window: number,
    button: number,
    modifiers: number,
    ownerEvents: boolean,
    eventMask: number,
    pointerMode: number,
    keyboardMode: number,
    confineTo: number,
    cursor: number,
  ): void;
  /** XUngrabButton -> remove a passive button grab. AnyButton / AnyModifier
   *  act as wildcards in the matching axis (xorg semantics). */
  onUngrabButton(window: number, button: number, modifiers: number): void;
  /** XGrabPointer / XUngrabPointer: active pointer grab on behalf of
   *  `connId`'s wasm. Subsequent button + motion events route there. */
  onGrabPointer(connId: number, window: number, ownerEvents: boolean): void;
  onUngrabPointer(): void;
  /** Defer pointer-window repoll across one event-loop tick. C-side
   *  XMapWindow / XUnmapWindow request this when state actually
   *  changed; the host setTimeout(0)s a ccall back into the named
   *  conn's `emx11_repoll_pointer_window_now` so the synthetic
   *  crossings hit the queue only after the caller's wasm dispatch
   *  has unwound. Coalesces bursts on the same conn. */
  onScheduleRepoll(connId: number): void;
  /** XSetInputFocus on any module. Forwarded so press-driven focus
   *  override can stop tracking the WM frame after a click-to-raise. */
  onSetInputFocus(window: number): void;
  /** XIM (xim.c): Tk's XSetICFocus / XUnsetICFocus / XSetICValues
   *  XNSpotLocation. Host translates these into hidden-textarea focus
   *  + position so the OS IME anchors candidate windows correctly. */
  onXimSetFocus(window: number): void;
  onXimClearFocus(): void;
  onXimSetSpot(window: number, x: number, y: number): void;

  /** Tcl notifier setTimerProc bridge (libemx11/notifier.c). `ms < 0`
   *  means Tcl passed timePtr == NULL ("no timer"); otherwise schedule
   *  a pump wake at +ms relative. */
  onTclSetTimer(ms: number): void;
  /** Tcl notifier alertNotifierProc bridge. Standardised "wake the
   *  event loop now" primitive (analogue of writing to a self-pipe
   *  fd to break a blocking select). */
  onTclAlertNotifier(): void;

  /** Wasm-side `execvp` (process.c weak override, currently linked into
   *  twm only). Host stashes the request and a respawn handler
   *  registered by ProcessImpl picks it up after the wasm exits, so
   *  twm's F_RESTART produces a fresh Module instance with the supplied
   *  argv -- the wasm analogue of Linux's fork-less exec. */
  onExecSelf(connId: number, argv: string[]): void;
}

declare global {
  /**
   * Emscripten Module globals that em-x11 writes into.
   * In MODULARIZE=0 builds Module is a global; in MODULARIZE=1
   * it's closure-local.  Host.attachToBridge() writes through
   * `typeof Module !== 'undefined'` guard so both paths work.
   */
  // eslint-disable-next-line no-var
  var Module: {
    emx11Host?: import('./emscripten.js').EmX11Host;
    emx11Caches?: {
      measureCtx?: CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D | null;
      fontCache?: Map<string, { ascent: number; descent: number; maxW: number; widths: Int32Array }>;
      textCache?: Map<string, number>;
      propStash?: Uint8Array | null;
      shapeStash?: { id: number; rects: import('./emscripten.js').ShapeRect[] } | null;
      childrenStash?: { parent: number; kids: number[] } | null;
    };
    emx11Debug?: {
      traceHit: boolean;
      traceHitNext: boolean;
      traceMotion: boolean;
      traceButton: boolean;
      tracePaint: boolean;
      traceCBtn: boolean;
      traceCMot: boolean;
      traceMove: boolean;
      traceQp: boolean;
    };
    emx11ClipboardBytes?: Uint8Array | null;
    specialHTMLTargets?: Record<string, unknown>;
  };
}

// The Host is passed via Module['emx11Host'] (flat Module property,
// per emscripten convention).  See library_emx11.js $EmX11Host.init().
