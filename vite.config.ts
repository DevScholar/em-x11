import { defineConfig } from 'vite';
import type { Plugin, ResolvedServerUrls } from 'vite';
import { resolve, relative } from 'node:path';
import { readdirSync, statSync, existsSync, cpSync, readFileSync, mkdirSync } from 'node:fs';

/**
 * Auto-discovers examples/<name>/index.html entries and prints their URLs
 * after Vite's own "Local" / "Network" URL block, so you don't have to
 * remember the paths.
 */
function listDemoEntries(): { name: string; path: string }[] {
  const examplesDir = resolve(__dirname, 'examples');
  if (!existsSync(examplesDir)) return [];
  return readdirSync(examplesDir)
    .filter((name) => {
      const entry = resolve(examplesDir, name, 'index.html');
      return statSync(resolve(examplesDir, name)).isDirectory() && existsSync(entry);
    })
    .map((name) => ({ name, path: `/examples/${name}/` }));
}

/** Demos that import from src/ — Vite must bundle these. */
function bundlableEntries(): { name: string; path: string }[] {
  return listDemoEntries().filter((d) => d.name === 'twm-session');
}

/** Layer‑1 demos — pure HTML with only dynamic import('/artifacts/...'), no
 *  src/ imports. Vite can't bundle them, so we copy them as-is to dist/. */
function staticLayer1Entries(): { name: string; path: string }[] {
  return listDemoEntries().filter((d) => d.name !== 'twm-session');
}

function printDemoUrls(): Plugin {
  const demos = listDemoEntries();
  return {
    name: 'emx11-print-demo-urls',
    configureServer(server) {
      const originalPrint = server.printUrls.bind(server);
      server.printUrls = () => {
        originalPrint();
        if (demos.length === 0) return;
        const urls: ResolvedServerUrls | null = server.resolvedUrls;
        const bases = urls ? [...urls.local, ...urls.network] : [];
        // Prefer the first local URL as the "canonical" copy/paste target.
        const base = bases[0]?.replace(/\/$/, '') ?? '';
        // eslint-disable-next-line no-console
        console.log('\n  \x1b[1mDemos\x1b[0m:');
        for (const d of demos) {
          // eslint-disable-next-line no-console
          console.log(`    \x1b[36m${d.name.padEnd(10)}\x1b[0m ${base}${d.path}`);
        }
        // eslint-disable-next-line no-console
        console.log('');

        // Warn if native artifacts are missing.
        const artifacts = resolve(__dirname, 'build/artifacts');
        if (!existsSync(artifacts)) {
          // eslint-disable-next-line no-console
          console.warn(
            '\x1b[33m  build/artifacts/ not found. ' +
              "Run 'pnpm build:native' first, otherwise demos will fail " +
              'with "returned HTML instead of code".\x1b[0m\n',
          );
        }
      };
    },
  };
}

/**
 * Serve build/artifacts/**​/* as static files — no Vite transform.
 *
 * Emscripten's MODULARIZE=1+EXPORT_ES6=1 output is a pre-built artifact,
 * not source code.  Vite's import-analysis scans every .js file with
 * `export default` and chokes on the minified 250 KB blob full of
 * emscripten runtime internals (new URL, import.meta.url, etc.).
 *
 * This middleware runs before Vite's own transform middleware and serves
 * the raw file straight from disk, so the build artifact passes through
 * to the browser untouched.  It also serves the sibling .wasm and .data
 * files that Emscripten fetches at runtime.
 *
 * Maps URL path /artifacts/* → disk path build/artifacts/*.
 */
/**
 * Serve Layer‑1 example HTML files raw — no Vite transform.
 *
 * These demos contain only dynamic import('/artifacts/...') with no
 * src/ imports.  Vite's import-analysis can't resolve those absolute
 * paths and throws "Failed to resolve import".  Serving the HTML raw
 * (before Vite's transform middleware) bypasses the problem entirely.
 *
 * twm-session is excluded because its <script src="./main.ts"> DOES
 * need Vite's transform pipeline for TypeScript bundling.
 */
function serveLayer1HtmlRaw(): Plugin {
  const layer1Names = new Set(staticLayer1Entries().map((d) => d.name));
  return {
    name: 'emx11-serve-layer1-html-raw',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        if (!req.url) return next();
        const pathname = req.url.split('?')[0];
        // Match /examples/<name>/ or /examples/<name>/index.html
        const m = pathname.match(/^\/examples\/([^/]+)\/(index\.html)?$/);
        if (!m || !layer1Names.has(m[1]!)) return next();
        const filePath = resolve(__dirname, 'examples', m[1]!, 'index.html');
        if (!existsSync(filePath)) return next();
        res.setHeader('Content-Type', 'text/html');
        res.setHeader('Cache-Control', 'no-cache');
        res.statusCode = 200;
        res.end(readFileSync(filePath, 'utf-8'));
      });
    },
  };
}

function serveBuildArtifactsRaw(): Plugin {
  return {
    name: 'emx11-serve-build-artifacts-raw',
    configureServer(server) {
      server.middlewares.use((req, res, next) => {
        if (!req.url) return next();
        const pathname = req.url.split('?')[0];
        if (!pathname.startsWith('/artifacts/')) {
          return next();
        }
        const filePath = resolve(__dirname, 'build' + pathname);
        if (!existsSync(filePath)) return next();
        const ext = pathname.split('.').pop()?.toLowerCase();
        const mimeTypes: Record<string, string> = {
          js: 'application/javascript',
          wasm: 'application/wasm',
          data: 'application/octet-stream',
        };
        res.setHeader('Content-Type', mimeTypes[ext ?? ''] ?? 'application/octet-stream');
        res.setHeader('Cache-Control', 'no-cache');
        res.statusCode = 200;
        res.end(readFileSync(filePath));
      });
    },
  };
}

/**
 * Copy build/artifacts/ → dist/artifacts/ at the end of `vite build`.
 *
 * Demos `import('/artifacts/<name>/<name>.js')` and Emscripten
 * fetches the sibling `.wasm`. In dev these resolve via the raw-serve
 * middleware. In preview / static deploys, only `dist/` is served, so
 * unless the artifacts are mirrored into `dist/` the URLs hit the SPA
 * fallback (`index.html`) and dynamic-import fails with
 * "Unexpected token '<'".
 *
 * Uses fs.cpSync (Node ≥ 16.7); cheap because the cmake outputs are a
 * handful of MB total. Skips silently if the artifacts directory is
 * missing — that just means `pnpm build:native` hasn't run yet, and
 * the JS-only build is still useful (e.g. CI typecheck builds).
 */
function copyBuildArtifacts(): Plugin {
  return {
    name: 'emx11-copy-build-artifacts',
    apply: 'build',
    closeBundle() {
      const src = resolve(__dirname, 'build/artifacts');
      const dst = resolve(__dirname, 'dist/artifacts');
      if (!existsSync(src)) {
        // eslint-disable-next-line no-console
        console.warn(
          `[emx11] build/artifacts/ not found; skipping dist copy. ` +
            `Run 'pnpm build:native' first if you need a runnable preview.`,
        );
        return;
      }
      cpSync(src, dst, { recursive: true });
    },
  };
}

/**
 * Copy Layer‑1 example HTML files as-is into dist/examples/.
 *
 * These demos contain only dynamic import('/artifacts/...') — no src/
 * imports — so Rollup can't bundle them.  We copy them verbatim instead.
 * The bundled twm-session demo is handled by Vite's own HTML output.
 */
function copyLayer1Examples(): Plugin {
  return {
    name: 'emx11-copy-layer1-examples',
    apply: 'build',
    closeBundle() {
      const examplesDir = resolve(__dirname, 'examples');
      for (const d of staticLayer1Entries()) {
        const src = resolve(examplesDir, d.name, 'index.html');
        const dstDir = resolve(__dirname, 'dist', 'examples', d.name);
        if (!existsSync(dstDir)) mkdirSync(dstDir, { recursive: true });
        cpSync(src, resolve(dstDir, 'index.html'));
      }
    },
  };
}

export default defineConfig({
  root: '.',
  publicDir: 'public',

  plugins: [serveLayer1HtmlRaw(), serveBuildArtifactsRaw(), printDemoUrls(), copyBuildArtifacts(), copyLayer1Examples()],

  resolve: {
    alias: {
      '@': resolve(__dirname, 'src'),
    },
  },

  server: {
    port: 5173,
    headers: {
      // Required for SharedArrayBuffer (used by emscripten pthreads / JSPI Worker mode)
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
    },
    fs: {
      // Allow serving wasm artifacts from the out-of-root build directory.
      allow: ['.', 'build'],
    },
  },

  build: {
    target: 'es2022',
    outDir: 'dist',
    emptyOutDir: true,
    sourcemap: true,
    rollupOptions: {
      external: [/^\/artifacts\//],
      input: Object.fromEntries(
        [
          ['main', resolve(__dirname, 'index.html')],
          ...bundlableEntries().map((d) => [d.name, resolve(__dirname, `examples/${d.name}/index.html`)]),
        ],
      ),
    },
  },

  assetsInclude: ['**/*.wasm'],

  worker: {
    /* Emit workers as ES modules so dynamic `import(glueUrl)` inside the
     * Client Worker sees the glue's `export default` -- Emscripten's
     * MODULARIZE=1+EXPORT_ES6=1 output is ESM, and a classic-script
     * worker can't import it. */
    format: 'es',
  },

  optimizeDeps: {
    // Emscripten glue is ESM-ish but does odd things; exclude from pre-bundling.
    exclude: ['@/loader/wasm'],
  },
});
