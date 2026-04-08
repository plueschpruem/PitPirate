import { defineConfig, Plugin } from "vite";
import vue from "@vitejs/plugin-vue";
import vuetify from "vite-plugin-vuetify";
import { fileURLToPath, URL } from "url";
import { createHash } from "crypto";
import { gzipSync } from "zlib";
import { readFileSync, writeFileSync, existsSync, readdirSync, unlinkSync, cpSync, mkdirSync, rmSync } from "fs";
import { resolve, relative, extname } from "path";

// ── Set this to your ESP32's IP for `npm run dev` HMR proxy ──────────────────
const ESP32_IP = "http://192.168.179.7";

// ── Cache-busting: appends ?v=<content-hash> to every /assets/* URL in ───────
// index.html after the build so browsers always re-fetch changed assets.
// Stable filenames are kept (no hash in the filename itself) so LittleFS
// paths stay short; the query string is ignored by the ESP32 web server.
function assetVersionPlugin(): Plugin {
  let outDir: string;

  return {
    name: "asset-version",
    apply: "build",
    configResolved(config) {
      outDir = config.build.outDir; // resolved absolute path by Vite
    },
    buildStart() {
      // No-op: emptyOutDir:true already wipes data/ before each build.
    },
    closeBundle() {
      const htmlPath = resolve(outDir, "index.html");
      if (!existsSync(htmlPath)) return;

      let html = readFileSync(htmlPath, "utf-8");
      html = html.replace(/(src|href)="(\/assets\/[^"?#]+)"/g, (_: string, attr: string, path: string) => {
        const filePath = resolve(outDir, path.slice(1)); // strip leading /
        if (existsSync(filePath)) {
          const hash = createHash("md5").update(readFileSync(filePath)).digest("hex").slice(0, 8);
          return `${attr}="${path}?v=${hash}"`;
        }
        return `${attr}="${path}"`;
      });
      writeFileSync(htmlPath, html);
    },
  };
}

// ── Copy web assets to ../server/ after build ─────────────────────────────────
// PHP files, manifest.json and sw.js that live in server/ are preserved.
// Only .html / .js / .css / .png / .svg / .ico / .webmanifest are synced.
const SYNC_EXTS = new Set([".html", ".js", ".css", ".png", ".svg", ".ico", ".webmanifest", ".woff", ".woff2", ".ttf", ".json"]);
// json files in the server root (config, subscriptions) must not be touched
const SERVER_ROOT_JSON_KEEP = new Set(["manifest.json"]);

function copyToServerPlugin(): Plugin {
  return {
    name: "copy-to-server",
    apply: "build",
    closeBundle() {
      const dataDir = resolve(__dirname, "../data");
      const serverDir = resolve(__dirname, "../server");
      mkdirSync(serverDir, { recursive: true });

      // Clean server/assets/ before copying so stale chunks don't accumulate.
      const serverAssetsDir = resolve(serverDir, "assets");
      if (existsSync(serverAssetsDir)) {
        rmSync(serverAssetsDir, { recursive: true, force: true });
      }

      // Walk data/ and copy every web-asset file, preserving structure.
      // Skips files that live in the server root and are not web assets
      // (e.g. .php, subscriptions.json, api_debug.log).
      const walk = (src: string, dst: string) => {
        for (const entry of readdirSync(src, { withFileTypes: true })) {
          const srcPath = resolve(src, entry.name);
          const dstPath = resolve(dst, entry.name);
          if (entry.isDirectory()) {
            mkdirSync(dstPath, { recursive: true });
            walk(srcPath, dstPath);
          } else {
            const ext = extname(entry.name).toLowerCase();
            // In the server root, keep manifest.json but skip other .json files
            const isServerRoot = relative(dataDir, src) === "";
            if (isServerRoot && ext === ".json" && !SERVER_ROOT_JSON_KEEP.has(entry.name)) continue;
            if (SYNC_EXTS.has(ext)) {
              writeFileSync(dstPath, readFileSync(srcPath));
            }
          }
        }
      };
      walk(dataDir, serverDir);
      console.log(">>> Copied web assets from data/ → server/");
    },
  };
}

// ── Gzip JS/CSS/HTML in data/ after copy so LittleFS image stays small ────────
// copyToServerPlugin runs first (server/ gets uncompressed originals).
// Browsers handle Content-Encoding: gzip transparently.
function gzipDataPlugin(): Plugin {
  return {
    name: "gzip-data",
    apply: "build",
    closeBundle() {
      const dataDir = resolve(__dirname, "../data");
      const compressible = new Set([".js", ".css", ".html"]);
      const walk = (dir: string) => {
        for (const entry of readdirSync(dir, { withFileTypes: true })) {
          const full = resolve(dir, entry.name);
          if (entry.isDirectory()) {
            walk(full);
            continue;
          }
          if (!compressible.has(extname(entry.name).toLowerCase())) continue;
          const raw = readFileSync(full);
          if (raw[0] === 0x1f && raw[1] === 0x8b) continue; // already gzipped
          const compressed = gzipSync(raw, { level: 9 });
          writeFileSync(full, compressed);
          const rel = relative(dataDir, full);
          console.log(`  gzip: ${rel}  (${Math.round(raw.length / 1024)}K → ${Math.round(compressed.length / 1024)}K)`);
        }
      };
      console.log(">>> Gzip-compressing JS/CSS/HTML for LittleFS…");
      walk(dataDir);
      console.log(">>> Compression complete.");
      // Make sure .gitkeep is always there, even after build cleanup
      writeFileSync(`${dataDir}/.gitkeep`, "");
    },
  };
}

export default defineConfig({
  plugins: [
    vue(),
    vuetify(),
    assetVersionPlugin(),
    copyToServerPlugin(), // copies uncompressed data/ → server/ first
    gzipDataPlugin(), // then gzips data/ in-place for LittleFS
  ],

  resolve: {
    alias: {
      "@": fileURLToPath(new URL("src", import.meta.url)),
    },
  },

  css: {
    preprocessorOptions: {
      scss: { loadPaths: ["node_modules"] },
      sass: { loadPaths: ["node_modules"] },
    },
  },

  build: {
    // Output goes directly into the PlatformIO data/ folder for LittleFS upload
    outDir: "../data",
    // pirate.bin removed; pirate_alpha_240h.png is in public/ so it's rebuilt.
    emptyOutDir: true,
    rollupOptions: {
      output: {
        // Stable filenames (no content hashes) — keeps LittleFS paths short.
        // Use a function for chunkFileNames because Rollup pre-bakes an 8-char
        // base64url hash into the [name] token itself for lazy-loaded chunks
        // (e.g. vue-data-ui internals), making the string template useless.
        entryFileNames: "assets/[name].js",
        chunkFileNames: (chunk) => {
          // Strip trailing -XXXXXXXX disambiguator (8 base64url chars) that
          // Rollup adds when chunk names collide.
          const name = chunk.name.replace(/-[A-Za-z0-9_-]{8}$/, "");
          return `assets/${name}.js`;
        },
        assetFileNames: "assets/[name].[ext]",
        // Split stable vendor libraries into separate chunks so they survive
        // firmware updates that only touch app code. Browsers (and the ESP
        // cache-control headers) can keep vue-vendor.js and chart-vendor.js
        // across updates while only re-downloading index.js.
        manualChunks: {
          "vue-vendor": ["vue", "vuetify"],
        },
      },
    },
  },

  server: {
    // Proxy all API calls to the real ESP32 during development
    proxy: {
      "/data": { target: ESP32_IP, changeOrigin: true },
      "/sc": { target: ESP32_IP, changeOrigin: true },
      "/settings": { target: ESP32_IP, changeOrigin: true },
      "/tuya-config": { target: ESP32_IP, changeOrigin: true },
      "/remote-config": { target: ESP32_IP, changeOrigin: true },
      "/save-remote": { target: ESP32_IP, changeOrigin: true },
      "/alarm-config": { target: ESP32_IP, changeOrigin: true },
      "/save-alarms": { target: ESP32_IP, changeOrigin: true },
      "/wifi-config": { target: ESP32_IP, changeOrigin: true },
      "/wifi-scan": { target: ESP32_IP, changeOrigin: true },
      "/save-wifi": { target: ESP32_IP, changeOrigin: true },
      "/fan-config": { target: ESP32_IP, changeOrigin: true },
      "/save-fan": { target: ESP32_IP, changeOrigin: true },
      "/save-fan-settings": { target: ESP32_IP, changeOrigin: true },
      "/pid-config": { target: ESP32_IP, changeOrigin: true },
      "/save-pid": { target: ESP32_IP, changeOrigin: true },
      "/telemetry-config": { target: ESP32_IP, changeOrigin: true },
      "/save-telemetry-config": { target: ESP32_IP, changeOrigin: true },
    },
  },
});
