// Zen native loader: loads zen.node and provides the JavaScript slugify
// used as a fallback when the native Unicode table does not match this
// runtime's Unicode version. Written from scratch.
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
export const nativeAddon = require('./zen.node');

// JavaScript slugify, following lib/text.js exactly. Used only when the
// native table is incompatible (non-ASCII input on a runtime whose Unicode
// version differs from the table's).
export function slugifyJs(text) {
  const plain = text.normalize('NFKD').replace(/\p{M}/gu, '');
  let out = '';
  let gap = false;
  for (const ch of plain) {
    let keep = null;
    if (ch >= 'A' && ch <= 'Z') keep = ch.toLowerCase();
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
    if (keep === null) {
      gap = out.length > 0;
      continue;
    }
    if (gap) out += '-';
    gap = false;
    out += keep;
  }
  out = out.slice(0, 64).replace(/-+$/, '');
  return out === '' ? 'untitled' : out;
}

// The native table was generated from Unicode 17.0. The addon only trusts
// it when this runtime reports the same Unicode version.
slugifyJs.tableCompatible = process.versions.unicode === nativeAddon.unicodeVersion;
