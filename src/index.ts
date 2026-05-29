/**
 * @devscholar/em-x11 public entry.
 *
 * Three API layers, from simplest to most powerful:
 *
 *   Layer 1 (zero JS): emcc myapp.c -sUSE_EMX11 -o myapp.html
 *     The port auto-injects the JS library and creates a default Host.
 *     No user JS required — same UX as emcc -sUSE_SDL=2.
 *
 *   Layer 2 (single program): initEmX11()
 *     import { initEmX11 } from '@devscholar/em-x11';
 *     const x11 = await initEmX11({ canvas, width: 1024, height: 768 });
 *     // x11.display, x11.debug, no child_process concept
 *
 *   Layer 3 (multi-process): createEmX11()
 *     const session = await createEmX11({ canvas });
 *     const proc = session.child_process.spawn('/bin/xeyes', { ... });
 *     await proc.ready;
 *
 * Internal manager classes (Host, ConnectionManager, ...) under
 * src/host are NOT re-exported. `em._host` is a typed @internal
 * escape hatch for callers still mid-migration.
 */

export { createEmX11, EmX11, VERSION } from './api/emx11.js';
export { initEmX11 } from './api/initEmX11.js';
export type { InitEmX11Options, EmX11Session } from './api/initEmX11.js';

export type {
  CreateEmX11Options,
  DlopenAdapter,
  DlopenOptions,
  EmX11Debug,
  EmX11Display,
  EmX11FS,
  InjectKeyEvent,
  InjectMouseEvent,
  LoadedModule,
  MountSpec,
  Process,
  ProcessFS,
  SpawnOptions,
  TextInputRemoteHandle,
} from './api/types.js';

export {
  createDomTextInputBridge,
} from './host/text-input.js';
export type {
  DomTextInputBridge,
  DomTextInputBridgeOptions,
} from './host/text-input.js';
