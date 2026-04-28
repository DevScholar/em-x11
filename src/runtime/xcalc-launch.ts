/**
 * xcalc launcher with em-x11 specific config.
 *
 * The vendored xcalc in third-party/ is a verbatim X.Org tarball that gets
 * fetched at setup time and is NOT git-tracked. Anything em-x11 specific
 * has to live in the runtime side.
 *
 * The tweak we need is the app-defaults resource file. xcalc's Xaw layout
 * (Form + Commands + Toggles in a grid) is driven almost entirely by
 * XCalc*... lines in app-defaults/XCalc. Without that file, XtGetResources
 * finds nothing, widgets realize with their hard-coded defaults (0x0 or
 * ~0-sized Forms, stacked Commands), and the calculator is unreadable.
 *
 * Strategy: stage app-defaults/XCalc (shipped in the upstream tarball)
 * into MEMFS at /usr/lib/X11/app-defaults/XCalc via the Module's preRun
 * hook. That's where libXt's compile-time XFILESEARCHPATHDEFAULT (see
 * third-party/libXt/include/X11/IntrinsicI.h) resolves "app-defaults/%N"
 * lookups; no env vars or XtAppSetFallbackResources call needed.
 *
 * The ?raw import is Vite's built-in mechanism for inlining a text asset
 * at build time, so the wasm binary doesn't have to ship the file and
 * the dev server doesn't have to serve it.
 */

import type { Host } from '../host/index.js';
import type { EmscriptenModule } from '../types/emscripten.js';
// eslint-disable-next-line import/no-unresolved
import xcalcAppDefaults from '../../third-party/xcalc/app-defaults/XCalc?raw';

const APP_DEFAULTS_PATH = '/usr/lib/X11/app-defaults/XCalc';

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
      (mod) => {
        const fs = mod.FS;
        if (!fs) {
          throw new Error(
            'em-x11: xcalc wasm has no FS - was it built with the default ' +
              'Emscripten filesystem support?',
          );
        }
        // mkdir -p /usr/lib/X11/app-defaults
        for (const dir of ['/usr', '/usr/lib', '/usr/lib/X11', '/usr/lib/X11/app-defaults']) {
          try {
            fs.mkdir(dir);
          } catch (e) {
            // ENOENT / EEXIST both get thrown as Error objects in Emscripten.
            // Pre-existing dir is fine; anything else re-throws.
            const msg = (e as Error).message ?? '';
            if (!msg.includes('exist') && !msg.includes('EEXIST')) throw e;
          }
        }
        fs.writeFile(APP_DEFAULTS_PATH, xcalcAppDefaults);
      },
    ],
  });
}
