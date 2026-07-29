import { defineConfig } from "vite";
import { fileURLToPath, URL } from "node:url";

export default defineConfig({
  resolve: {
    alias: {
      "@bindings": fileURLToPath(new URL("../../bindings", import.meta.url)),
    },
  },
  server: {
    port: 5174,
  },
});
