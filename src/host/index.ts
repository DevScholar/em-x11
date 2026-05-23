/**
 * Host facade: the single object every wasm client (and every demo
 * harness) talks to. Implements the EmX11Host interface used by the
 * EM_JS bridges in native/emx11/bridges.c, plus the launchClient
 * coordination that dev demos use to start each wasm.
 *
 * Internally Host is a thin coordinator over a set of manager classes,
 * one per concern. The split mirrors xserver's dix/ subdirectory layout:
 *
 *   atom.ts        -- xserver/dix/atom.c (XInternAtom, XGetAtomName)
 *   property.ts    -- xserver/dix/property.c (Change/Get/Delete/List)
 *   window.ts      -- xserver/dix/window.c (Create/Map/Destroy/Reparent)
 *   events.ts      -- xserver/dix/events.c (XSelectInput, redirect, Expose)
 *   connection.ts  -- per-client state + XID range allocation
 *   devices.ts     -- xserver/dix/devices.c (mouse + keyboard delivery)
 *   gc.ts          -- xserver/dix/gc.c (drawing) + dix/pixmap.c (lifecycle)
 *   render/        -- xserver/mi/ software renderer (window tree, paint)
 *
 * No real X server is implemented here -- we shim only enough server
 * semantics that Tk / Xt / twm linking against real Xlib think they
 * have one. See src/host/README in xserver-counterpart comments for
 * what each method's authoritative source-of-truth is.
 *
 * Host installs itself on globalThis.emX11._bridge via attachToBridge().
 * That must happen BEFORE any wasm module starts: the EM_JS bridges
 * read `globalThis.emX11._bridge` synchronously from C calls.
 *
 * `globalThis.emX11` is the single namespace owned by this package;
 * everything em-x11 puts on the global object lives under it (no
 * scattered `__EMX11_*` globals). `_bridge` is the C-facing facade
 * (this Host); `_caches` holds bridge-owned scratch maps; `_debug`
 * holds runtime trace flags. Public-facing surfaces (`fs`, `spawn`,
 * `display`, `debug`) sit unprefixed on the same object and are
 * wired up by createEmX11() in src/api/.
 */

import { RootCanvas } from '../runtime/canvas.js';
import type { RootCanvasOptions } from '../runtime/canvas.js';
import { Renderer } from './render/index.js';
import { absOrigin } from './render/window-tree.js';
import { AtomManager } from './atom.js';
import { PropertyManager } from './property.js';
import { EventDispatcher } from './events.js';
import { ConnectionManager } from './connection.js';
import { WindowManager } from './window.js';
import { GcManager } from './gc.js';
import { InputBridge } from './devices.js';
import { GrabManager } from './grabs.js';
import { GlxManager } from './glx.js';
import { TextInputOverlay } from './text-input.js';
import { KeyboardLayoutManager } from './keyboard-layout.js';
import { KeyboardLockManager } from './keyboard-lock.js';
import type { LoadOptions } from '../loader/wasm.js';
import type {
  EmX11Global,
  EmX11Host,
  EmscriptenModule,
  Point,
  ShapeRect,
} from '../types/emscripten.js';

export type HostOptions = RootCanvasOptions;

export class Host implements EmX11Host {
  readonly canvas: RootCanvas;
  readonly renderer: Renderer;
  /* Managers. Order in the field declarations doesn't matter; the
   * constructor body below sets them up in dependency order. */
  readonly atom: AtomManager;
  readonly property: PropertyManager;
  readonly events: EventDispatcher;
  readonly connection: ConnectionManager;
  readonly window: WindowManager;
  readonly gc: GcManager;
  readonly devices: InputBridge;
  readonly grabs: GrabManager;
  readonly glx: GlxManager;
  readonly textInput: TextInputOverlay;
  /** Layout-aware keymap manager. Fetches navigator.keyboard.getLayoutMap()
   *  once and pushes the layout into each wasm process before its first
   *  KeyPress fires. See keyboard-layout.ts. */
  readonly keyboardLayout: KeyboardLayoutManager;
  /** OS-key capture while fullscreen. Opt-in; off until a demo calls
   *  host.keyboard.lock.enable(). See keyboard-lock.ts. */
  readonly keyboardLock: KeyboardLockManager;

  constructor(options: HostOptions = {}) {
    this.canvas = new RootCanvas(options);

    /* Construction order matters when a manager calls a sibling at
     * construction time. Most don't -- they only reach into siblings
     * at request-handling time, by which point all fields are set --
     * so the order below is mostly readability:
     *
     *   1. AtomManager has no deps.
     *   2. GcManager owns the pixmaps map; the renderer's pixmapLookup
     *      is a closure that reads it at paint time.
     *   3. Renderer takes the lookup closure and the canvas.
     *   4. Property/Events/Connection/Window all take the host ref;
     *      they reach siblings only inside method bodies.
     *   5. InputBridge attaches DOM listeners in its constructor and
     *      reads canvas/renderer state, so it goes last.
     *   6. Finally, WindowManager.installSharedRoot creates the root
     *      window + weave background.
     */
    this.atom = new AtomManager();
    this.gc = new GcManager(this);
    this.renderer = new Renderer(this.canvas, (id) => this.gc.pixmapCanvas(id));
    this.property = new PropertyManager(this);
    this.events = new EventDispatcher(this);
    this.connection = new ConnectionManager(this);
    this.window = new WindowManager(this);
    this.devices = new InputBridge(this);
    this.grabs = new GrabManager(this);
    this.glx = new GlxManager(this.renderer);
    /* Hidden-textarea overlay for XIM. Constructor is a no-op in
     * headless / OffscreenCanvas mode -- pyodide-tk worker path drives
     * input through devices.push* directly and has no DOM. */
    this.textInput = new TextInputOverlay(this);
    this.devices.registerOverlay(
      // Reach in through a typed accessor that returns the underlying
      // <textarea> when available, null otherwise.
      this.textInput.element,
    );
    this.keyboardLayout = new KeyboardLayoutManager();
    this.keyboardLock = new KeyboardLockManager();
    /* Fire-and-forget the getLayoutMap() fetch so the Promise is hot by
     * the time the first wasm process connects -- it'll await on the
     * same Promise. No-op (returns empty map) when the Keyboard API
     * isn't available. */
    void this.keyboardLayout.resolve();

    this.window.installSharedRoot();
  }

  /** Tear down DOM state owned by this Host (DOM listeners on
   *  window/document/canvas, the hidden IME textarea). Safe to call
   *  on a headless / remote-mode Host (downstream dispose() are
   *  no-ops there). Idempotent. Use when reloading a Host (HMR,
   *  multi-canvas test harness) so previous-instance closures stop
   *  pinning the prior `this`. */
  dispose(): void {
    this.devices.dispose();
    this.textInput.dispose();
    this.keyboardLock.disable();
  }

  /** Install this Host as the bridge facade under `globalThis.emX11._bridge`,
   *  so EM_JS bodies in native/emx11/bridges.c can reach it synchronously.
   *  Allocates the singleton if needed and never overwrites unrelated
   *  surfaces (fs, spawn, display, debug) that createEmX11 may have
   *  already attached.
   *
   *  Must be called BEFORE any wasm module starts. createEmX11() does
   *  this for you; legacy callers that constructed Host directly should
   *  invoke it manually. */
  attachToBridge(): void {
    const slot = (globalThis.emX11 ??= {} as EmX11Global);
    slot._bridge = this;
    if (!slot._caches) slot._caches = {};
    if (!slot._debug) {
      slot._debug = {
        traceHit: false,
        traceHitNext: false,
        traceMotion: false,
        traceButton: false,
        tracePaint: false,
        traceCBtn: false,
        traceCMot: false,
        traceMove: false,
        traceQp: false,
      };
    }
  }

  launchClient(opts: LoadOptions): Promise<{ connId: number; module: EmscriptenModule }> {
    return this.connection.launchClient(opts);
  }

  waitForSubstructureRedirect(winId: number, timeoutMs?: number): Promise<number> {
    return this.events.waitForSubstructureRedirect(winId, timeoutMs);
  }

  /* -- EmX11Host: lifecycle --------------------------------------------- */

  onInit(screenWidth: number, screenHeight: number): void {
    this.gc.onInit(screenWidth, screenHeight);
  }
  openDisplay(): { connId: number; xidBase: number; xidMask: number } {
    return this.connection.open();
  }
  closeDisplay(connId: number): void {
    this.connection.close(connId);
  }
  getRootWindow(): number {
    return this.window.getRootWindow();
  }
  getWindowChildren(parent: number): number[] {
    return this.window.childrenOf(parent);
  }
  getPointerXY(): Point {
    return this.devices.getPointerXY();
  }
  getWindowAttrs(id: number): {
    x: number; y: number; width: number; height: number;
    mapped: boolean; overrideRedirect: boolean; borderWidth: number;
  } | null {
    return this.window.getAttrs(id);
  }

  /** Authoritative cumulative absolute origin for a window, computed
   *  from Host's full tree. C-side per-display tables only see windows
   *  that connection created, so a reparented client (e.g. xcalc shell
   *  under a twm-owned frame) walks its parent chain into a None and
   *  treats its shell's recorded local position as absolute -- which
   *  loses the frame's offset and lands every Motion/ButtonPress event
   *  at coords offset by `frame.position`. The resulting symptom: hover
   *  highlights the wrong widget, Xt's button text disappears (the
   *  *wrong* widget gets ClearArea+Expose), xeyes' SHAPE clears the
   *  eye sockets to background pixel.
   *
   *  Returning {ax, ay} from the renderer's full tree closes the gap.
   *  C-side window_abs_origin uses this as a fallback when the parent
   *  isn't in its own table. */
  getWindowAbsOrigin(id: number): { ax: number; ay: number } | null {
    const win = this.renderer.windows.get(id);
    if (!win) return null;
    /* Same fn the paint path uses, so input and pixels stay in lockstep. */
    return absOrigin(this.renderer, win);
  }

  /** Cross-connection bounding-shape lookup. Returns:
   *    null  -- window unknown to the host
   *    []    -- window known, but no bounding shape (rectangular)
   *    [..]  -- the rects (window-local coords), as last set via XShape*
   *  twm uses this so XShapeQueryExtents on a foreign client (xeyes)
   *  reports boundingShaped=true and XShapeCombineShape can copy the
   *  source rects onto twm's frame. Without this the WM frame stays
   *  rectangular and clicks in the shape's hole hit the frame instead
   *  of passing through to whatever's behind. */
  getWindowShape(id: number): ShapeRect[] | null {
    const win = this.renderer.windows.get(id);
    if (!win) return null;
    return win.shape ? win.shape.slice() : [];
  }

  /* -- EmX11Host: window structure -------------------------------------- */

  onWindowCreate(
    connId: number, id: number, parent: number,
    x: number, y: number, width: number, height: number,
    borderWidth: number, borderPixel: number,
    bgType: number, bgValue: number,
  ): void {
    this.window.onCreate(connId, id, parent, x, y, width, height, borderWidth, borderPixel, bgType, bgValue);
  }
  onWindowSetBorder(id: number, borderWidth: number, borderPixel: number): void {
    this.window.onSetBorder(id, borderWidth, borderPixel);
  }
  onWindowSetBg(id: number, bgType: number, bgValue: number): void {
    this.window.onSetBg(id, bgType, bgValue);
  }
  onWindowSetBgPixmap(id: number, pmId: number): void {
    this.window.onSetBgPixmap(id, pmId);
  }
  onWindowConfigure(connId: number, id: number, x: number, y: number, w: number, h: number): void {
    this.window.onConfigure(connId, id, x, y, w, h);
  }
  onWindowMap(connId: number, id: number): void { this.window.onMap(connId, id); }
  onWindowUnmap(connId: number, id: number): void { this.window.onUnmap(connId, id); }
  onWindowDestroy(id: number): void { this.window.onDestroy(id); }
  onWindowRaise(id: number): void { this.window.onRaise(id); }
  onSelectInput(connId: number, id: number, mask: number): void {
    this.events.onSelectInput(connId, id, mask);
  }
  onShapeSelectInput(connId: number, window: number, mask: number): void {
    this.events.onShapeSelectInput(connId, window, mask);
  }
  onSetOverrideRedirect(id: number, flag: boolean): void {
    this.window.onSetOverrideRedirect(id, flag);
  }
  onWindowSetBitGravity(id: number, gravity: number): void {
    this.window.onSetBitGravity(id, gravity);
  }
  onWindowSetCursor(id: number, cursor: number): void {
    this.window.onSetCursor(id, cursor);
  }
  onSetGrabCursor(cursor: number): void {
    this.devices.setGrabCursor(cursor);
  }
  onReparentWindow(id: number, parent: number, x: number, y: number): void {
    this.window.onReparent(id, parent, x, y);
  }

  /* -- EmX11Host: GC drawing -------------------------------------------- */

  onClearArea(id: number, x: number, y: number, w: number, h: number): void {
    this.gc.onClearArea(id, x, y, w, h);
  }
  onFillRect(id: number, x: number, y: number, w: number, h: number, color: number): void {
    this.gc.onFillRect(id, x, y, w, h, color);
  }
  onDrawLine(
    id: number, x1: number, y1: number, x2: number, y2: number,
    color: number, lineWidth: number,
  ): void {
    this.gc.onDrawLine(id, x1, y1, x2, y2, color, lineWidth);
  }
  onDrawArc(
    id: number, x: number, y: number, w: number, h: number,
    angle1: number, angle2: number, color: number, lineWidth: number,
  ): void {
    this.gc.onDrawArc(id, x, y, w, h, angle1, angle2, color, lineWidth);
  }
  onFillArc(
    id: number, x: number, y: number, w: number, h: number,
    angle1: number, angle2: number, color: number,
  ): void {
    this.gc.onFillArc(id, x, y, w, h, angle1, angle2, color);
  }
  onFillPolygon(id: number, points: Point[], shape: number, mode: number, color: number): void {
    this.gc.onFillPolygon(id, points, shape, mode, color);
  }
  onDrawPoints(id: number, points: Point[], mode: number, color: number): void {
    this.gc.onDrawPoints(id, points, mode, color);
  }
  onDrawString(
    id: number, x: number, y: number, font: string, text: string,
    fgColor: number, bgColor: number, imageMode: number,
  ): void {
    this.gc.onDrawString(id, x, y, font, text, fgColor, bgColor, imageMode);
  }
  onFlush(): void { this.gc.onFlush(); }
  onWindowShape(id: number, rects: ShapeRect[]): void { this.gc.onWindowShape(id, rects); }

  /* -- EmX11Host: passive button grab dispatch -------------------------- */

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
  ): void {
    this.grabs.add(
      window, button, modifiers, ownerEvents, eventMask,
      pointerMode, keyboardMode, confineTo, cursor,
    );
  }
  onUngrabButton(window: number, button: number, modifiers: number): void {
    this.grabs.remove(window, button, modifiers);
  }
  /** XGrabPointer / XUngrabPointer: install or release an active
   *  pointer grab on behalf of the calling wasm. Forwards to the input
   *  bridge, which redirects subsequent button + motion delivery. */
  onGrabPointer(connId: number, window: number, _ownerEvents: boolean): void {
    this.devices.setActivePointerGrab(connId, window);
  }
  onUngrabPointer(): void {
    this.devices.clearActivePointerGrab();
  }

  /** Defer-and-coalesce pointer-window repoll, scheduled by C-side
   *  XMapWindow / XUnmapWindow on state change. Bouncing through a
   *  microtask breaks the synchronous map → crossing → handle → map
   *  chain that wedged twm: the actual repoll runs only after the
   *  caller's wasm dispatch has unwound, so any logic chained off the
   *  new crossing happens on a fresh dispatch frame. Microtask (vs
   *  setTimeout(0)) fires before the next macrotask -- sooner than
   *  browser-clamped setTimeout minimum -- so twm's menu pop-up case
   *  (XMapWindow(menu) immediately followed by entering the menu loop
   *  that gates UpdateMenu on `mr->entered`) still gets its EnterNotify
   *  before the first UpdateMenu pass. Per-conn coalescing collapses a
   *  Tk widget-realize burst (N maps in one frame) into a single repoll.
   *
   *  We resolve the window-under-pointer here on the host side and pass
   *  it as a hint to the C side, instead of letting C re-run hit-test
   *  via dpy->windows[]. The C-side hit_test in event.c walks by tree
   *  depth + first-found and ignores stack order, so it picks the wrong
   *  sibling when twm raises a decoration above its primary window
   *  (root menu's shadow is XRaiseWindow'd before menu->w is mapped:
   *  shadow created earlier → first-found returns shadow even though
   *  host's stack-order-aware findWindowAt correctly puts menu on top).
   *  emit_crossing's mask check then drops the Enter on shadow because
   *  shadow doesn't select EnterWindowMask, twm's ActiveMenu->entered
   *  never flips, UpdateMenu's gate at menus.c:512 spins, no item ever
   *  highlights. Trusting the host's hit-test closes the gap. */
  private repollScheduled = new Set<number>();
  onScheduleRepoll(connId: number): void {
    if (this.repollScheduled.has(connId)) return;
    this.repollScheduled.add(connId);
    queueMicrotask(() => {
      this.repollScheduled.delete(connId);
      const conn = this.connection.get(connId);
      const mod = conn?.module;
      if (!mod) return;
      const { x, y } = this.devices.getPointerXY();
      const cur = this.renderer.findWindowAt(x, y) ?? 0;
      try {
        mod.ccall(
          'emx11_repoll_pointer_window_hint_now',
          null,
          ['number'],
          [cur],
        );
      } catch {
        /* swallow: a torn-down conn is benign here */
      }
    });
  }

  /** XSetInputFocus from any module. The WM uses this to hand keyboard
   *  control off to a client whose own window did NOT subscribe to
   *  ButtonPressMask -- without this, our press-driven focus tracker
   *  would still point at the WM frame and keys would never reach the
   *  app. */
  onSetInputFocus(window: number): void {
    this.devices.setExplicitFocus(window);
  }

  /* -- exec-self (twm F_RESTART) ---------------------------------------- */

  /** Per-connection respawn handlers, populated by ProcessImpl when its
   *  connection lands and consumed when the wasm calls execvp via the
   *  process.c override. The handler tears down + re-launches into the
   *  same Process handle so callers that hold a `Process` reference
   *  (launchTwm's awaiter) keep their handle valid across the restart. */
  private execHandlers = new Map<number, (argv: string[]) => void>();

  registerExecHandler(connId: number, handler: (argv: string[]) => void): void {
    this.execHandlers.set(connId, handler);
  }
  unregisterExecHandler(connId: number): void {
    this.execHandlers.delete(connId);
  }

  onExecSelf(connId: number, argv: string[]): void {
    const handler = this.execHandlers.get(connId);
    if (!handler) {
      console.warn(`[emx11] onExecSelf: no handler for conn ${connId}`);
      return;
    }
    /* Defer to a fresh macrotask so the respawn launches OUTSIDE the
     * Asyncify unwind triggered by the wasm's exit(): boot of the new
     * Module needs a clean JS stack, not one held mid-rewind by the
     * dying wasm. */
    setTimeout(() => {
      try {
        handler(argv);
      } catch (err) {
        console.error('[emx11] exec-self respawn failed:', err);
      }
    }, 0);
  }

  /* -- EmX11Host: XIM (xim.c) ------------------------------------------- */

  /** XSetICFocus on any module. Tk fires this when its focus moves
   *  between entries / texts. The hidden textarea overlay grabs DOM
   *  focus and starts following the caret. */
  onXimSetFocus(window: number): void {
    this.textInput.setFocus(window);
  }
  onXimClearFocus(): void {
    this.textInput.clearFocus();
  }
  /** XSetICValues(XNSpotLocation): caret moved inside the focused widget.
   *  (window-local pixels). Overlay translates to viewport coords and
   *  repositions itself so the OS IME candidate window stays anchored. */
  onXimSetSpot(window: number, x: number, y: number): void {
    this.textInput.setSpot(window, x, y);
  }

  /* -- EmX11Host: Tcl notifier wake signals ---------------------------- */

  /** Wake target for the Tcl notifier. Hosts register their pump's
   *  scheduler here (typically via emX11.display.installEventLoopWake).
   *  When unset, setTimer/alert bridges from libemx11 are no-ops --
   *  callers without a pump (test harnesses, demos that never enter
   *  Tk's event loop) don't need to install anything. */
  private eventLoopWake: { onTimer: (ms: number) => void; onAlert: () => void } | null = null;

  installEventLoopWake(wake: { onTimer: (ms: number) => void; onAlert: () => void } | null): void {
    this.eventLoopWake = wake;
  }

  /** Bridge target for libemx11/notifier.c real_SetTimer. ms < 0 means
   *  "no timer scheduled" (Tcl passed timePtr == NULL); otherwise
   *  schedule a pump wake at +ms relative. The host pump translates
   *  this to a real setTimeout / Atomics.wait timeout as appropriate. */
  onTclSetTimer(ms: number): void {
    this.eventLoopWake?.onTimer(ms);
  }

  /** Bridge target for libemx11/notifier.c real_AlertNotifier. Tcl's
   *  cross-thread "break out of waitForEvent" primitive. Hosts use
   *  this to drive an immediate drain regardless of the current timer
   *  schedule. */
  onTclAlertNotifier(): void {
    this.eventLoopWake?.onAlert();
  }


  /* -- EmX11Host: pixmaps + drawable copies + Shape --------------------- */

  onPixmapCreate(id: number, width: number, height: number, depth: number): void {
    this.gc.onPixmapCreate(id, width, height, depth);
  }
  onPixmapDestroy(id: number): void { this.gc.onPixmapDestroy(id); }
  onShapeCombineMask(
    destId: number, srcId: number, xOff: number, yOff: number, op: number,
  ): void {
    this.gc.onShapeCombineMask(destId, srcId, xOff, yOff, op);
  }
  onCopyArea(
    srcId: number, dstId: number, srcX: number, srcY: number,
    w: number, h: number, dstX: number, dstY: number,
  ): void {
    this.gc.onCopyArea(srcId, dstId, srcX, srcY, w, h, dstX, dstY);
  }
  onCopyPlane(
    srcId: number, dstId: number, srcX: number, srcY: number,
    w: number, h: number, dstX: number, dstY: number,
    plane: number, fg: number, bg: number, applyBg: boolean,
  ): void {
    this.gc.onCopyPlane(srcId, dstId, srcX, srcY, w, h, dstX, dstY, plane, fg, bg, applyBg);
  }
  onPutImage(
    dstId: number, dstX: number, dstY: number, w: number, h: number,
    format: number, depth: number, bytesPerLine: number,
    data: Uint8Array, fg: number, bg: number,
  ): void {
    this.gc.onPutImage(dstId, dstX, dstY, w, h, format, depth, bytesPerLine, data, fg, bg);
  }

  /* -- EmX11Host: atoms + properties ------------------------------------ */

  internAtom(name: string, onlyIfExists: boolean): number {
    return this.atom.intern(name, onlyIfExists);
  }
  getAtomName(atom: number): string | null {
    return this.atom.nameOf(atom);
  }
  changeProperty(
    window: number, atom: number, type: number,
    format: 8 | 16 | 32, mode: number,
    data: Uint8Array,
  ): boolean {
    return this.property.change(window, atom, type, format, mode, data);
  }
  peekProperty(
    window: number, atom: number, reqType: number,
    longOffset: number, longLength: number, deleteFlag: boolean,
  ): {
    found: boolean; type: number; format: number;
    nitems: number; bytesAfter: number; data: Uint8Array;
  } | null {
    return this.property.peek(window, atom, reqType, longOffset, longLength, deleteFlag);
  }
  deleteProperty(window: number, atom: number): void {
    this.property.delete(window, atom);
  }
  listProperties(window: number): number[] {
    return this.property.list(window);
  }
}
