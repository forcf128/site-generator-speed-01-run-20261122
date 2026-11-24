// Generate the NFKD slug table for the Zen engine, from this Node's own
// Unicode data. Never answers: it only records how each code point
// normalises, so the native slug matches Node's String.normalize('NFKD').
// Run: node tools/gen_unicode.mjs   (writes native/unicode.hpp)
//
// The table stores, per code point, the string it contributes to the
// slugify loop's input: NFKD, then combining marks (\p{M}) removed. That is
// exactly what slugify does before its loop, so the native loop can feed
// each byte of that string through the same keep/separator rule. A code
// point whose contribution is empty (a mark, or one that decomposes wholly
// to marks) is a drop: it never reaches the loop, so the gap is unchanged.
// A code point whose contribution is itself (no decomposition, not a mark)
// needs no entry: the native loop treats an unmapped non-ASCII code point
// as a separator, which is what the reference loop does with it.
import { writeFileSync } from 'node:fs';

const MARK = /\p{M}/gu;
const rows = []; // [cp, d] for code points whose contribution differs from themselves
const drops = []; // [first, last] ranges of code points that vanish
let dropStart = -1, dropPrev = -1;

for (let cp = 128; cp <= 0x10ffff; cp++) {
  const ch = String.fromCodePoint(cp);
  const d = ch.normalize('NFKD').replace(MARK, '');
  if (d === '') {
    if (dropStart < 0) dropStart = cp;
    dropPrev = cp;
  } else {
    if (dropStart >= 0) { drops.push([dropStart, dropPrev]); dropStart = -1; }
    // Store only when the contribution keeps at least one ASCII letter or
    // digit. Otherwise it is pure separators, which is exactly what the
    // native loop does with an unmapped non-ASCII code point, so no entry
    // is needed.
    if (d !== ch && /[A-Za-z0-9]/.test(d)) rows.push([cp, d]);
  }
}
if (dropStart >= 0) drops.push([dropStart, dropPrev]);

// Pack the contribution strings into one pool, as UTF-8 bytes. Emit the pool
// as a C++ string literal with non-ASCII bytes as 3-digit octal escapes:
// an octal escape consumes at most 3 digits, so it can never swallow the
// character after it (unlike \x, which is greedy). The header stays pure ASCII.
function cppLiteral(buf) {
  let out = '"';
  for (const b of buf) {
    if (b === 0x22) out += '\\"';
    else if (b === 0x5c) out += '\\\\';
    else if (b >= 0x20 && b < 0x7f) out += String.fromCharCode(b);
    else out += '\\' + b.toString(8).padStart(3, '0');
  }
  return out + '"';
}
let poolBuf = Buffer.alloc(0);
const table = rows.map(([cp, text]) => {
  const bytes = Buffer.from(text, 'utf8');
  const off = poolBuf.length;
  poolBuf = Buffer.concat([poolBuf, bytes]);
  return `  {${cp},${off},${bytes.length}},`;
}).join('\n');
const ranges = drops.map(([a, b]) => `  {${a},${b}},`).join('\n');

const header = `// NFKD slug transforms, generated from Node ${process.versions.node} / Unicode ${process.versions.unicode}.
// Regenerate with: node tools/gen_unicode.mjs
#pragma once
namespace zen {
inline constexpr char ZEN_UNICODE_VERSION[] = ${JSON.stringify(process.versions.unicode)};
struct ZSlugMap { unsigned cp, off, len; };
struct ZDropRange { unsigned first, last; };
inline constexpr char ZSLUG_POOL[] = ${cppLiteral(poolBuf)};
inline constexpr ZSlugMap ZSLUG_MAP[] = {
${table}
};
inline constexpr ZDropRange ZSLUG_DROPS[] = {
${ranges}
};
} // namespace zen
`;
writeFileSync(new URL('../native/unicode.hpp', import.meta.url), header);
console.log(JSON.stringify({
  node: process.versions.node,
  unicode: process.versions.unicode,
  mappings: rows.length,
  dropRanges: drops.length,
  poolBytes: poolBuf.length,
  headerBytes: Buffer.byteLength(header),
}));
