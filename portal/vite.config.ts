import { defineConfig } from 'vite'
import preact from '@preact/preset-vite'
import compression from 'vite-plugin-compression'

export default defineConfig({
  plugins: [
    preact(),
    // firmware serves the pre-compressed assets (`Content-Encoding: gzip`)
    compression({ algorithm: 'gzip', deleteOriginFile: true })
  ],
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    minify: 'esbuild',
    rollupOptions: {
      output: {
        manualChunks: undefined,
        inlineDynamicImports: true,
        entryFileNames: 'assets/app.js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: 'assets/[name].[ext]'
      }
    }
  }
})
