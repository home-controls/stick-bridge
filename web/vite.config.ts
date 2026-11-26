import { defineConfig } from "vite";
import preact from "@preact/preset-vite";

// One page, served from the bridge's flash: `npm run build` bundles it, then tools/inline_web.mjs
// folds the JS and CSS into main/web/index.html, which the firmware build gzips and embeds.
// `npm run dev` proxies the API to a bridge (BRIDGE=http://192.168.1.50 by default).
const bridge = process.env.BRIDGE || "http://192.168.1.50";

export default defineConfig({
  base: "./",
  plugins: [preact()],
  build: {
    outDir: "dist",
    emptyOutDir: true,
    target: "es2022",
    assetsInlineLimit: 100000,
    cssCodeSplit: false,
    modulePreload: false,
    reportCompressedSize: true,
  },
  server: {
    port: 5174,
    proxy: { "/api": { target: bridge, changeOrigin: true }, "/metrics": { target: bridge, changeOrigin: true } },
  },
});
