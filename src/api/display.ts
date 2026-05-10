/**
 * em.display — public surface for the canvas, root window, and input
 * injection.
 *
 * `display.inject.*` is the documented replacement for the older
 * `host.devices.push*` reach-through. Worker-mode hosts (pyodide-tk)
 * relay raw DOM events from the main thread and call these methods
 * inside the worker; ordinary DOM-mode demos don't need to call inject
 * at all (the InputBridge attaches its own listeners).
 */

import type { Host } from '../host/index.js';
import type {
  EmX11Display,
  InjectKeyEvent,
  InjectMouseEvent,
} from './types.js';

export class DisplayNamespace implements EmX11Display {
  constructor(private readonly host: Host) {}

  readonly inject = {
    mouseDown: (e: InjectMouseEvent) => this.host.devices.pushMouseDown(e),
    mouseUp: (e: InjectMouseEvent) => this.host.devices.pushMouseUp(e),
    mouseMove: (e: Omit<InjectMouseEvent, 'button'>) =>
      this.host.devices.pushMouseMove(e),
    keyDown: (e: InjectKeyEvent) =>
      this.host.devices.pushKeyDown({ ...e, hasFocus: e.hasFocus ?? true }),
    keyUp: (e: InjectKeyEvent) =>
      this.host.devices.pushKeyUp({ ...e, hasFocus: e.hasFocus ?? true }),
    setPointer: (x: number, y: number) => this.host.devices.setPointer(x, y),
    textKey: (text: string) => this.host.devices.pushTextKey(text),
  };

  get canvas(): HTMLCanvasElement | OffscreenCanvas {
    return this.host.canvas.surface;
  }

  get width(): number {
    return this.host.canvas.cssWidth;
  }

  get height(): number {
    return this.host.canvas.cssHeight;
  }

  get rootWindowId(): number {
    return this.host.getRootWindow();
  }

  waitForSubstructureRedirect(winId: number, timeoutMs?: number): Promise<number> {
    return this.host.waitForSubstructureRedirect(winId, timeoutMs);
  }
}
