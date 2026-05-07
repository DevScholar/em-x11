import { defineConfig } from 'vite';
import type { Plugin, ResolvedServerUrls } from 'vite';
import { resolve } from 'node:path';
import { readdirSync, statSync, existsSync, cpSync } from 'node:fs';

/**
 * Auto-discovers demos/<name>/index.html entries and prints their URLs
 * after Vite's own "Local" / "Network" URL block, so you don't have to
 * remember the paths.
 */
function listDemoEntries(): { name: string; path: string }[] {
  const demosDir = resolve(__dirname, 'demos');
  if (!existsSync(demosDir)) return [];
  return readdirSync(demosDir)
    .filter((name) => {
      const entry = resolve(demosDir, name, 'index.html');
      return statSync(resolve(demosDir, name)).isDirectory() && existsSync(entry);
    })
    .map((name) => ({ name, path: `/demos/${name}/` }));
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
      };
    },
  };
}

/**
 * Copy build/artifacts/ → dist/build/artifacts/ at the end of `vite build`.
 *
 * Demos `import('/build/artifacts/<name>/<name>.js')` and Emscripten
 * fetches the sibling `.wasm`. In dev these resolve via `server.fs.allow:
 * ['build']`. In preview / static deploys, only `dist/` is served, so
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
      const dst = resolve(__dirname, 'dist/build/artifacts');
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

export default defineConfig({
  root: '.',
  publicDir: 'public',

  plugins: [printDemoUrls(), copyBuildArtifacts()],

  resolve: {
    alias: {
      '@': resolve(__dirname, 'src'),
    },
  },

  server: {
    port: 5173,
    headers: {
      // Required for SharedArrayBuffer (used by emscripten pthreads / Asyncify optional)
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
      input: Object.fromEntries(
        [
          ['main', resolve(__dirname, 'index.html')],
          ...listDemoEntries().map((d) => [d.name, resolve(__dirname, `demos/${d.name}/index.html`)]),
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
