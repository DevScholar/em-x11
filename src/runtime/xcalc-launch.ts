/**
 * xcalc launcher.
 *
 * The vendored xcalc in third-party/ is a verbatim X.Org tarball that
 * gets fetched at setup time and is NOT git-tracked. Anything em-x11
 * specific has to live on the runtime side.
 *
 * The one tweak xcalc needs is /usr/lib/X11/app-defaults/XCalc — without
 * it XtGetResources finds nothing, widgets realize with their compile-
 * time defaults (0x0 Forms, stacked Commands), and the calculator is
 * unreadable. That staging is now project-default: src/runtime/app-
 * defaults.ts registers XCalc against `xcalc.js`, and Host's
 * launchClient auto-injects the preRun hook for any registered
 * artifact. So this launcher is just a thin URL-defaulting wrapper —
 * you can also call host.launchClient directly with the same glueUrl
 * (as the session demo does) and Xrm will still be set up.
 */

import type { Host } from '../host/index.js';
import type { EmscriptenModule } from '../types/emscripten.js';

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
  });
}
