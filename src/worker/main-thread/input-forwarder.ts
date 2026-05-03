/**
 * Main-thread DOM input forwarder. Attaches listeners to the real
 * HTMLCanvasElement (pre-`transferControlToOffscreen` is NOT required;
 * event dispatch continues to work on a transferred canvas), translates
 * to CSS-local coords, and relays to the Server Worker's RpcChannel.
 *
 * Reuses the modifier decoding + keysym translation from
 * `src/runtime/keymap.js` unchanged -- no wasm-side logic lives here.
 *
 * The Server Worker will write pointer_xy to SAB on receipt, so the
 * Client Workers can XQueryPointer-style read it directly without
 * additional round-trips.
 */

import type { RpcChannel } from '../rpc/channel.js';
import { keyEventToKeysym, modifiersFromEvent } from '../../runtime/keymap.js';
import type {
  DomMouseMove, DomMouseDown, DomMouseUp,
  DomKeyDown, DomKeyUp, DomFocusChange,
} from '../rpc/protocol.js';

export interface AttachOpts {
  canvas: HTMLCanvasElement;
  channel: RpcChannel;
}

export function attachInputForwarder(opts: AttachOpts): () => void {
  const { canvas, channel } = opts;
  const disposers: Array<() => void> = [];

  /* The Server Worker wants CSS-local coords (origin at the canvas's
   * top-left), not clientX/Y (viewport coords). `getBoundingClientRect`
   * returns the canvas's position in CSS px; subtract to get local. */
  function cssPoint(ev: MouseEvent): { x: number; y: number } {
    const rect = canvas.getBoundingClientRect();
    return { x: ev.clientX - rect.left, y: ev.clientY - rect.top };
  }

  /* Window-level mousemove so pointer tracks even when the cursor
   * drifts over browser chrome mid-drag (matches InputBridge.attachDom). */
  const onMove = (ev: MouseEvent): void => {
    const { x, y } = cssPoint(ev);
    const msg: DomMouseMove = {
      kind: 'Dom.MouseMove', x, y,
      modifiers: modifiersFromEvent(ev),
    };
    channel.send(msg);
  };
  window.addEventListener('mousemove', onMove);
  disposers.push(() => window.removeEventListener('mousemove', onMove));

  const onDown = (ev: MouseEvent): void => {
    const { x, y } = cssPoint(ev);
    const msg: DomMouseDown = {
      kind: 'Dom.MouseDown', x, y,
      button: ev.button + 1,                /* 0-based → X11 1-based */
      modifiers: modifiersFromEvent(ev),
    };
    channel.send(msg);
    /* focus canvas so keyboard lands here, matches InputBridge.attachDom */
    canvas.focus();
  };
  canvas.addEventListener('mousedown', onDown);
  disposers.push(() => canvas.removeEventListener('mousedown', onDown));

  /* window-level mouseup so a release outside the canvas still fires
   * (pointer-grab parity with InputBridge). */
  const onUp = (ev: MouseEvent): void => {
    const { x, y } = cssPoint(ev);
    const msg: DomMouseUp = {
      kind: 'Dom.MouseUp', x, y,
      button: ev.button + 1,
      modifiers: modifiersFromEvent(ev),
    };
    channel.send(msg);
  };
  window.addEventListener('mouseup', onUp);
  disposers.push(() => window.removeEventListener('mouseup', onUp));

  const onContextMenu = (ev: MouseEvent): void => {
    ev.preventDefault();                   /* let right-click be usable */
  };
  canvas.addEventListener('contextmenu', onContextMenu);
  disposers.push(() => canvas.removeEventListener('contextmenu', onContextMenu));

  const onKeyDown = (ev: KeyboardEvent): void => {
    const hasFocus = document.activeElement === canvas;
    if (hasFocus) ev.preventDefault();
    const msg: DomKeyDown = {
      kind: 'Dom.KeyDown',
      keysym: keyEventToKeysym(ev),
      modifiers: modifiersFromEvent(ev),
      hasFocus,
    };
    channel.send(msg);
  };
  window.addEventListener('keydown', onKeyDown);
  disposers.push(() => window.removeEventListener('keydown', onKeyDown));

  const onKeyUp = (ev: KeyboardEvent): void => {
    const hasFocus = document.activeElement === canvas;
    if (hasFocus) ev.preventDefault();
    const msg: DomKeyUp = {
      kind: 'Dom.KeyUp',
      keysym: keyEventToKeysym(ev),
      modifiers: modifiersFromEvent(ev),
      hasFocus,
    };
    channel.send(msg);
  };
  window.addEventListener('keyup', onKeyUp);
  disposers.push(() => window.removeEventListener('keyup', onKeyUp));

  const onFocus = (): void => {
    const msg: DomFocusChange = { kind: 'Dom.FocusChange', hasFocus: true };
    channel.send(msg);
  };
  const onBlur = (): void => {
    const msg: DomFocusChange = { kind: 'Dom.FocusChange', hasFocus: false };
    channel.send(msg);
  };
  canvas.addEventListener('focus', onFocus);
  canvas.addEventListener('blur', onBlur);
  disposers.push(() => canvas.removeEventListener('focus', onFocus));
  disposers.push(() => canvas.removeEventListener('blur', onBlur));

  return () => {
    for (const d of disposers) d();
  };
}
