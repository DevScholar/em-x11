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
      return path;
    },
  });
}
