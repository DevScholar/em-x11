/**
 * xcalc launcher.
 *
 * The vendored xcalc in third-party/ is a verbatim X.Org tarball that
 * gets fetched at setup time and is NOT git-tracked. Anything em-x11
 * specific lives on the runtime side.
 *
 * xcalc requires /usr/lib/X11/app-defaults/XCalc staged into MEMFS
 * before main() runs — without it XtGetResources finds nothing, widgets
 * realize with their compile-time defaults (0x0 Forms, stacked Commands),
 * and the calculator is unreadable. This launcher stages that file
 * explicitly via `stagedFiles`.
 */

// eslint-disable-next-line
import xcalcAppDefaults from '../../third-party/xcalc/app-defaults/XCalc?raw';
import type { Orchestrator } from '../worker/main-thread/orchestrator.js';
import type { ClientWorkerHandle } from '../worker/main-thread/client-proxy.js';

export interface LaunchXcalcOptions {
  /** Build artifact directory containing xcalc.js / xcalc.wasm. */
  artifactBase?: string;
}

export async function launchXcalc(
  orch: Orchestrator,
  options: LaunchXcalcOptions = {},
): Promise<ClientWorkerHandle> {
  const base = options.artifactBase ?? '/build/artifacts/xcalc';
  return orch.launchClient({
    glueUrl: `${base}/xcalc.js`,
    wasmUrl: `${base}/xcalc.wasm`,
    thisProgram: 'xcalc',
    stagedFiles: [
      { path: '/usr/lib/X11/app-defaults/XCalc', contents: xcalcAppDefaults },
    ],
    name: 'emx11-xcalc',
  });
}
