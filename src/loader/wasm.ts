/**
 * Emscripten wasm loader.
 *
 * Each demo wasm is built with `-s MODULARIZE=1 -s EXPORT_ES6=1`, producing
 * a .js ES module whose default export is a factory returning a Promise
 * that resolves to the EmscriptenModule instance. The .wasm sits beside it
 * and the factory locates it relative to its own script URL.
 *
 * The .js glue is imported directly via `import(glueUrl)` — the browser's
 * native module loader handles it, and in dev Vite processes the import
 * naturally without the blob-URL / absolute-path conflicts.  The .wasm is
 * routed through Cache Storage (see ./cache.ts) so the large binary skips
 * the network on repeat visits.  Sibling assets (`--preload-file` .data
 * blobs etc.) fall through to Emscripten's default `locateFile` resolution
 * relative to the glue URL.
 */

import type {
  EmscriptenModule,
  EmscriptenModuleFactory,
} from '../types/emscripten.js';
import { cachedFetchBytes, type CacheMode } from './cache.js';

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
  /** Cache Storage policy for the .wasm fetch. Defaults to `'use'`
   *  (cache-first); the EmX11 facade overrides to `'bypass'` in Vite
   *  dev mode. */
  cacheMode?: CacheMode;
  /** Override Emscripten's `quit_` (the throw used by exit/abort).
   *  Must be set at factory-init time -- Emscripten captures it once
   *  during runtime bootstrap and never reads `Module.quit` again. The
   *  callback should perform JS-side cleanup and then re-throw `toThrow`
   *  to match default behavior. Used by ConnectionManager.launchClient
   *  to catch wasm-internal exit() (xcalc's q-key Quit() flow) which
   *  the post-launch onExit / ccall-throw paths both miss. */
  quit?: (status: number, toThrow: unknown) => void;
  /** Extra properties to set on Module before the wasm factory starts.
   *  ConnectionManager uses this to pre-populate Module['emX11Host']
   *  so the JS library's $EmX11Host.init() sees the caller's Host and
   *  skips creating a duplicate default host from --pre-js. */
  moduleOverrides?: Record<string, unknown>;
  /** Emscripten's `onExit` -- fires from `_proc_exit` when
   *  `noExitRuntime: false`. Belt-and-suspenders for clean-shutdown
   *  paths that DO reach run() (e.g. main() returning normally). */
  onExit?: (status: number) => void;
  /** stdout sink. Must be passed in factory args -- recent Emscripten
   *  defines `Module.print` as a getter that reads a closure-bound
   *  variable, so a post-init assignment throws "only a getter". */
  print?: (msg: string) => void;
  /** stderr sink. Same constraint as `print`. */
  printErr?: (msg: string) => void;
}

export async function loadWasm(options: LoadOptions): Promise<EmscriptenModule> {
  const cacheMode = options.cacheMode ?? 'use';

  /* .js glue: direct dynamic import.  The browser's native module
   * loader resolves it against the page origin; in dev Vite processes
   * the import naturally (no blob-URL / absolute-path conflict). */
  const glueModule = (await import(/* @vite-ignore */ options.glueUrl)) as {
    default: EmscriptenModuleFactory;
  };
  const factory = glueModule.default;

  /* .wasm: fetch via Cache Storage, hand instantiate to Emscripten via
   * the `instantiateWasm` hook. Emscripten will skip its own
   * fetch+instantiateStreaming and call our hook with the imports
   * table once it's ready. */
  const wasmBytesPromise = cachedFetchBytes(options.wasmUrl, cacheMode);

  const mod = await factory({
    ...(options.moduleOverrides ?? {}),
    instantiateWasm: (imports, success) => {
      wasmBytesPromise
        .then((bytes) =>
          /* Cast: TS picks the `Module` overload of WebAssembly.instantiate
           * over the `BufferSource` one when the input is `Uint8Array<...>`,
           * giving us back `Instance` instead of `{instance, module}`.
           * Pass the underlying ArrayBuffer to nudge the right overload. */
          WebAssembly.instantiate(bytes.buffer as ArrayBuffer, imports),
        )
        .then(({ instance, module }) => success(instance, module))
        .catch((err) => {
          /* Emscripten swallows instantiateWasm rejections silently
           * (it just keeps waiting for success() to be called).
           * Surface the error to the console so a broken cached blob
           * doesn't look like a hung load. */
          console.error('em-x11 loader: instantiateWasm failed:', err);
          throw err;
        });
      /* Returning {} signals to Emscripten "I'm handling instantiation
       * asynchronously" so it doesn't fall back to its own
       * instantiateStreaming on a missing return value. */
      return {};
    },
    locateFile: (path: string) => {
      if (path.endsWith('.wasm')) {
        /* The instantiateWasm hook above already loaded the wasm; this
         * branch shouldn't fire. Keep it pointing at the original URL
         * as a safety net. */
        return options.wasmUrl;
      }
      /* `--preload-file` produces a sibling .data blob the glue fetches
       * by its basename. Resolve it against the glue URL's directory. */
      const baseDir = options.glueUrl.slice(0, options.glueUrl.lastIndexOf('/') + 1);
      return baseDir + path;
    },
    arguments: options.arguments,
    thisProgram: options.thisProgram,
    preRun: options.preRun,
    /* Default Emscripten behavior is `noExitRuntime: true`, which makes
     * exit()/main-return a no-op for cleanup hooks (atexit / onExit
     * never fire). xcalc's q action calls exit(0) inside an Xt action
     * callback, and without this flip the host never learns the wasm
     * is gone -- xcalc's frame stays on the compositor as an inert
     * window ("frozen"). With noExitRuntime=false, _proc_exit invokes
     * Module.onExit (set by ConnectionManager.launchClient after binding)
     * which routes to closeDisplay and tears down owned windows. */
    noExitRuntime: false,
    /* exactOptionalPropertyTypes: passing `undefined` to a non-optional
     * Module field would error. Spread only the keys that are set. */
    ...(options.quit !== undefined ? { quit: options.quit } : {}),
    ...(options.onExit !== undefined ? { onExit: options.onExit } : {}),
    ...(options.print !== undefined ? { print: options.print } : {}),
    ...(options.printErr !== undefined ? { printErr: options.printErr } : {}),
  });

  /* JSPI: Emscripten 5.0.3+ wraps JSPI_EXPORTS functions with
   * WebAssembly.promising so they return Promises; non-JSPI exports
   * return their value directly (void functions return undefined).
   *
   * Always forcing {async:true} breaks non-JSPI functions: the
   * generated ccall does `ret.then(onDone)` and `undefined.then()`
   * throws.  Default to the sync path instead — for JSPI exports
   * the Promise passes through convertReturnValue unchanged, and
   * for non-JSPI exports onDone converts the value synchronously.
   *
   * The caller can still override via opts.async for value-returning
   * JSPI functions that need onDone chained for UTF8ToString etc. */
  {
    const origCcall = mod.ccall.bind(mod);
    mod.ccall = function (
      ident: string,
      returnType: string | null,
      argTypes: string[],
      args: unknown[],
      opts?: { async?: boolean },
    ) {
      return origCcall(ident, returnType, argTypes, args, opts);
    } as typeof mod.ccall;
  }

  return mod;
}
