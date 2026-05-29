/**
 * em.debug — toggleable trace flags + state dumpers.
 *
 * The flag fields read/write a module-level debug store (see
 * runtime/debug-flags.ts). Host.attachToBridge() initialises both
 * the module-level store (for Host TS code) and Module['emx11Debug']
 * (for JS library / EM_JS code inside the emscripten factory).
 * Both hold the same object reference, so DevTools toggles and API
 * calls are visible everywhere.
 */

import type { Host } from '../host/index.js';
import { dumpWindows as renderDumpWindows } from '../host/render/hit-test.js';
import { ensureDebugFlags } from '../runtime/debug-flags.js';
import type { EmX11Debug } from './types.js';

export class DebugNamespace implements EmX11Debug {
  constructor(private readonly host: Host) {}

  private get state() {
    return ensureDebugFlags();
  }

  get traceHit(): boolean { return this.state.traceHit; }
  set traceHit(v: boolean) { this.state.traceHit = v; }

  get traceHitNext(): boolean { return this.state.traceHitNext; }
  set traceHitNext(v: boolean) { this.state.traceHitNext = v; }

  get traceMotion(): boolean { return this.state.traceMotion; }
  set traceMotion(v: boolean) { this.state.traceMotion = v; }

  get traceButton(): boolean { return this.state.traceButton; }
  set traceButton(v: boolean) { this.state.traceButton = v; }

  get tracePaint(): boolean { return this.state.tracePaint; }
  set tracePaint(v: boolean) { this.state.tracePaint = v; }

  get traceCBtn(): boolean { return this.state.traceCBtn; }
  set traceCBtn(v: boolean) { this.state.traceCBtn = v; }

  get traceCMot(): boolean { return this.state.traceCMot; }
  set traceCMot(v: boolean) { this.state.traceCMot = v; }

  get traceMove(): boolean { return this.state.traceMove; }
  set traceMove(v: boolean) { this.state.traceMove = v; }

  get traceQp(): boolean { return this.state.traceQp; }
  set traceQp(v: boolean) { this.state.traceQp = v; }

  dumpWindows(): void {
    renderDumpWindows(this.host.renderer);
  }

  dumpGrabs(): void {
    this.host.grabs.dump();
  }
}
