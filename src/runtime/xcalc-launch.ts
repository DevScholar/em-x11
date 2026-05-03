/**
 * xcalc launcher.
 *
 * The vendored xcalc in third-party/ is a verbatim X.Org tarball that
 * gets fetched at setup time and is NOT git-tracked. Anything em-x11
 * specific has to live on the runtime side.
 *
 * xcalc requires /usr/lib/X11/app-defaults/XCalc staged into MEMFS
 * before main() runs — without it XtGetResources finds nothing, widgets
 * realize with their compile-time defaults (0x0 Forms, stacked Commands),
 * and the calculator is unreadable. This launcher stages that file
 * explicitly via a preRun hook; callers using host.launchClient directly
 * must do the same staging themselves.
 */

// eslint-disable-next-line import/no-unresolved
import xcalcAppDefaults from '../../third-party/xcalc/app-defaults/XCalc?raw';
import type { Host } from '../host/index.js';
import type { EmscriptenModule } from '../types/emscripten.js';
import type { Orchestrator } from '../worker/main-thread/orchestrator.js';
import type { ClientWorkerHandle } from '../worker/main-thread/client-proxy.js';
import { makeStagingPreRun } from './app-defaults.js';

export interface LaunchXcalcOptions {
  /** Build artifact directory containing xcalc.js / xcalc.wasm. */
  artifactBase?: string;
}

export async function launchXcalc(
  host: Host,
  options: LaunchXcalcOptions = {},
): Promise<{ connId: number; module: EmscriptenModule }> {
  const base = options.artifactBase ?? '/build/artifacts/xcalc';
  return host.launchClient({
    glueUrl: `${base}/xcalc.js`,
    wasmUrl: `${base}/xcalc.wasm`,
    preRun: [
      makeStagingPreRun([
        { path: '/usr/lib/X11/app-defaults/XCalc', contents: xcalcAppDefaults },
      ]),
    ],
  });
}

/** Worker-mode xcalc launcher. Same app-defaults staging as the legacy
 *  launcher, but described as data so it can cross the worker boundary
 *  (the Client Worker's bootstrap applies it as preRun internally). */
export async function launchXcalcWorker(
  orch: Orchestrator,
  options: LaunchXcalcOptions = {},
): Promise<ClientWorkerHandle> {
  const base = options.artifactBase ?? '/build/artifacts/xcalc';
  return orch.launchClient({
    glueUrl: `${base}/xcalc.js`,
    wasmUrl: `${base}/xcalc.wasm`,
    stagedFiles: [
      { path: '/usr/lib/X11/app-defaults/XCalc', contents: xcalcAppDefaults },
    ],
    name: 'emx11-xcalc',
  });
}
