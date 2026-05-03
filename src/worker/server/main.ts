/**
 * Server Worker: runs the Host + Renderer + OffscreenCanvas inside a
 * dedicated thread. xorg-analog: this is the server process
 * (dix/dispatch.c + fb/* renderer).
 *
 * Architecture:
 *   - One `Host` instance owns the canvas, window tree, connection
 *     registry, atom/property tables. All existing src/host/ modules
 *     are reused verbatim -- only the canvas surface is OffscreenCanvas
 *     (already supported by RootCanvas).
 *   - Each Client Worker port gets a `PortModuleSurface` that
 *     translates Module.ccall('emx11_push_*_event', ...) into
 *     `XEvent.*` postMessage. Host.connection.bindModule installs it
 *     as the conn's module, so Host's existing dispatch (events.ts /
 *     window.ts / devices.ts) routes automatically.
 *   - Incoming RPCs (Draw.*, Window.*, Atom.*, Property.*) translate
 *     to matching Host.onXxx calls.
 *   - DOM input from main thread (Dom.Mouse*, Dom.Key*) enters via
 *     Host.devices.pushMouseXxx which already hit-tests + routes via
 *     Module.ccall -- see src/host/devices.ts headless path.
 *   - SAB mirror for hot sync queries: after any structural change,
 *     resync `winAttr` + `absOrigin` slots so Client Workers' local
 *     SAB reads stay coherent.
 */

/// <reference lib="webworker" />

import { Host } from '../../host/index.js';
import type { ModuleCcallSurface } from '../../host/connection.js';
import { RpcChannel } from '../rpc/channel.js';
import type {
  BootstrapServer,
  ConnectServerClientPort,
  AllocateConnIdResp,
  DomMouseMove,
  DomMouseDown,
  DomMouseUp,
  DomKeyDown,
  DomKeyUp,
  DomFocusChange,
  ClientToServerVoid,
  ClientToServerCall,
  ServerToClientSlot,
} from '../rpc/protocol.js';
import {
  attachSab,
  clearWinSlot,
  writeAbsOrigin,
  writePointer,
  writeWinAttr,
  SAB_MAX_WINDOW_SLOTS,
  type SabViews,
} from '../rpc/sab.js';

declare const self: DedicatedWorkerGlobalScope;

interface ServerState {
  host: Host;
  sab: SabViews;
  mainChannel: RpcChannel;
  /** Per-connId RPC channel + port. Created on ConnectServerClientPort. */
  clientChannels: Map<number, RpcChannel>;
  clientPorts: Map<number, MessagePort>;
  /** Per-connId PortModuleSurface we installed via bindModule. */
  clientSurfaces: Map<number, PortModuleSurface>;
  /** SAB slot allocator. slot 0 reserved (means "not mirrored"). */
  xidToSlot: Map<number, number>;
  slotToXid: Map<number, number>;
  nextSlot: number;
  freedSlots: number[];
}

let state: ServerState | null = null;

self.addEventListener('message', bootstrapOnce);

function bootstrapOnce(ev: MessageEvent): void {
  const data = ev.data as BootstrapServer;
  if (!data || data.kind !== 'BootstrapServer') return;
  self.removeEventListener('message', bootstrapOnce);

  /* Set OffscreenCanvas size explicitly (main thread set it too, but
   * the transfer may have reset the backing-store attrs). */
  data.canvas.width = data.cssWidth;
  data.canvas.height = data.cssHeight;

  const host = new Host({
    surface: data.canvas,
    width: data.cssWidth,
    height: data.cssHeight,
  });
  /* Do NOT host.install() -- we don't want globalThis.__EMX11__ in
   * the server worker. Bridges (legacy mode) would start misfiring
   * into our Host if it were set. */

  const sab = attachSab(data.sab);
  const mainChannel = new RpcChannel(self);

  state = {
    host,
    sab,
    mainChannel,
    clientChannels: new Map(),
    clientPorts: new Map(),
    clientSurfaces: new Map(),
    xidToSlot: new Map(),
    slotToXid: new Map(),
    nextSlot: 1,       /* slot 0 reserved */
    freedSlots: [],
  };

  wireMainChannel(state);
  console.log('[emx11:server] bootstrap complete', {
    w: data.cssWidth, h: data.cssHeight,
  });
}

/* -- Main-thread channel: DOM input + conn allocation --------------------- */

function wireMainChannel(s: ServerState): void {
  const { mainChannel, host, sab } = s;

  /* DOM input path: push through the existing InputBridge (headless
   * mode). It hit-tests via renderer, looks up target module via
   * Host.connection, calls module.ccall — which our PortModuleSurface
   * turns into an XEvent.* postMessage to the target client. */
  mainChannel.on<DomMouseMove>('Dom.MouseMove', (msg) => {
    writePointer(sab, msg.x, msg.y);
    host.devices.pushMouseMove({ x: msg.x, y: msg.y, modifiers: msg.modifiers });
  });
  mainChannel.on<DomMouseDown>('Dom.MouseDown', (msg) => {
    writePointer(sab, msg.x, msg.y);
    host.devices.pushMouseDown({
      x: msg.x, y: msg.y, button: msg.button, modifiers: msg.modifiers,
    });
  });
  mainChannel.on<DomMouseUp>('Dom.MouseUp', (msg) => {
    writePointer(sab, msg.x, msg.y);
    host.devices.pushMouseUp({
      x: msg.x, y: msg.y, button: msg.button, modifiers: msg.modifiers,
    });
  });
  mainChannel.on<DomKeyDown>('Dom.KeyDown', (msg) => {
    host.devices.pushKeyDown({
      keysym: msg.keysym, modifiers: msg.modifiers, hasFocus: msg.hasFocus,
    });
  });
  mainChannel.on<DomKeyUp>('Dom.KeyUp', (msg) => {
    host.devices.pushKeyUp({
      keysym: msg.keysym, modifiers: msg.modifiers, hasFocus: msg.hasFocus,
    });
  });
  mainChannel.on<DomFocusChange>('Dom.FocusChange', () => {
    /* No-op for M2; InputBridge doesn't expose a focus setter in
     * headless mode. Can stub later if a client needs exact focus
     * gating. */
  });

  /* Connection allocation: main asks us to allocate a conn, we call
   * Host.connection.open() and echo the values back. */
  mainChannel.on<{ seqId: number }>('AllocateConnId', (msg) => {
    const conn = host.connection.open();
    const reply: AllocateConnIdResp = {
      connId: conn.connId,
      xidBase: conn.xidBase,
      xidMask: conn.xidMask,
      rootWindow: host.getRootWindow(),
    };
    mainChannel.reply(msg.seqId, reply);
  });

  /* Main posted a MessagePort for a specific connId. Install it as
   * that conn's module surface. */
  mainChannel.on<ConnectServerClientPort>('ConnectServerClientPort', (msg) => {
    const port = msg.port;
    const surface = new PortModuleSurface(port);
    host.connection.bindModule(surface, msg.connId);
    s.clientPorts.set(msg.connId, port);
    s.clientSurfaces.set(msg.connId, surface);
    const channel = new RpcChannel(port);
    s.clientChannels.set(msg.connId, channel);
    wireClientChannel(s, msg.connId, channel);
  });
}

/* -- Per-client channel: Draw/Window/Atom/Property RPCs ------------------- */

function wireClientChannel(s: ServerState, connId: number, ch: RpcChannel): void {
  const { host } = s;

  /* Display.Open: client asks for the root window id (the BootstrapClient
   * message could have carried it, but keeping Display.Open as a separate
   * RPC means the client bootstraps without having to know root upfront). */
  ch.on<{ seqId: number }>('Display.Open', (msg) => {
    ch.reply(msg.seqId, { rootWindow: host.getRootWindow() });
  });
  ch.on('Display.Close', () => host.closeDisplay(connId));

  /* Draw primitives: 1-to-1 with existing Host.onXxx. */
  ch.on<ClientToServerVoid & { kind: 'Core.Init' }>('Core.Init', (m) => host.onInit(m.screenWidth, m.screenHeight));
  ch.on('Core.Flush', () => host.onFlush());
  ch.on<ClientToServerVoid & { kind: 'Draw.ClearArea' }>('Draw.ClearArea', (m) =>
    host.onClearArea(m.id, m.x, m.y, m.w, m.h));
  ch.on<ClientToServerVoid & { kind: 'Draw.FillRect' }>('Draw.FillRect', (m) =>
    host.onFillRect(m.id, m.x, m.y, m.w, m.h, m.color));
  ch.on<ClientToServerVoid & { kind: 'Draw.Line' }>('Draw.Line', (m) =>
    host.onDrawLine(m.id, m.x1, m.y1, m.x2, m.y2, m.color, m.lineWidth));
  ch.on<ClientToServerVoid & { kind: 'Draw.Arc' }>('Draw.Arc', (m) =>
    host.onDrawArc(m.id, m.x, m.y, m.w, m.h, m.angle1, m.angle2, m.color, m.lineWidth));
  ch.on<ClientToServerVoid & { kind: 'Draw.FillArc' }>('Draw.FillArc', (m) =>
    host.onFillArc(m.id, m.x, m.y, m.w, m.h, m.angle1, m.angle2, m.color));
  ch.on<ClientToServerVoid & { kind: 'Draw.FillPolygon' }>('Draw.FillPolygon', (m) =>
    host.onFillPolygon(m.id, m.pts, m.shape, m.mode, m.color));
  ch.on<ClientToServerVoid & { kind: 'Draw.Points' }>('Draw.Points', (m) =>
    host.onDrawPoints(m.id, m.pts, m.mode, m.color));
  ch.on<ClientToServerVoid & { kind: 'Draw.String' }>('Draw.String', (m) =>
    host.onDrawString(m.id, m.x, m.y, m.font, m.text, m.fg, m.bg, m.imageMode));
  ch.on<ClientToServerVoid & { kind: 'Draw.PixmapCreate' }>('Draw.PixmapCreate', (m) =>
    host.onPixmapCreate(m.id, m.width, m.height, m.depth));
  ch.on<ClientToServerVoid & { kind: 'Draw.PixmapDestroy' }>('Draw.PixmapDestroy', (m) =>
    host.onPixmapDestroy(m.id));
  ch.on<ClientToServerVoid & { kind: 'Draw.ShapeCombineMask' }>('Draw.ShapeCombineMask', (m) =>
    host.onShapeCombineMask(m.destId, m.srcId, m.xOff, m.yOff, m.op));
  ch.on<ClientToServerVoid & { kind: 'Draw.CopyArea' }>('Draw.CopyArea', (m) =>
    host.onCopyArea(m.srcId, m.dstId, m.srcX, m.srcY, m.w, m.h, m.dstX, m.dstY));
  ch.on<ClientToServerVoid & { kind: 'Draw.CopyPlane' }>('Draw.CopyPlane', (m) =>
    host.onCopyPlane(m.srcId, m.dstId, m.srcX, m.srcY, m.w, m.h, m.dstX, m.dstY, m.plane, m.fg, m.bg, m.applyBg));
  ch.on<ClientToServerVoid & { kind: 'Draw.PutImage' }>('Draw.PutImage', (m) =>
    host.onPutImage(m.dstId, m.dstX, m.dstY, m.w, m.h, m.format, m.depth, m.bytesPerLine, m.data, m.fg, m.bg));

  /* Window lifecycle / config: after each one, sync affected SAB slots
   * and (for Create/Destroy) push Slot.Assigned/Freed to the owner
   * client so its xidToSlot table stays current. */
  ch.on<ClientToServerVoid & { kind: 'Window.Create' }>('Window.Create', (m) => {
    host.onWindowCreate(m.connId, m.id, m.parent, m.x, m.y, m.w, m.h, m.borderWidth, m.borderPixel, m.bgType, m.bgValue);
    const slot = allocSlot(s, m.id);
    pushSlotAssigned(s, m.connId, m.id, slot);
    syncWindowSab(s, m.id);
  });
  ch.on<ClientToServerVoid & { kind: 'Window.SetBorder' }>('Window.SetBorder', (m) => {
    host.onWindowSetBorder(m.id, m.borderWidth, m.borderPixel);
    syncWindowSab(s, m.id);
  });
  ch.on<ClientToServerVoid & { kind: 'Window.SetBg' }>('Window.SetBg', (m) =>
    host.onWindowSetBg(m.id, m.bgType, m.bgValue));
  ch.on<ClientToServerVoid & { kind: 'Window.SetBgPixmap' }>('Window.SetBgPixmap', (m) =>
    host.onWindowSetBgPixmap(m.id, m.pmId));
  ch.on<ClientToServerVoid & { kind: 'Window.Configure' }>('Window.Configure', (m) => {
    host.onWindowConfigure(m.id, m.x, m.y, m.w, m.h);
    resyncAllSab(s);   /* geometry cascade can hit many descendants */
  });
  ch.on<ClientToServerVoid & { kind: 'Window.Map' }>('Window.Map', (m) => {
    host.onWindowMap(m.connId, m.id);
    resyncAllSab(s);
  });
  ch.on<ClientToServerVoid & { kind: 'Window.Unmap' }>('Window.Unmap', (m) => {
    host.onWindowUnmap(m.connId, m.id);
    resyncAllSab(s);
  });
  ch.on<ClientToServerVoid & { kind: 'Window.Destroy' }>('Window.Destroy', (m) => {
    host.onWindowDestroy(m.id);
    freeSlot(s, m.id);
    resyncAllSab(s);
    pushSlotFreed(s, connId, m.id);
  });
  ch.on<ClientToServerVoid & { kind: 'Window.Raise' }>('Window.Raise', (m) =>
    host.onWindowRaise(m.id));
  ch.on<ClientToServerVoid & { kind: 'Window.SelectInput' }>('Window.SelectInput', (m) =>
    host.onSelectInput(m.connId, m.id, m.mask));
  ch.on<ClientToServerVoid & { kind: 'Window.SetOverrideRedirect' }>('Window.SetOverrideRedirect', (m) =>
    host.onSetOverrideRedirect(m.id, m.flag));
  ch.on<ClientToServerVoid & { kind: 'Window.Reparent' }>('Window.Reparent', (m) => {
    host.onReparentWindow(m.id, m.parent, m.x, m.y);
    resyncAllSab(s);
  });
  ch.on<ClientToServerVoid & { kind: 'Window.Shape' }>('Window.Shape', (m) =>
    host.onWindowShape(m.id, m.rects));

  /* Grabs */
  ch.on<ClientToServerVoid & { kind: 'Grab.Button' }>('Grab.Button', (m) =>
    host.onGrabButton(m.window, m.button, m.modifiers, m.ownerEvents, m.eventMask, m.pointerMode, m.keyboardMode, m.confineTo, m.cursor));
  ch.on<ClientToServerVoid & { kind: 'Ungrab.Button' }>('Ungrab.Button', (m) =>
    host.onUngrabButton(m.window, m.button, m.modifiers));

  /* Properties (void + reply) */
  ch.on<ClientToServerVoid & { kind: 'Property.Change' }>('Property.Change', (m) => {
    /* Host.changeProperty's format is typed 8|16|32; the RPC carries it
     * as a plain number. Cast after a runtime gate. */
    if (m.format !== 8 && m.format !== 16 && m.format !== 32) return;
    host.changeProperty(m.w, m.atom, m.type, m.format, m.mode, m.data);
  });
  ch.on<ClientToServerVoid & { kind: 'Property.Delete' }>('Property.Delete', (m) =>
    host.deleteProperty(m.w, m.atom));
  ch.on<ClientToServerCall & { kind: 'Property.PeekMeta' }>('Property.PeekMeta', (m) => {
    const r = host.peekProperty(m.w, m.atom, m.reqType, m.longOffset, m.longLength, false);
    if (r === null) {
      ch.reply(m.seqId, null);
      return;
    }
    ch.reply(m.seqId, {
      found: r.found, type: r.type, format: r.format,
      nitems: r.nitems, bytesAfter: r.bytesAfter,
      dataLen: r.data.length, data: r.data,
    });
  });
  ch.on<ClientToServerCall & { kind: 'Property.PeekData' }>('Property.PeekData', (m) => {
    const r = host.peekProperty(m.w, m.atom, m.reqType, m.longOffset, m.longLength, m.deleteFlag);
    if (!r || !r.found) { ch.reply(m.seqId, { data: new ArrayBuffer(0) }); return; }
    /* Transfer the bytes -- copy into a fresh ArrayBuffer for transfer. */
    const copy = r.data.slice().buffer as ArrayBuffer;
    ch.reply(m.seqId, { data: copy }, [copy]);
  });
  ch.on<ClientToServerCall & { kind: 'Property.ListCount' }>('Property.ListCount', (m) => {
    const atoms = host.listProperties(m.w);
    const u32 = new Uint32Array(atoms);
    const buf = u32.buffer as ArrayBuffer;
    ch.reply(m.seqId, { atoms: u32 }, [buf]);
  });

  /* Atoms (reply) */
  ch.on<ClientToServerCall & { kind: 'Atom.Intern' }>('Atom.Intern', (m) => {
    const atom = host.internAtom(m.name, m.onlyIfExists);
    ch.reply(m.seqId, { atom });
  });
  ch.on<ClientToServerCall & { kind: 'Atom.GetName' }>('Atom.GetName', (m) => {
    const name = host.getAtomName(m.atom);
    ch.reply(m.seqId, { name });
  });

  /* Clipboard (write only for now) */
  ch.on<ClientToServerVoid & { kind: 'Clipboard.WriteUtf8' }>('Clipboard.WriteUtf8', (m) => {
    if (typeof navigator !== 'undefined' && navigator.clipboard?.writeText) {
      const text = new TextDecoder('utf-8').decode(m.bytes);
      navigator.clipboard.writeText(text).catch((e) => {
        console.warn('[emx11:server] clipboard write failed:', e);
      });
    }
  });
}

/* -- PortModuleSurface ---------------------------------------------------- */

/** Adapter that presents a Client Worker's MessagePort as a
 *  `ModuleCcallSurface`. Host's existing dispatch layers call
 *  `module.ccall('emx11_push_*_event', ...)` whenever an event needs
 *  to reach a client; here we translate that into a `XEvent.*`
 *  postMessage delivered over the port. No reply channel needed --
 *  push_* events are strictly fire-and-forget on both sides. */
class PortModuleSurface implements ModuleCcallSurface {
  constructor(private readonly port: MessagePort) {}

  ccall(name: string, _ret: unknown, _argTypes: unknown, args: unknown): unknown {
    const a = args as number[];
    switch (name) {
      case 'emx11_push_button_event':
        this.port.postMessage({
          kind: 'XEvent.Button',
          xType: a[0], window: a[1], lx: a[2], ly: a[3],
          x_root: a[4], y_root: a[5], button: a[6], state: a[7],
        });
        return;
      case 'emx11_push_motion_event':
        this.port.postMessage({
          kind: 'XEvent.Motion',
          window: a[0], x: a[1], y: a[2],
          x_root: a[3], y_root: a[4], state: a[5],
        });
        return;
      case 'emx11_push_key_event':
        this.port.postMessage({
          kind: 'XEvent.Key',
          xType: a[0], window: a[1], keysym: a[2], state: a[3],
          x: a[4], y: a[5],
        });
        return;
      case 'emx11_push_expose_event':
        this.port.postMessage({
          kind: 'XEvent.Expose',
          window: a[0], x: a[1], y: a[2], w: a[3], h: a[4],
        });
        return;
      case 'emx11_push_map_request':
        this.port.postMessage({
          kind: 'XEvent.MapRequest',
          parent: a[0], window: a[1],
        });
        return;
      case 'emx11_push_reparent_notify':
        this.port.postMessage({
          kind: 'XEvent.ReparentNotify',
          window: a[0], parent: a[1], x: a[2], y: a[3],
        });
        return;
      default:
        console.warn(`[emx11:server] PortModuleSurface: unknown ccall '${name}'`);
    }
  }
}

/* -- SAB slot allocator --------------------------------------------------- */

function allocSlot(s: ServerState, winId: number): number {
  let slot = s.freedSlots.pop();
  if (slot === undefined) {
    slot = s.nextSlot++;
    if (slot >= SAB_MAX_WINDOW_SLOTS) {
      console.warn('[emx11:server] SAB slot table full');
      return 0;
    }
  }
  s.xidToSlot.set(winId, slot);
  s.slotToXid.set(slot, winId);
  return slot;
}

function freeSlot(s: ServerState, winId: number): void {
  const slot = s.xidToSlot.get(winId);
  if (slot === undefined) return;
  s.xidToSlot.delete(winId);
  s.slotToXid.delete(slot);
  s.freedSlots.push(slot);
  clearWinSlot(s.sab, slot);
}

function pushSlotAssigned(s: ServerState, connId: number, winId: number, slot: number): void {
  const port = s.clientPorts.get(connId);
  if (!port) return;
  const msg: ServerToClientSlot = { kind: 'Slot.Assigned', winId, slot };
  port.postMessage(msg);
}

function pushSlotFreed(s: ServerState, connId: number, winId: number): void {
  const port = s.clientPorts.get(connId);
  if (!port) return;
  const msg: ServerToClientSlot = { kind: 'Slot.Freed', winId };
  port.postMessage(msg);
}

function syncWindowSab(s: ServerState, winId: number): void {
  const slot = s.xidToSlot.get(winId);
  if (slot === undefined || slot === 0) return;
  const attrs = s.host.getWindowAttrs(winId);
  const origin = s.host.getWindowAbsOrigin(winId);
  if (attrs) {
    writeWinAttr(
      s.sab, slot, 1, attrs.x, attrs.y, attrs.width, attrs.height,
      attrs.mapped ? 1 : 0, attrs.overrideRedirect ? 1 : 0, attrs.borderWidth,
    );
  } else {
    clearWinSlot(s.sab, slot);
  }
  if (origin) {
    writeAbsOrigin(s.sab, slot, 1, origin.ax, origin.ay);
  }
}

/** Resync every mirrored slot. Called after Configure/Map/Unmap/Reparent
 *  since a single call can cascade to many descendants' abs origins. */
function resyncAllSab(s: ServerState): void {
  for (const winId of s.xidToSlot.keys()) syncWindowSab(s, winId);
}
