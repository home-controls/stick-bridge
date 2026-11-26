// Folds Vite's built JS and CSS into one HTML file for the firmware to embed.
//   node tools/inline_web.mjs web/dist/index.html main/web/index.html
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";

const [src, out] = process.argv.slice(2);
if (!src || !out) {
  console.error("usage: inline_web.mjs <dist/index.html> <out.html>");
  process.exit(2);
}
const base = dirname(resolve(src));
let html = readFileSync(src, "utf8");
const read = (p) => readFileSync(resolve(base, p.replace(/^\.?\//, "")), "utf8");

html = html.replace(/<link rel="stylesheet"[^>]*href="([^"]+)"[^>]*>/g, (_, href) => `<style>${read(href)}</style>`);
html = html.replace(/<script type="module"[^>]*src="([^"]+)"[^>]*><\/script>/g, (_, s) =>
  // "</script" inside the bundle would end the tag early
  `<script type="module">${read(s).replace(/<\/script/gi, "<\\/script")}</script>`);
if (/(src|href)="\.?\/?assets\//.test(html)) {
  console.error("inline_web: an asset reference was left behind");
  process.exit(1);
}
writeFileSync(out, html);
console.log(`inline_web: wrote ${out} (${(html.length / 1024).toFixed(1)} KiB)`);
