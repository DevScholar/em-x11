/**
 * Vite config for building the default Host as a standalone IIFE bundle.
 *
 * Output: build/artifacts/emx11-default-host.js
 *
 * This IIFE sets `globalThis.EmX11DefaultHost = { create(Module) }`,
 * which library_emx11.js calls during $EmX11Host.init() in Layer 1
 * (zero-JS / -sUSE_EMX11 mode).
 *
 * Usage: npx vite build -c vite.host.config.ts
 */

import { defineConfig } from 'vite';
import { resolve } from 'node:path';

export default defineConfig({
  root: '.',

  resolve: {
    alias: {
      '@': resolve(__dirname, 'src'),
    },
  },

  build: {
    target: 'es2022',
    outDir: 'build/artifacts',
    emptyOutDir: false, // don't wipe the cmake artifacts
    sourcemap: true,
    lib: {
      entry: resolve(__dirname, 'src/default-host.ts'),
      name: 'EmX11DefaultHost',
      formats: ['iife'],
      fileName: () => 'emx11-default-host.js',
    },
  },
});
