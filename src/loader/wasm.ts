/**
 * Emscripten wasm loader.
 *
 * Each demo wasm is built with `-s MODULARIZE=1 -s EXPORT_ES6=1`, producing
 * a .js ES module whose default export is a factory returning a Promise
 * that resolves to the EmscriptenModule instance. The .wasm sits beside it
 * and the factory locates it relative to its own script URL.
 *
 * Both files are routed through Cache Storage (see ./cache.ts) so the
 * second visit doesn't re-download. The .js glue is fetched as text
 * and dynamically imported via a Blob URL; the .wasm is fed to
 * Emscripten via the `instantiateWasm` factory hook so we control the
 * bytes ourselves. Sibling assets (`--preload-file` .data blobs etc.)
 * fall through to Emscripten's default `locateFile` resolution.
 */

import type {
  EmscriptenModule,
  EmscriptenModuleFactory,
} from '../types/emscripten.js';
import { cachedFetchAsBlobUrl, cachedFetchBytes, type CacheMode } from './cache.js';

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
  /** Cache Storage policy for the .js + .wasm fetch. Defaults to
   *  `'use'` (cache-first); the EmX11 facade overrides to `'bypass'`
   *  in Vite dev mode. */
  cacheMode?: CacheMode;
  /** Override Emscripten's `quit_` (the throw used by exit/abort).
   *  Must be set at factory-init time -- Emscripten captures it once
   *  during runtime bootstrap and never reads `Module.quit` again. The
   *  callback should perform JS-side cleanup and then re-throw `toThrow`
   *  to match default behavior. Used by ConnectionManager.launchClient
   *  to catch wasm-internal exit() (xcalc's q-key Quit() flow) which
   *  the post-launch onExit / ccall-throw paths both miss. */
  quit?: (status: number, toThrow: unknown) => void;
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

  /* .js glue: fetch (possibly from cache), wrap in a Blob URL, import.
   * The blob URL keeps the import side identical to a plain network
   * load — the factory's default export is the same shape. We don't
   * URL.revokeObjectURL after import because the imported module may
   * still reference its own source URL internally (Emscripten records
   * scriptDirectory from import.meta.url; revoking too early can
   * break worker spawn for ENVIRONMENT=worker builds). The blob is
   * tiny on the cost scale and gets reaped on tab unload. */
  const glueBlobUrl = await cachedFetchAsBlobUrl(options.glueUrl, cacheMode);
  const glueModule = (await import(/* @vite-ignore */ glueBlobUrl)) as {
    default: EmscriptenModuleFactory;
  };
  const factory = glueModule.default;

  /* .wasm: fetch via cache, hand instantiate to Emscripten via the
   * `instantiateWasm` hook. Emscripten will skip its own
   * fetch+instantiateStreaming and call our hook with the imports
   * table once it's ready. */
  const wasmBytesPromise = cachedFetchBytes(options.wasmUrl, cacheMode);

  return factory({
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
         * as a safety net so even a regression to default loading
         * resolves correctly. */
        return options.wasmUrl;
      }
      /* `--preload-file` produces a sibling .data blob the glue fetches
       * by its basename. Resolve it (and anything else) against the
       * GLUE URL's directory — NOT the blob URL we imported it through.
       * The blob URL has no useful directory; Emscripten would try to
       * fetch `blob:.../foo.data`, which 404s. */
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
}
