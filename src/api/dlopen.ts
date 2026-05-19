/**
 * em.dlopen — pluggable side-module loader.
 *
 * em-x11 itself doesn't ship a dynamic linker; Emscripten's loader is
 * tied to the Module that hosts the side modules, and Pyodide already
 * has a battle-tested `loadDynlib` (with NEEDED auto-cascade). We
 * accept an adapter and delegate.
 *
 * pyodide-tk supplies an adapter wrapping `py._api.loadDynlib +
 * LDSO.loadedLibsByName` so a single `em.dlopen('/usr/lib/libXft.so')`
 * loads the .so and returns its exports table.
 *
 * tcldide's static-archive demos never call em.dlopen and never set
 * the adapter; the default thrower below makes that misuse loud
 * rather than silent.
 */

import type { DlopenAdapter, LoadedModule } from './types.js';

export const defaultDlopen: DlopenAdapter = (soPath: string) => {
  return Promise.reject(
    new Error(
      `em-x11: em.dlopen('${soPath}') called but no dlopen adapter was supplied to createEmX11(). ` +
        `Pass { dlopen } in CreateEmX11Options. Pyodide consumers should wrap py._api.loadDynlib.`,
    ),
  );
};

export type { DlopenAdapter, LoadedModule };
