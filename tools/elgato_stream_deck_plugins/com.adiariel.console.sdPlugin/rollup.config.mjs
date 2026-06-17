import commonjs from "@rollup/plugin-commonjs";
import nodeResolve from "@rollup/plugin-node-resolve";

export default {
  input: "src/plugin.js",
  output: {
    file: "bin/plugin.js",
    format: "esm",
    sourcemap: false
  },
  external: ["node:child_process", "node:os"],
  plugins: [
    nodeResolve({ preferBuiltins: true }),
    commonjs()
  ]
};
