// Differential verifier: generates fresh cases from the kit's generator,
// runs the oracle (makefun-site) and this entry on each, and compares the
// answers byte for byte (as JSON values, the way the scorer compares them).
// Also runs the three edge-case regressions (cyclic include, NBSP word
// count, page.constructor) and a determinism check (two runs identical).
// Usage: node tools/verify.mjs [count=30] [seedStart=1000]
// The kit dir is found relative to this repo (../kit/...) or via KIT_DIR.
import { createHash } from 'node:crypto';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

const root = dirname(dirname(fileURLToPath(import.meta.url)));
const base = dirname(root);
const kit = process.env.KIT_DIR ?? join(base, 'kit', 'site-generator-speed-01-run-20261122-kit');
const genDir = join(kit, 'program', 'generator');

const { streamFor } = await import(pathToFileURL(join(genDir, 'rng.ts')).href);
const { generate } = await import(join(genDir, 'subject', 'sites.mjs'));
const { run: oracleRun } = await import(join(genDir, 'oracle.js'));
const { run: myRun } = await import(join(root, 'solve.js'));

const count = Number(process.argv[2] ?? 30);
const seedStart = Number(process.argv[3] ?? 1000);

function seedFor(n) {
  return createHash('sha256').update(`zen-verify:${n}`).digest('hex');
}
function same(a, b) {
  return JSON.stringify(a) === JSON.stringify(b);
}
function firstDiff(a, b, prefix = '') {
  if (typeof a !== typeof b) return `${prefix}: type ${typeof a} vs ${typeof b}`;
  if (a === null || b === null || typeof a !== 'object') {
    return a === b ? null : `${prefix}: ${JSON.stringify(a)} vs ${JSON.stringify(b)}`;
  }
  const keys = new Set([...Object.keys(a), ...Object.keys(b)]);
  for (const k of keys) {
    if (!(k in a)) return `${prefix}${k}: missing in mine`;
    if (!(k in b)) return `${prefix}${k}: extra in mine`;
    const d = firstDiff(a[k], b[k], `${prefix}${k}.`);
    if (d) return d;
  }
  return null;
}

let diffPass = 0, diffFail = 0;
const t0 = Date.now();
for (let n = seedStart; n < seedStart + count; n++) {
  const seed = seedFor(n);
  for (let idx = 0; idx < 3; idx++) {
    const input = generate(streamFor(seed, 'case', idx), idx);
    const expected = oracleRun(structuredClone(input));
    const mine = myRun(structuredClone(input));
    if (same(mine, expected)) diffPass++;
    else {
      diffFail++;
      console.log(`DIFF seed=${n} idx=${idx}:`, firstDiff(mine, expected));
    }
  }
}
console.log(`differential: ${diffPass} passed, ${diffFail} failed (${count * 3} cases, ${Date.now() - t0} ms)`);

// --- Edge cases ------------------------------------------------------------
function site(templates, files) {
  return { site: { title: 'T', base_url: 'https://example.test/blog/', per_page: 10, recent: 5 }, templates, files };
}
function page(md, path = 'a.md') {
  return { path, content: md };
}
let edgePass = 0, edgeFail = 0;
function check(name, input) {
  const expected = oracleRun(structuredClone(input));
  const mine = myRun(structuredClone(input));
  const ok = same(mine, expected);
  console.log(`edge ${name}: ${ok ? 'PASS' : 'FAIL'}`);
  if (!ok) { console.log('  diff:', firstDiff(mine, expected)); console.log('  mine:', JSON.stringify(mine).slice(0, 400)); console.log('  exp :', JSON.stringify(expected).slice(0, 400)); }
  ok ? edgePass++ : edgeFail++;
}

// 1. Cyclic include: the layout includes "cycle", which includes itself.
//    The absolute depth budget renders exactly 8 X's.
check('cyclic-include', site(
  { page: '{% include "cycle" %}', cycle: 'X{% include "cycle" %}' },
  [page('---\ntitle: T\n---\nbody\n')]));
// 2. NBSP word count: 201 words joined by NBSP. NBSP is JavaScript
//    whitespace, so there are 201 words and reading_minutes is 2.
check('nbsp-word-count', site(
  { page: '{{ page.reading_minutes }}' },
  [page('---\ntitle: T\n---\n' + ('word' + '\u00a0').repeat(201))]));
// 3. page.constructor: inherited properties are not own properties, so the
//    lookup misses and renders "". Array length IS own, so it renders.
check('page-constructor', site(
  { page: '[{{ page.constructor }}|{{ page.tags.constructor }}|{{ page.toc.length }}]' },
  [page('---\ntitle: T\ntags: [x]\n---\n## H\n\ntext\n')]));
// 4. CRLF input, escaping, nested loops, invalid front matter, slug
//    collision, tag ordering, report ordering.
check('crlf-and-escaping', site(
  { page: '{{ page.content }}' },
  [page('---\r\ntitle: A & B <C>\r\n---\r\n# H & <x>\r\n\r\npara with `code` and **bold** and *em* and [link](a.md)\r\n')]));
check('nested-loops', site(
  { page: '{% for t in page.tags %}{% for p in site.tags %}{{ p.name }};{% endfor %}{% endfor %}' },
  [page('---\ntitle: T\ntags: [one, two]\n---\nbody\n'),
   { path: 'posts/p.md', content: '---\ntitle: P\ndate: 2026-01-02\ntags: [one]\n---\nbody\n' }]));
check('invalid-front-matter', site(
  { page: '{{ page.title }}' },
  [page('---\ntitle: T\nbogus: 1\n---\nbody\n'),
   page('---\ntitle: U\n---\nbody\n', 'b.md')]));
check('slug-collision', site(
  { page: '{{ page.url }}' },
  [page('---\ntitle: Same\n---\nbody\n'),
   page('---\ntitle: Same\n---\nbody\n', 'b.md'),
   page('---\ntitle: Same\nslug: same\n---\nbody\n', 'c.md')]));
check('tag-ordering', site(
  { page: '{% for t in tags %}{{ t.slug }}:{{ t.count }};{% endfor %}' },
  [{ path: 'posts/a.md', content: '---\ntitle: A\ndate: 2026-01-03\ntags: [z, y]\n---\nbody\n' },
   { path: 'posts/b.md', content: '---\ntitle: B\ndate: 2026-01-02\ntags: [z]\n---\nbody\n' },
   { path: 'posts/c.md', content: '---\ntitle: C\ndate: 2026-01-01\ntags: [y, y, z]\n---\nbody\n' }]));
check('reserved-dirs', site(
  { page: '{{ page.title }}' },
  [page('---\ntitle: T\n---\nbody\n'),
   { path: 'tags/x.md', content: '---\ntitle: X\n---\nbody\n' },
   { path: 'page/y.md', content: '---\ntitle: Y\n---\nbody\n' },
   { path: 'posts/z.md', content: '---\ntitle: Z\n---\nbody\n' }]));
console.log(`edge cases: ${edgePass} passed, ${edgeFail} failed`);

// --- Determinism -----------------------------------------------------------
{
  const input = generate(streamFor(seedFor(seedStart), 'case', 0), 0);
  const a = JSON.stringify(myRun(structuredClone(input)));
  const b = JSON.stringify(myRun(structuredClone(input)));
  console.log(`determinism: ${a === b ? 'PASS' : 'FAIL'} (two runs byte-identical)`);
  if (a !== b) edgeFail++;
}

if (diffFail > 0 || edgeFail > 0) process.exit(1);
console.log('ALL GREEN');
