/**
 * Server Worker entry (== xorg server main).
 *
 * Owns the OffscreenCanvas and the Renderer. Main thread forwards DOM
 * input to us via the bootstrap port; we hit-test here (not on main),
 * write pointer_xy into SAB, and route XEvents to the owning Client
 * Worker's port.
 *
 * M1 scope: accept BootstrapServer, paint a static green rect to prove
 * the canvas transfer, log Dom.MouseMove and write them to SAB's
 * pointer slot. No Client Workers yet; no hit-test; no renderer logic
 * beyond the smoke-test rect.
 */

/// <reference lib="webworker" />

import { RpcChannel } from '../rpc/channel.js';
import type {
  BootstrapServer,
  DomMouseMove,
  DomMouseDown,
  DomMouseUp,
} from '../rpc/protocol.js';
import { attachSab, writePointer, type SabViews } from '../rpc/sab.js';

declare const self: DedicatedWorkerGlobalScope;

interface ServerState {
  canvas: OffscreenCanvas;
  ctx: OffscreenCanvasRenderingContext2D;
  sab: SabViews;
  mainChannel: RpcChannel;
}

let state: ServerState | null = null;

/* Bootstrap happens once, then we install the real message handlers
 * against the RpcChannel (which uses addEventListener, so we need to
 * wire it BEFORE creating the channel so no early messages are missed). */
self.addEventListener('message', bootstrapOnce);

function bootstrapOnce(ev: MessageEvent): void {
  const data = ev.data as BootstrapServer;
  if (!data || data.kind !== 'BootstrapServer') return;
  self.removeEventListener('message', bootstrapOnce);

  const ctx = data.canvas.getContext('2d', { alpha: false });
  if (!ctx) {
    throw new Error('[emx11:server] 2D context unavailable on OffscreenCanvas');
  }

  /* The transfer sized the OffscreenCanvas from the main side already;
   * but main only set the .width/.height attributes -- re-apply here so
   * the backing store matches cssWidth/cssHeight. */
  data.canvas.width = data.cssWidth;
  data.canvas.height = data.cssHeight;

  const sab = attachSab(data.sab);
  const mainChannel = new RpcChannel(self);

  state = { canvas: data.canvas, ctx, sab, mainChannel };

  registerDomInputHandlers(state);
  paintSmokeTest(state);
  console.log('[emx11:server] bootstrap complete', {
    w: data.cssWidth, h: data.cssHeight,
  });
}

function registerDomInputHandlers(s: ServerState): void {
  s.mainChannel.on<DomMouseMove>('Dom.MouseMove', (msg) => {
    writePointer(s.sab, msg.x, msg.y);
    /* M1: log every ~60th event to keep the console from flooding. */
    if ((s.pointerLogCounter = (s.pointerLogCounter ?? 0) + 1) % 60 === 0) {
      console.log('[emx11:server] mouse', msg.x, msg.y);
    }
  });
  s.mainChannel.on<DomMouseDown>('Dom.MouseDown', (msg) => {
    writePointer(s.sab, msg.x, msg.y);
    console.log('[emx11:server] mousedown', msg.x, msg.y, 'btn=', msg.button);
  });
  s.mainChannel.on<DomMouseUp>('Dom.MouseUp', (msg) => {
    writePointer(s.sab, msg.x, msg.y);
    console.log('[emx11:server] mouseup', msg.x, msg.y, 'btn=', msg.button);
  });
}

function paintSmokeTest(s: ServerState): void {
  const { ctx, canvas } = s;
  ctx.fillStyle = '#1a1a1a';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.fillStyle = '#2fa862';
  ctx.fillRect(40, 40, 100, 60);
  ctx.fillStyle = '#eef';
  ctx.font = '16px monospace';
  ctx.textBaseline = 'top';
  ctx.fillText('em-x11 server worker (M1)', 160, 60);
}

/* Minor nicety: augment ServerState with a pointer-log counter without
 * leaking it into the interface. Using a module-scoped declare. */
interface ServerState {
  pointerLogCounter?: number;
}
