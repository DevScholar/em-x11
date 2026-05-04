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
 * explicitly via the Emscripten `preRun` hook.
 */

// eslint-disable-next-line
import xcalcAppDefaults from '../../third-party/xcalc/app-defaults/XCalc?raw';
import type { Host } from '../host/index.js';
import type { EmscriptenModule } from '../types/emscripten.js';
import { stagePreRun } from './stage-files.js';

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
    thisProgram: 'xcalc',
    preRun: [
      stagePreRun([
        { path: '/usr/lib/X11/app-defaults/XCalc', contents: xcalcAppDefaults },
      ]),
    ],
  });
}
