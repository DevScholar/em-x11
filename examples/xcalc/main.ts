/**
 * xcalc demo harness — single-program mode.
 *
 * xcalc needs /usr/lib/X11/app-defaults/XCalc in MEMFS before main()
 * runs, otherwise XtGetResources returns nothing and the button layout
 * collapses to widget defaults. The file is embedded at build time via
 * Emscripten's --preload-file — the glue loads the .data package
 * automatically. locateFile tells Emscripten where to find both the
 * .wasm and .data files.
 */

import { createEmX11 } from '../../src/index.js';

const x11 = await createEmX11({ width: 800, height: 600 });

const factory = (await import('/build/artifacts/xcalc/xcalc.js')).default;
await factory({
  ...x11.moduleOverrides,
  thisProgram: 'xcalc',
  locateFile: (path: string) => `/build/artifacts/xcalc/${path}`,
});
