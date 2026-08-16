import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// The dev server proxies /api to the C++ binary.
//
// WHY A PROXY RATHER THAN AN ABSOLUTE BASE URL
//
// With a proxy the browser only ever talks to one origin, so the app fetches
// "/api/..." in development and in production without a build-time switch. The
// alternative - VITE_API_URL baked in at build time - means the deployed bundle
// carries a hostname, and the one thing you cannot change after shipping a
// static bundle is a hostname compiled into it.
//
// The CORS headers the server sends are belt and braces for anyone who runs the
// dev server against a remote instance without this proxy.
export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    port: 5173,
    proxy: {
      '/api': {
        target: 'http://127.0.0.1:8080',
        changeOrigin: true,
      },
    },
  },
  build: {
    // `sextant serve --static web/dist` serves this directory.
    outDir: 'dist',
    sourcemap: true,
  },
})
