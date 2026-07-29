import { defineConfig } from "vite";
import { fileURLToPath, URL } from "node:url";

export default defineConfig({
  resolve: {
    alias: {
      "@bindings": fileURLToPath(new URL("../../bindings", import.meta.url)),
    },
  },
  server: {
    // Cross-origin isolation. Required for SharedArrayBuffer, and the
    // production deploy must set the same headers (docs/RISKS.md §1 — this
    // breaks late and invalidates test results if deferred to Phase 7).
    headers: {
      "Cross-Origin-Opener-Policy": "same-origin",
      "Cross-Origin-Embedder-Policy": "require-corp",
    },
  },
  worker: {
    // The task loop runs in a Web Worker, not the main thread — background
    // tabs throttle the main thread and rAF stops firing (CONVENTIONS.md §2).
    format: "es",
  },
});
