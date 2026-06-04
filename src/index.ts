/**
 * @devscholar/em-x11 public entry.
 *
 * Two API layers, from simplest to most powerful:
 *
 *   Layer 1 (zero JS): emcc myapp.c -sUSE_EMX11 -o myapp.html
 *     The port auto-injects the JS library and creates a default Host.
 *     No user JS required — same UX as emcc -sUSE_SDL=2.
 *
 *   Layer 2 (multi-instance): createEmX11()
 *     import { createEmX11 } from '@devscholar/em-x11';
 *     const x11 = await createEmX11({ canvas });
 *
 *     // Single wasm program: spread moduleOverrides into the factory
 *     const factory = (await import('./myapp.js')).default;
 *     await factory({ ...x11.moduleOverrides });
 *
 *     // Multi-process session: use child_process
 *     const proc = x11.child_process.spawn('/bin/xeyes');
 *     await proc.ready;
 *
 * Internal manager classes (Host, ConnectionManager, ...) under
 * src/host are NOT re-exported. `x11._host` is a typed @internal
 * escape hatch for callers still mid-migration.
 */

export { createEmX11, EmX11, VERSION } from './api/emx11.js';

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
