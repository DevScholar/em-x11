/**
 * Host facade: the single object every wasm client (and every demo
 * harness) talks to. Implements the EmX11Host interface used by the
 * --js-library bridges in src/bindings/*.js, plus the launchClient
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
 * Host installs itself on globalThis.__EMX11__ via install(). That must
 * happen BEFORE any wasm module starts: bindings/*.js read the global
 * synchronously from C calls.
 */

import { RootCanvas } from '../runtime/canvas.js';
import type { RootCanvasOptions } from '../runtime/canvas.js';
import { Renderer } from './render/index.js';
import { AtomManager } from './atom.js';
import { PropertyManager } from './property.js';
import { EventDispatcher } from './events.js';
import { ConnectionManager } from './connection.js';
import { WindowManager } from './window.js';
import { GcManager } from './gc.js';
import { InputBridge } from './devices.js';
import type { LoadOptions } from '../loader/wasm.js';
import type {
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

    this.window.installSharedRoot();
  }

  install(): void {
    globalThis.__EMX11__ = this;
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
    const attrs = this.renderer.attrsOf(id);
    if (!attrs) return null;
    const win = this.renderer.windows.get(id);
    if (!win) return null;
    /* renderer.absOrigin imported via window-tree; use the same fn the
     * paint path uses so input and pixels stay in lockstep. */
    let ax = win.x;
    let ay = win.y;
    let pid = win.parent;
    for (let i = 0; pid !== 0 && i < 32; i++) {
      const p = this.renderer.windows.get(pid);
      if (!p) break;
      ax += p.x;
      ay += p.y;
      pid = p.parent;
    }
    return { ax, ay };
  }

  /* -- EmX11Host: window structure -------------------------------------- */

  onWindowCreate(
    connId: number, id: number, parent: number,
    x: number, y: number, width: number, height: number,
    borderWidth: number, borderPixel: number, background: number,
  ): void {
    this.window.onCreate(connId, id, parent, x, y, width, height, borderWidth, borderPixel, background);
  }
  onWindowSetBorder(id: number, borderWidth: number, borderPixel: number): void {
    this.window.onSetBorder(id, borderWidth, borderPixel);
  }
  onWindowSetBg(id: number, background: number): void {
    this.window.onSetBg(id, background);
  }
  onWindowSetBgPixmap(id: number, pmId: number): void {
    this.window.onSetBgPixmap(id, pmId);
  }
  onWindowConfigure(id: number, x: number, y: number, w: number, h: number): void {
    this.window.onConfigure(id, x, y, w, h);
  }
  onWindowMap(connId: number, id: number): void { this.window.onMap(connId, id); }
  onWindowUnmap(connId: number, id: number): void { this.window.onUnmap(connId, id); }
  onWindowDestroy(id: number): void { this.window.onDestroy(id); }
  onSelectInput(connId: number, id: number, mask: number): void {
    this.events.onSelectInput(connId, id, mask);
  }
  onSetOverrideRedirect(id: number, flag: boolean): void {
    this.window.onSetOverrideRedirect(id, flag);
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
