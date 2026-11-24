// Build script: compiles native/addon.cc into zen.node (the prebuilt binary
// shipped in the repo). Usage:
//   node tools/build.mjs            normal optimised build
//   node tools/build.mjs --pgo      profile-guided build (two passes over the
//                                   practice input; falls back to normal on error)
// The Node 22.23.4 headers live in the work dir (NODE_HEADERS overrides).
import { execSync, spawnSync } from 'node:child_process';
import { existsSync, copyFileSync, mkdirSync, rmSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = dirname(dirname(fileURLToPath(import.meta.url)));
const base = dirname(root); // .../site-generator-speed-01-run-20261122
const headers = process.env.NODE_HEADERS ??
  join(base, 'work', 'node-v22.23.4', 'include', 'node');
const kitPractice = join(base, 'kit', 'site-generator-speed-01-run-20261122-kit', 'practice', 'input', 'cases.json');
const profDir = join(base, 'work', 'pgo');
const tmpNode = join(root, 'zen-pgo.node');

const CXX = process.env.CXX ?? 'g++';
const common = [
  '-O3', '-flto', '-fPIC', '-shared',
  `-I${headers}`, '-DNAPI_VERSION=8',
  '-fno-rtti',
  '-Wall', '-Wno-unused-parameter',
];
function compile(out, extra) {
  const cmd = [CXX, ...common, ...extra, join(root, 'native', 'addon.cc'), '-o', out].join(' ');
  console.log('+', cmd);
  execSync(cmd, { stdio: 'inherit' });
}
function strip(file) {
  execSync(`strip ${file}`, { stdio: 'inherit' });
}

const pgo = process.argv.includes('--pgo');
const out = join(root, 'zen.node');
if (!existsSync(join(headers, 'node_api.h'))) {
  console.error('node_api.h not found under', headers);
  console.error('set NODE_HEADERS to the Node 22.23.4 include/node directory');
  process.exit(1);
}
if (!pgo) {
  compile(out, []);
  strip(out);
  console.log('built', out);
  process.exit(0);
}
// PGO: instrument, run on the practice input, rebuild with the profile.
try {
  mkdirSync(profDir, { recursive: true });
  rmSync(profDir, { recursive: true, force: true });
  mkdirSync(profDir, { recursive: true });
  compile(tmpNode, [`-fprofile-generate=${profDir}`]);
  copyFileSync(tmpNode, out);
  const run = spawnSync(process.execPath,
    ['--input-type=module', '-e',
     `import('./solve.js').then(m => m.runBatch(${JSON.stringify(kitPractice)}, '/tmp/zen-pgo-answers.json'))`],
    { cwd: root, stdio: 'inherit' });
  if (run.status !== 0) throw new Error('PGO training run failed');
  compile(out, [`-fprofile-use=${profDir}`, '-fprofile-correction']);
  strip(out);
  rmSync(tmpNode, { force: true });
  console.log('built (PGO)', out);
} catch (e) {
  console.error('PGO failed, falling back to a normal build:', e.message);
  try { rmSync(tmpNode, { force: true }); } catch {}
  compile(out, []);
  strip(out);
  console.log('built (fallback)', out);
}
