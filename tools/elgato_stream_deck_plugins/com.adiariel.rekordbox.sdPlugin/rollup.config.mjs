import commonjs from "@rollup/plugin-commonjs";
import nodeResolve from "@rollup/plugin-node-resolve";

// Bundles src -> bin/plugin.js (ESM, run by the Stream Deck app's Node 20).
// easymidi / @julusian/midi are NOT bundled: they are loaded at runtime from
// the committed vendor/ tree via createRequire (native .node prebuilds can't
// live inside a JS bundle). See scripts/vendor.mjs.
export default {
  input: "src/plugin.js",
  output: {
    file: "bin/plugin.js",
    format: "esm",
    sourcemap: false
  },
  external: ["node:module", "node:os", "node:path", "node:url"],
  plugins: [
    nodeResolve({ preferBuiltins: true }),
    commonjs()
  ]
};
