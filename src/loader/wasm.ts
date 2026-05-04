/**
 * Emscripten wasm loader.
 *
 * Each demo wasm is built with `-s MODULARIZE=1 -s EXPORT_ES6=1`, producing
 * a .js ES module whose default export is a factory returning a Promise
 * that resolves to the EmscriptenModule instance. The .wasm sits beside it
 * and the factory locates it relative to its own script URL.
 */

import type {
  EmscriptenModule,
  EmscriptenModuleFactory,
} from '../types/emscripten.js';

export interface LoadOptions {
  /** URL of the emscripten-generated .js glue. */
  glueUrl: string;
  /** URL of the .wasm binary. Usually co-located with the glue. */
  wasmUrl: string;
  /** Argv to pass to main(), excluding argv[0]. Set `thisProgram` to
   *  override argv[0] itself (Emscripten otherwise hard-codes
   *  `./this.program`, which Xt-based apps then surface as their
   *  application name / WM_CLASS / window title). */
  arguments?: string[];
  /** Override for argv[0]. Xt's XtAppInitialize derives the application
   *  name from `basename(argv[0])`, so without this xeyes/xcalc/twm all
   *  identify themselves as "this.program". */
  thisProgram?: string;
  /** Hooks fired after MEMFS is initialised but before main() runs. The
   *  Module argument has FS available, so callers can stage files (e.g.
   *  the twmrc) into MEMFS before the program reads them. */
  preRun?: ((mod: EmscriptenModule) => void)[];
}

export async function loadWasm(options: LoadOptions): Promise<EmscriptenModule> {
  const glueModule = (await import(/* @vite-ignore */ options.glueUrl)) as {
    default: EmscriptenModuleFactory;
  };
  const factory = glueModule.default;

  return factory({
    locateFile: (path: string) => {
      if (path.endsWith('.wasm')) {
        return options.wasmUrl;
      }
      /* `--preload-file` produces a sibling .data blob the glue fetches
       * by its basename. Resolve it (and anything else) against the
       * glue URL's directory so relative fetches don't fall through to
       * the dev server's SPA fallback. */
      const baseDir = options.glueUrl.slice(0, options.glueUrl.lastIndexOf('/') + 1);
      const resolved = baseDir + path;
      return resolved;
    },
    arguments: options.arguments,
    thisProgram: options.thisProgram,
    preRun: options.preRun,
  });
}
