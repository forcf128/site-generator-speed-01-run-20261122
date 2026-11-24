// Zen solver: run(input) returns exactly what makefun-site 1.0.0 returns
// for the same input — the output tree as a JSON object of path -> text.
// The work is done by the native addon; this module is the contract surface.
// Written from scratch.
import { nativeAddon, slugifyJs } from './native.js';

/** makefun-site 1.0.0's answer for one case's input. */
export function run(input) {
  return JSON.parse(nativeAddon.runJson(JSON.stringify(input), slugifyJs));
}

/** The batch fast path: build every case in /input/cases.json and write
 * /output/answers.json, all inside the addon (no JSON round-trips). */
export function runBatch(inputPath, outputPath) {
  nativeAddon.runBatch(inputPath, outputPath, slugifyJs);
}

/** The slug rule, exposed for testing. */
export function slugify(text) {
  return slugifyJs.tableCompatible ? nativeAddon.slug(text) : slugifyJs(text);
}
