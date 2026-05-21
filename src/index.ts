/**
 * @devscholar/em-x11 public entry.
 *
 * Use `createEmX11(options)` to construct an em-x11 instance. The
 * returned object is also mirrored onto `globalThis.emX11` so
 * DevTools and the C-side EM_JS bridges share one namespace.
 *
 * Example:
 *
 *   import { createEmX11 } from '@devscholar/em-x11';
 *
 *   const em = await createEmX11({ canvas: document.getElementById('x') });
 *   await em.fs.mount({ type: 'tar', source: '/assets/x11-base.tar', target: '/usr' });
 *   const xeyes = em.child_process.spawn('/build/artifacts/xeyes/xeyes', {
 *     argv: ['xeyes'],
 *     thisProgram: 'xeyes',
 *   });
 *   xeyes.on('exit', (code) => console.log('xeyes exited', code));
 *
 * Internal manager classes (Host, ConnectionManager, ...) under
 * src/host are NOT re-exported. `em._host` is a typed @internal
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
