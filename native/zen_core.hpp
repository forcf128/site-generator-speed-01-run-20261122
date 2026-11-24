// Zen engine core: arena, JSON value tree, UTF-8, whitespace, SIMD scanning,
// HTML escaping, JSON parse/serialize, slug. Written from scratch for the
// makefun-site 1.0.0 contract. C++17, Linux x86-64.
#pragma once
#include <node_api.h>
#include "unicode.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace zen {

using Str = std::string;
using View = std::string_view;

// ---------------------------------------------------------------- CPU features
struct Cpu {
  bool avx2 = false;
  Cpu() {
#if defined(__x86_64__)
    unsigned eax, ebx, ecx, edx;
    if (__get_cpuid_max(0, nullptr) >= 7) {
      __cpuid_count(7, 0, eax, ebx, ecx, edx);
      avx2 = (ebx & (1u << 5)) != 0; // AVX2 bit
      // AVX2 also needs OSXSAVE + XMM/YMM state; check XCR0.
      if (avx2) {
        unsigned xcr0lo, xcr0hi;
        __asm__ volatile("xgetbv" : "=a"(xcr0lo), "=d"(xcr0hi) : "c"(0));
        avx2 = (xcr0lo & 0x6) == 0x6;
      }
    }
#endif
  }
};
inline const Cpu& cpu() { static const Cpu c; return c; }

// ---------------------------------------------------------------- UTF-8
inline void pushCp(Str& out, uint32_t cp) {
  if (cp < 0x80) out += char(cp);
  else if (cp < 0x800) { out += char(0xc0 | (cp >> 6)); out += char(0x80 | (cp & 63)); }
  else if (cp < 0x10000) { out += char(0xe0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
  else { out += char(0xf0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 63)); out += char(0x80 | ((cp >> 6) & 63)); out += char(0x80 | (cp & 63)); }
}

// Decode one code point at i (advances i). Assumes valid UTF-8.
inline uint32_t readCp(View s, size_t& i) {
  uint8_t a = uint8_t(s[i++]);
  if (a < 0x80) return a;
  int n = a < 0xe0 ? 1 : a < 0xf0 ? 2 : 3;
  uint32_t cp = a & (n == 1 ? 31u : n == 2 ? 15u : 7u);
  while (n-- > 0 && i < s.size()) cp = (cp << 6) | (uint8_t(s[i++]) & 63u);
  return cp;
}

// Convert UTF-8 to UTF-16 (for N-API strings).
inline std::u16string toUtf16(View s) {
  std::u16string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = readCp(s, i);
    if (cp <= 0xffff) out += char16_t(cp);
    else { cp -= 0x10000; out += char16_t(0xd800 + (cp >> 10)); out += char16_t(0xdc00 + (cp & 1023)); }
  }
  return out;
}

inline bool isAscii(View s) {
  for (unsigned char c : s) if (c >= 128) return false;
  return true;
}

// Code-unit order, UTF-16 comparison for non-ASCII (matches JS string <).
inline bool lessCodeUnits(View a, View b) {
  if (isAscii(a) && isAscii(b)) return a < b;
  return toUtf16(a) < toUtf16(b);
}

inline bool hasPrefix(View s, View p) { return s.size() >= p.size() && s.substr(0, p.size()) == p; }
inline bool hasSuffix(View s, View p) { return s.size() >= p.size() && s.substr(s.size() - p.size()) == p; }

// JavaScript whitespace (White_Space + LineTerminator), per the spec.
inline bool isWs(uint32_t c) {
  return c == 32 || (c >= 9 && c <= 13) || c == 160 || c == 0x1680 ||
    (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 ||
    c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}
inline bool hasWs(View s) {
  for (size_t i = 0; i < s.size();) if (isWs(readCp(s, i))) return true;
  return false;
}
inline View trimSpaces(View s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}
// Trim JavaScript whitespace from both ends.
inline View trimWs(View s) {
  size_t a = 0, first = 0, last = 0;
  bool seen = false;
  while (a < s.size()) {
    size_t at = a;
    uint32_t cp = readCp(s, a);
    if (!isWs(cp)) { if (!seen) first = at; seen = true; last = a; }
  }
  return seen ? s.substr(first, last - first) : View{};
}
inline std::vector<View> splitLines(View s) {
  std::vector<View> out;
  out.reserve(s.size() / 60 + 1);
  size_t a = 0;
  for (;;) {
    size_t b = s.find('\n', a);
    if (b == View::npos) { out.push_back(s.substr(a)); break; }
    out.push_back(s.substr(a, b - a));
    a = b + 1;
  }
  return out;
}
// A regex `.` does not match U+2028/U+2029; trim those (and \r/\n) from a line end.
inline View dotTrim(View s) {
  if (hasSuffix(s, "\xe2\x80\xa8") || hasSuffix(s, "\xe2\x80\xa9")) s.remove_suffix(3);
  else if (!s.empty() && (s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
  return s;
}
inline bool dotSafe(View s) {
  return s.find('\r') == View::npos && s.find('\n') == View::npos &&
    s.find("\xe2\x80\xa8") == View::npos && s.find("\xe2\x80\xa9") == View::npos;
}

// ---------------------------------------------------------------- SIMD scanning
// AVX2 lives in target-attributed functions, called only when the runtime
// CPU supports it (checked once via CPUID + XCR0). The rest of the file
// compiles for the x86-64 baseline (SSE2), so the binary is safe on any
// x86-64 CPU. Find the first byte in [at, size) that needs HTML escaping
// (or, for markdown, also a backslash, backtick, star or open bracket).
inline size_t scanSpecialScalar(View s, size_t at, bool md) {
  while (at < s.size()) {
    char c = s[at];
    if (c == '&' || c == '<' || c == '>' || c == '"' || c == '\'' ||
        (md && (c == '\\' || c == '`' || c == '*' || c == '['))) break;
    ++at;
  }
  return at;
}
#if defined(__x86_64__)
__attribute__((target("avx2")))
static size_t scanSpecialAvx2(View s, size_t at, bool md) {
  while (at + 32 <= s.size()) {
    __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s.data() + at));
    __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(x, _mm256_set1_epi8('&')), _mm256_cmpeq_epi8(x, _mm256_set1_epi8('<')));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(x, _mm256_set1_epi8('>')));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(x, _mm256_set1_epi8('"')));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(x, _mm256_set1_epi8('\'')));
    if (md) {
      m = _mm256_or_si256(m, _mm256_cmpeq_epi8(x, _mm256_set1_epi8('\\')));
      m = _mm256_or_si256(m, _mm256_cmpeq_epi8(x, _mm256_set1_epi8('`')));
      m = _mm256_or_si256(m, _mm256_cmpeq_epi8(x, _mm256_set1_epi8('*')));
      m = _mm256_or_si256(m, _mm256_cmpeq_epi8(x, _mm256_set1_epi8('[')));
    }
    unsigned bits = unsigned(_mm256_movemask_epi8(m));
    if (bits) return at + unsigned(__builtin_ctz(bits));
    at += 32;
  }
  return at;
}
static size_t scanSpecialSse2(View s, size_t at, bool md) {
  while (at + 16 <= s.size()) {
    __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.data() + at));
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(x, _mm_set1_epi8('&')), _mm_cmpeq_epi8(x, _mm_set1_epi8('<')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, _mm_set1_epi8('>')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, _mm_set1_epi8('"')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, _mm_set1_epi8('\'')));
    if (md) {
      m = _mm_or_si128(m, _mm_cmpeq_epi8(x, _mm_set1_epi8('\\')));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(x, _mm_set1_epi8('`')));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(x, _mm_set1_epi8('*')));
      m = _mm_or_si128(m, _mm_cmpeq_epi8(x, _mm_set1_epi8('[')));
    }
    unsigned bits = unsigned(_mm_movemask_epi8(m));
    if (bits) return at + unsigned(__builtin_ctz(bits));
    at += 16;
  }
  return at;
}
#endif
inline size_t scanSpecial(View s, size_t at, bool md) {
#if defined(__x86_64__)
  if (cpu().avx2) at = scanSpecialAvx2(s, at, md);
  else at = scanSpecialSse2(s, at, md);
#endif
  return scanSpecialScalar(s, at, md);
}

inline void escapeChar(Str& out, char c) {
  switch (c) {
    case '&': out += "&amp;"; break;
    case '<': out += "&lt;"; break;
    case '>': out += "&gt;"; break;
    case '"': out += "&quot;"; break;
    case '\'': out += "&#39;"; break;
    default: out += c;
  }
}
inline void escapeInto(Str& out, View s) {
  for (size_t i = 0; i < s.size();) {
    size_t k = scanSpecial(s, i, false);
    out.append(s.substr(i, k - i));
    if (k == s.size()) break;
    escapeChar(out, s[k]);
    i = k + 1;
  }
}
inline Str escaped(View s) { Str out; out.reserve(s.size() + 32); escapeInto(out, s); return out; }

// Find the first byte needing a JSON escape: control char, quote, backslash,
// or 0xED (start of a UTF-16 surrogate half in UTF-8).
inline size_t scanJsonScalar(View s, size_t at) {
  while (at < s.size()) {
    unsigned c = uint8_t(s[at]);
    if (c < 32 || c == '"' || c == '\\' || c == 0xed) break;
    ++at;
  }
  return at;
}
#if defined(__x86_64__)
__attribute__((target("avx2")))
static size_t scanJsonAvx2(View s, size_t at) {
  while (at + 32 <= s.size()) {
    __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s.data() + at));
    __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(x, _mm256_set1_epi8('"')), _mm256_cmpeq_epi8(x, _mm256_set1_epi8('\\')));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(_mm256_and_si256(x, _mm256_set1_epi8(char(0xe0))), _mm256_setzero_si256()));
    m = _mm256_or_si256(m, _mm256_cmpeq_epi8(x, _mm256_set1_epi8(char(0xed))));
    unsigned bits = unsigned(_mm256_movemask_epi8(m));
    if (bits) return at + unsigned(__builtin_ctz(bits));
    at += 32;
  }
  return at;
}
static size_t scanJsonSse2(View s, size_t at) {
  while (at + 16 <= s.size()) {
    __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.data() + at));
    __m128i m = _mm_or_si128(_mm_cmpeq_epi8(x, _mm_set1_epi8('"')), _mm_cmpeq_epi8(x, _mm_set1_epi8('\\')));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(_mm_and_si128(x, _mm_set1_epi8(char(0xe0))), _mm_setzero_si128()));
    m = _mm_or_si128(m, _mm_cmpeq_epi8(x, _mm_set1_epi8(char(0xed))));
    unsigned bits = unsigned(_mm_movemask_epi8(m));
    if (bits) return at + unsigned(__builtin_ctz(bits));
    at += 16;
  }
  return at;
}
#endif
inline size_t scanJson(View s, size_t at) {
#if defined(__x86_64__)
  if (cpu().avx2) at = scanJsonAvx2(s, at);
  else at = scanJsonSse2(s, at);
#endif
  return scanJsonScalar(s, at);
}
inline void hex4(Str& out, unsigned cp) {
  static const char* hex = "0123456789abcdef";
  out += "\\u";
  out += hex[(cp >> 12) & 15]; out += hex[(cp >> 8) & 15];
  out += hex[(cp >> 4) & 15]; out += hex[cp & 15];
}
inline void jsonQuote(Str& out, View s) {
  out += '"';
  for (size_t i = 0; i < s.size();) {
    size_t at = scanJson(s, i);
    out.append(s.substr(i, at - i));
    if (at == s.size()) break;
    unsigned c = uint8_t(s[at]);
    i = at + 1;
    if (c == 0xed) {
      i = at;
      uint32_t cp = readCp(s, i);
      if (cp >= 0xd800 && cp <= 0xdfff) hex4(out, cp);
      else out.append(s.substr(at, i - at));
      continue;
    }
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: hex4(out, c);
    }
  }
  out += '"';
}
inline Str quoted(View s) { Str out; jsonQuote(out, s); return out; }

// ---------------------------------------------------------------- Arena
// Chunked bump allocator: pointers into it never move. One arena per case;
// discard the whole arena when the case is done.
struct Arena {
  static constexpr size_t BLOCK = 1 << 20; // 1 MiB blocks
  std::vector<std::unique_ptr<char[]>> blocks;
  char* cur = nullptr;
  size_t left = 0;
  std::unordered_map<Str, int> syms;
  std::vector<Str> names;

  void* alloc(size_t n) {
    n = (n + 7) & ~size_t(7);
    if (n > left) {
      size_t sz = n > BLOCK ? n : BLOCK;
      blocks.emplace_back(new char[sz]);
      cur = blocks.back().get();
      left = sz;
    }
    void* p = cur;
    cur += n;
    left -= n;
    return p;
  }
  template <typename T> T* make() { return new (alloc(sizeof(T))) T(); }

  // Intern a key string; returns its id. The string must outlive the arena
  // (input spans and literals do; owned strings are kept via keep()).
  int key(View s) {
    auto it = syms.find(Str(s));
    if (it != syms.end()) return it->second;
    int id = int(names.size());
    names.emplace_back(s);
    syms.emplace(names.back(), id);
    return id;
  }
  // Copy a string into the arena and return a view of the copy.
  // (The View overload copies straight into the arena: going through a
  // std::string first would allocate a throwaway copy for every call.)
  View keep(View s) {
    char* p = static_cast<char*>(alloc(s.size() + 1));
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = '\0';
    return View(p, s.size());
  }
  View keep(Str s) { return keep(View(s)); }
};

// ---------------------------------------------------------------- JSON values
struct Val {
  enum Kind { Missing, Null, Bool, Number, String, Array, Object } kind = Missing;
  double number = 0;
  bool boolean = false;
  bool escapeReady = false;
  View text;   // String value
  View html;   // cached escaped form (template {{ }} only)
  std::vector<Val*> array;
  std::vector<std::pair<int, Val*>> object; // (key id, value), insertion order
};

struct Vals {
  Arena& a;
  Val missing;
  explicit Vals(Arena& ar) : a(ar) {}
  Val* make(Val::Kind k) { Val* v = a.make<Val>(); v->kind = k; return v; }
  Val* str(View s) { Val* v = make(Val::String); v->text = s; return v; }
  Val* own(Str s) { return str(a.keep(std::move(s))); }
  Val* own(View s) { return str(a.keep(s)); }
  Val* num(double n) { Val* v = make(Val::Number); v->number = n; return v; }
  Val* boolean(bool b) { Val* v = make(Val::Bool); v->boolean = b; return v; }
  Val* obj() { return make(Val::Object); }
  Val* arr() { return make(Val::Array); }
  void put(Val* o, int k, Val* v) {
    for (auto& p : o->object) if (p.first == k) { p.second = v; return; }
    o->object.emplace_back(k, v);
  }
  void put(Val* o, View k, Val* v) { put(o, a.key(k), v); }
  Val* get(Val* o, int k) {
    if (!o) return &missing;
    if (o->kind == Val::Object) {
      for (auto& p : o->object) if (p.first == k) return p.second;
    } else if (o->kind == Val::Array) {
      View s = a.names[k];
      if (s == "length") return num(double(o->array.size()));
      size_t n = 0;
      auto conv = std::from_chars(s.data(), s.data() + s.size(), n);
      if (!s.empty() && (s.size() == 1 || s[0] != '0') && conv.ec == std::errc{} &&
          conv.ptr == s.data() + s.size() && n < o->array.size())
        return o->array[n];
    }
    return &missing;
  }
  Val* get(Val* o, View k) { return get(o, a.key(k)); }
  bool has(Val* o, View k) { return get(o, k)->kind != Val::Missing; }
};

// ---------------------------------------------------------------- JSON parser
// Parses into Val trees. Plain strings are zero-copy spans of the input;
// escaped strings are materialised in the arena.
struct Json {
  View s;
  size_t p = 0;
  Arena& a;
  Vals& v;
  Json(View src, Arena& ar, Vals& vv) : s(src), a(ar), v(vv) {}
  [[noreturn]] void fail() { throw std::runtime_error("invalid JSON at byte " + std::to_string(p)); }
  void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\r' || s[p] == '\n' || s[p] == '\t')) ++p; }
  unsigned hex4() {
    if (p + 4 > s.size()) fail();
    unsigned n = 0;
    for (int i = 0; i < 4; i++) {
      char c = s[p++];
      int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (d < 0) fail();
      n = (n << 4) | unsigned(d);
    }
    return n;
  }
  // Next '"' or '\\' or control byte at or after p (a control byte is a
  // JSON error, reported by the caller). AVX2 + scalar.
#if defined(__x86_64__)
  __attribute__((target("avx2")))
  static size_t scanStringSpecialAvx2(View s, size_t p) {
    while (p + 32 <= s.size()) {
      __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s.data() + p));
      __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(x, _mm256_set1_epi8('"')), _mm256_cmpeq_epi8(x, _mm256_set1_epi8('\\')));
      __m256i low = _mm256_cmpeq_epi8(_mm256_and_si256(x, _mm256_set1_epi8(char(0xe0))), _mm256_setzero_si256());
      unsigned bits = unsigned(_mm256_movemask_epi8(_mm256_or_si256(m, low)));
      if (!bits) { p += 32; continue; }
      p += unsigned(__builtin_ctz(bits));
      break;
    }
    return p;
  }
#endif
  size_t scanStringSpecial(size_t p) {
#if defined(__x86_64__)
    if (cpu().avx2) {
      p = scanStringSpecialAvx2(s, p);
    } else {
      while (p + 16 <= s.size()) {
        __m128i x = _mm_loadu_si128(reinterpret_cast<const __m128i*>(s.data() + p));
        __m128i m = _mm_or_si128(_mm_cmpeq_epi8(x, _mm_set1_epi8('"')), _mm_cmpeq_epi8(x, _mm_set1_epi8('\\')));
        __m128i low = _mm_cmpeq_epi8(_mm_and_si128(x, _mm_set1_epi8(char(0xe0))), _mm_setzero_si128());
        unsigned bits = unsigned(_mm_movemask_epi8(_mm_or_si128(m, low)));
        if (!bits) { p += 16; continue; }
        return p + unsigned(__builtin_ctz(bits));
      }
    }
#endif
    while (p < s.size()) {
      if (s[p] == '"' || s[p] == '\\' || uint8_t(s[p]) < 32) break;
      ++p;
    }
    return p;
  }
  View string() {
    if (p >= s.size() || s[p++] != '"') fail();
    size_t start = p;
    size_t at = scanStringSpecial(p);
    if (at == s.size()) fail();
    if (s[at] == '"') { p = at + 1; return s.substr(start, at - start); }
    if (uint8_t(s[at]) < 32) fail();
    Str out(s.substr(start, at - start));
    p = at;
    while (p < s.size()) {
      size_t begin = p;
      size_t next = scanStringSpecial(p);
      out.append(s.substr(begin, next - begin));
      if (next == s.size()) fail();
      p = next;
      if (uint8_t(s[p]) < 32) fail();
      char c = s[p++];
      if (c == '"') return a.keep(std::move(out));
      if (p == s.size()) fail();
      c = s[p++];
      switch (c) {
        case '"': case '\\': case '/': out += c; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp = hex4();
          if (cp >= 0xd800 && cp <= 0xdbff && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
            size_t before = p;
            p += 2;
            unsigned low = hex4();
            if (low >= 0xdc00 && low <= 0xdfff) cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            else p = before;
          }
          pushCp(out, cp);
          break;
        }
        default: fail();
      }
    }
    fail();
  }
  Val* value(unsigned depth = 0) {
    if (depth > 256) fail();
    ws();
    if (p == s.size()) fail();
    char c = s[p];
    if (c == '"') return v.str(string());
    if (c == '[') {
      ++p;
      Val* arr = v.arr();
      ws();
      if (p < s.size() && s[p] == ']') { ++p; return arr; }
      for (;;) {
        arr->array.push_back(value(depth + 1));
        ws();
        if (p == s.size()) fail();
        c = s[p++];
        if (c == ']') return arr;
        if (c != ',') fail();
      }
    }
    if (c == '{') {
      ++p;
      Val* obj = v.obj();
      ws();
      if (p < s.size() && s[p] == '}') { ++p; return obj; }
      for (;;) {
        ws();
        int k = a.key(string());
        ws();
        if (p == s.size() || s[p++] != ':') fail();
        v.put(obj, k, value(depth + 1));
        ws();
        if (p == s.size()) fail();
        c = s[p++];
        if (c == '}') return obj;
        if (c != ',') fail();
      }
    }
    if (hasPrefix(s.substr(p), "true")) { p += 4; return v.boolean(true); }
    if (hasPrefix(s.substr(p), "false")) { p += 5; return v.boolean(false); }
    if (hasPrefix(s.substr(p), "null")) { p += 4; return v.make(Val::Null); }
    size_t start = p;
    while (p < s.size() && (s[p] == '-' || s[p] == '+' || s[p] == '.' || s[p] == 'e' || s[p] == 'E' || (s[p] >= '0' && s[p] <= '9'))) ++p;
    if (start == p) fail();
    double n = 0;
    Str text(s.substr(start, p - start));
    char* end = nullptr;
    n = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) fail();
    return v.num(n);
  }
  Val* parse() { Val* r = value(); ws(); if (p != s.size()) fail(); return r; }
};

// ---------------------------------------------------------------- Value -> text
inline void appendNumber(Str& out, double n) {
  if (std::isnan(n)) { out += "NaN"; return; }
  if (std::isinf(n)) { out += n < 0 ? "-Infinity" : "Infinity"; return; }
  if (n == 0) { out += '0'; return; }
  // Fast path: integral values in the common range.
  if (n == std::trunc(n) && n >= -9007199254740991.0 && n <= 9007199254740991.0) {
    char b[32];
    auto r = std::to_chars(b, b + 32, int64_t(n));
    out.append(b, r.ptr);
    return;
  }
  char b[64];
  auto r = std::to_chars(b, b + 64, n, std::chars_format::general);
  if (r.ec != std::errc{}) throw std::runtime_error("number formatting failed");
  // std::to_chars writes exponents with a leading zero (1e-07); JavaScript
  // writes 1e-7. Strip a leading zero from the exponent.
  Str s(b, r.ptr);
  size_t e = s.find('e');
  if (e != Str::npos && e + 2 < s.size() && s[e + 2] == '0') s.erase(e + 2, 1);
  out += s;
}
inline void textInto(Str& out, Val* val) {
  switch (val->kind) {
    case Val::String: out.append(val->text); break;
    case Val::Number: appendNumber(out, val->number); break;
    case Val::Bool: out += val->boolean ? "true" : "false"; break;
    case Val::Array:
      for (size_t i = 0; i < val->array.size(); i++) {
        if (i) out += ", ";
        textInto(out, val->array[i]);
      }
      break;
    default: break;
  }
}
inline bool truthy(Val* val) {
  switch (val->kind) {
    case Val::Missing: case Val::Null: return false;
    case Val::Bool: return val->boolean;
    case Val::Number: return val->number != 0;
    case Val::String: return !val->text.empty();
    case Val::Array: return !val->array.empty();
    default: return true;
  }
}

// ---------------------------------------------------------------- CRLF -> LF
inline Str lineEnds(View s) {
  Str out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\r') { out += '\n'; if (i + 1 < s.size() && s[i + 1] == '\n') ++i; }
    else out += s[i];
  }
  return out;
}

// ---------------------------------------------------------------- Output sink
// A growable byte buffer written with raw pointers, for the serialiser.
// std::string::append costs a bounds check + bookkeeping per call, and the
// JSON output is escape-dense (a `"` every ~33 bytes in HTML), so the
// per-call overhead dominates; writing through a raw pointer is ~34% faster.
struct Sink {
  char* buf = nullptr;
  size_t len = 0, cap = 0;
  ~Sink() { std::free(buf); }
  void reserve(size_t n) {
    if (n > cap) { buf = static_cast<char*>(std::realloc(buf, n)); if (!buf) throw std::bad_alloc(); cap = n; }
  }
  void ensure(size_t n) {
    if (len + n > cap) {
      size_t nc = cap ? cap : 4096;
      while (len + n > nc) nc *= 2;
      reserve(nc);
    }
  }
  void clear() { len = 0; }
  void append(View s) {
    ensure(s.size());
    std::memcpy(buf + len, s.data(), s.size());
    len += s.size();
  }
  void push(char c) { ensure(1); buf[len++] = c; }
  void pushStr(const char* s) { append(View(s)); }
};

// Raw-pointer JSON quoting into a Sink. Same output as jsonQuote, but the
// inner loop writes through a pointer instead of std::string::append.
// AVX2 variant: scanJsonAvx2 carries target("avx2"), so it cannot inline
// into a plain function — every escape (~224k of them for HTML) would pay
// a call plus a cpu().avx2 re-check. This copy inlines the scan, so the
// per-escape cost is just the scan itself. Kept byte-identical to
// jsonQuoteSink by construction (same loop, same escape table).
//
// Capacity: the caller (runBatch) reserves 2x the raw size, and every
// input byte becomes at most 2 output bytes except control chars /
// surrogates, which become 6 and are the only writes that still ensure().
// So the run memcpy and the 2-byte escapes need no per-call ensure —
// that was ~450k ensure calls per batch for nothing.
#if defined(__x86_64__)
__attribute__((target("avx2")))
static void jsonQuoteSinkAvx2(Sink& out, View s) {
  out.push('"');
  for (size_t i = 0; i < s.size();) {
    size_t at = i;
    while (at + 32 <= s.size()) {
      __m256i x = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(s.data() + at));
      __m256i m = _mm256_or_si256(_mm256_cmpeq_epi8(x, _mm256_set1_epi8('"')), _mm256_cmpeq_epi8(x, _mm256_set1_epi8('\\')));
      m = _mm256_or_si256(m, _mm256_cmpeq_epi8(_mm256_and_si256(x, _mm256_set1_epi8(char(0xe0))), _mm256_setzero_si256()));
      m = _mm256_or_si256(m, _mm256_cmpeq_epi8(x, _mm256_set1_epi8(char(0xed))));
      unsigned bits = unsigned(_mm256_movemask_epi8(m));
      if (bits) { at += unsigned(__builtin_ctz(bits)); break; }
      at += 32;
    }
    while (at < s.size()) {
      unsigned c = uint8_t(s[at]);
      if (c < 32 || c == '"' || c == '\\' || c == 0xed) break;
      ++at;
    }
    size_t run = at - i;
    std::memcpy(out.buf + out.len, s.data() + i, run);
    out.len += run;
    if (at == s.size()) break;
    unsigned c = uint8_t(s[at]);
    i = at + 1;
    if (c == 0xed) {
      size_t save = at;
      i = at;
      uint32_t cp = readCp(s, i);
      if (cp >= 0xd800 && cp <= 0xdfff) {
        static const char* hex = "0123456789abcdef";
        out.ensure(6);
        char* p = out.buf + out.len;
        p[0] = '\\'; p[1] = 'u';
        p[2] = hex[(cp >> 12) & 15]; p[3] = hex[(cp >> 8) & 15];
        p[4] = hex[(cp >> 4) & 15]; p[5] = hex[cp & 15];
        out.len += 6;
      } else {
        out.append(s.substr(save, i - save));
      }
      continue;
    }
    char* p = out.buf + out.len;
    switch (c) {
      case '"': p[0] = '\\'; p[1] = '"'; out.len += 2; break;
      case '\\': p[0] = '\\'; p[1] = '\\'; out.len += 2; break;
      case '\b': p[0] = '\\'; p[1] = 'b'; out.len += 2; break;
      case '\f': p[0] = '\\'; p[1] = 'f'; out.len += 2; break;
      case '\n': p[0] = '\\'; p[1] = 'n'; out.len += 2; break;
      case '\r': p[0] = '\\'; p[1] = 'r'; out.len += 2; break;
      case '\t': p[0] = '\\'; p[1] = 't'; out.len += 2; break;
      default: {
        static const char* hex = "0123456789abcdef";
        out.ensure(6);
        p = out.buf + out.len;
        p[0] = '\\'; p[1] = 'u'; p[2] = '0'; p[3] = '0';
        p[4] = hex[(c >> 4) & 15]; p[5] = hex[c & 15];
        out.len += 6;
      }
    }
  }
  out.push('"');
}
#endif
inline void jsonQuoteSink(Sink& out, View s) {
#if defined(__x86_64__)
  if (cpu().avx2) { jsonQuoteSinkAvx2(out, s); return; }
#endif
  out.push('"');
  for (size_t i = 0; i < s.size();) {
    size_t at = scanJson(s, i);
    size_t run = at - i;
    std::memcpy(out.buf + out.len, s.data() + i, run);
    out.len += run;
    if (at == s.size()) break;
    unsigned c = uint8_t(s[at]);
    i = at + 1;
    if (c == 0xed) {
      size_t save = at;
      i = at;
      uint32_t cp = readCp(s, i);
      if (cp >= 0xd800 && cp <= 0xdfff) {
        static const char* hex = "0123456789abcdef";
        out.ensure(6);
        char* p = out.buf + out.len;
        p[0] = '\\'; p[1] = 'u';
        p[2] = hex[(cp >> 12) & 15]; p[3] = hex[(cp >> 8) & 15];
        p[4] = hex[(cp >> 4) & 15]; p[5] = hex[cp & 15];
        out.len += 6;
      } else {
        out.append(s.substr(save, i - save));
      }
      continue;
    }
    char* p = out.buf + out.len;
    switch (c) {
      case '"': p[0] = '\\'; p[1] = '"'; out.len += 2; break;
      case '\\': p[0] = '\\'; p[1] = '\\'; out.len += 2; break;
      case '\b': p[0] = '\\'; p[1] = 'b'; out.len += 2; break;
      case '\f': p[0] = '\\'; p[1] = 'f'; out.len += 2; break;
      case '\n': p[0] = '\\'; p[1] = 'n'; out.len += 2; break;
      case '\r': p[0] = '\\'; p[1] = 'r'; out.len += 2; break;
      case '\t': p[0] = '\\'; p[1] = 't'; out.len += 2; break;
      default: {
        static const char* hex = "0123456789abcdef";
        out.ensure(6);
        p = out.buf + out.len;
        p[0] = '\\'; p[1] = 'u'; p[2] = '0'; p[3] = '0';
        p[4] = hex[(c >> 4) & 15]; p[5] = hex[c & 15];
        out.len += 6;
      }
    }
  }
  out.push('"');
}

// ---------------------------------------------------------------- Output tree
// A plain JS object's key order: integer-like keys (canonical numeric
// strings in [0, 2^32-2]) first, ascending numerically, then string keys in
// insertion order. Replicated here so the serialised tree matches
// JSON.stringify of the reference's output object byte for byte.
inline bool isIntKey(View s, uint32_t& n) {
  if (s.empty() || s.size() > 10) return false;
  if (s.size() > 1 && s[0] == '0') return false;
  uint64_t v = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + unsigned(c - '0');
  }
  if (v > 4294967294ULL) return false;
  n = uint32_t(v);
  return true;
}
struct Output {
  std::vector<std::pair<Str, Str>> files;
  std::unordered_map<Str, size_t> index;
  void put(Str path, Str text) {
    if (path == "__proto__") return; // assigning a string to __proto__ is a no-op in JS
    auto it = index.find(path);
    if (it == index.end()) { index.emplace(path, files.size()); files.emplace_back(std::move(path), std::move(text)); }
    else files[it->second].second = std::move(text);
  }
  bool has(View path) const { return path != "__proto__" && index.count(Str(path)) != 0; }
  void json(Str& out) const {
    size_t total = 2;
    for (auto& f : files) total += f.first.size() + f.second.size() + 8;
    out.reserve(out.size() + total);
    out += '{';
    bool first = true;
    auto emit = [&](size_t i) {
      if (!first) out += ',';
      first = false;
      jsonQuote(out, files[i].first);
      out += ':';
      jsonQuote(out, files[i].second);
    };
    // Integer-like keys come first in JSON.stringify order. Output paths
    // are file paths (they hold '/' or '.'), so in practice there are
    // never any; skip the flag vector entirely in that common case.
    std::vector<std::pair<uint32_t, size_t>> ints;
    for (size_t i = 0; i < files.size(); i++) {
      uint32_t n = 0;
      if (isIntKey(files[i].first, n)) ints.emplace_back(n, i);
    }
    if (ints.empty()) {
      for (size_t i = 0; i < files.size(); i++) emit(i);
    } else {
      std::sort(ints.begin(), ints.end());
      std::vector<char> isInt(files.size(), 0);
      for (auto& p : ints) { isInt[p.second] = 1; emit(p.second); }
      for (size_t i = 0; i < files.size(); i++) if (!isInt[i]) emit(i);
    }
    out += '}';
  }
  // Streaming form: same bytes as json(), written into a Sink with raw
  // pointers (no intermediate std::string, no second copy to the file).
  void jsonTo(Sink& out) const {
    out.push('{');
    bool first = true;
    auto emit = [&](size_t i) {
      if (!first) out.push(',');
      first = false;
      jsonQuoteSink(out, files[i].first);
      out.push(':');
      jsonQuoteSink(out, files[i].second);
    };
    std::vector<std::pair<uint32_t, size_t>> ints;
    for (size_t i = 0; i < files.size(); i++) {
      uint32_t n = 0;
      if (isIntKey(files[i].first, n)) ints.emplace_back(n, i);
    }
    if (ints.empty()) {
      for (size_t i = 0; i < files.size(); i++) emit(i);
    } else {
      std::sort(ints.begin(), ints.end());
      std::vector<char> isInt(files.size(), 0);
      for (auto& p : ints) { isInt[p.second] = 1; emit(p.second); }
      for (size_t i = 0; i < files.size(); i++) if (!isInt[i]) emit(i);
    }
    out.push('}');
  }
};

// ---------------------------------------------------------------- N-API helpers
inline void napiCheck(napi_env e, napi_status s) {
  if (s != napi_ok) {
    const napi_extended_error_info* info = nullptr;
    napi_get_last_error_info(e, &info);
    throw std::runtime_error(info && info->error_message ? info->error_message : "N-API error");
  }
}
inline napi_value jsString(napi_env e, View s) {
  auto u = toUtf16(s);
  napi_value val;
  napiCheck(e, napi_create_string_utf16(e, u.data(), u.size(), &val));
  return val;
}
inline Str nativeString(napi_env e, napi_value val) {
  size_t n = 0;
  napiCheck(e, napi_get_value_string_utf16(e, val, nullptr, 0, &n));
  std::u16string u(n + 1, 0);
  napiCheck(e, napi_get_value_string_utf16(e, val, u.data(), u.size(), &n));
  Str out;
  out.reserve(n);
  for (size_t i = 0; i < n; i++) {
    uint32_t cp = u[i];
    if (cp >= 0xd800 && cp <= 0xdbff && i + 1 < n && u[i + 1] >= 0xdc00 && u[i + 1] <= 0xdfff)
      cp = 0x10000 + ((cp - 0xd800) << 10) + (u[++i] - 0xdc00);
    pushCp(out, cp);
  }
  return out;
}

// ---------------------------------------------------------------- Slug (NFKD)
// Native table path; falls back to the JS normalize callback when the
// runtime's Unicode version differs from the table's.
inline Str nativeSlug(View s) {
  Str out;
  out.reserve(72);
  bool gap = false;
  auto feed = [&](char c) {
    if (c >= 'A' && c <= 'Z') c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) { if (gap) out += '-'; out += c; gap = false; }
    else gap = !out.empty();
  };
  for (size_t i = 0; i < s.size();) {
    uint32_t cp = readCp(s, i);
    if (cp < 128) { feed(char(cp)); continue; }
    auto it = std::lower_bound(std::begin(ZSLUG_MAP), std::end(ZSLUG_MAP), cp,
      [](const ZSlugMap& m, uint32_t c) { return m.cp < c; });
    if (it != std::end(ZSLUG_MAP) && it->cp == cp) {
      for (unsigned k = 0; k < it->len; k++) feed(ZSLUG_POOL[it->off + k]);
      continue;
    }
    auto mark = std::lower_bound(std::begin(ZSLUG_DROPS), std::end(ZSLUG_DROPS), cp,
      [](const ZDropRange& r, uint32_t c) { return r.last < c; });
    if (mark == std::end(ZSLUG_DROPS) || mark->first > cp) gap = !out.empty();
    // else: dropped combining mark, gap unchanged.
  }
  if (out.size() > 64) out.resize(64);
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "untitled" : out;
}

struct Slugger {
  napi_env env;
  napi_value fn, receiver;
  bool nativeOk = false;
  // Memo keyed by View, not Str: every slug input is a view into the
  // cases.json buffer (or the front matter / source body derived from it),
  // which lives for the whole runBatch, so the view stays valid. Keying by
  // Str copied the input twice per call (lookup + insert) for a string
  // that is usually unique anyway (headings).
  std::unordered_map<View, Str> memo;
  Slugger(napi_env e, napi_value f) : env(e), fn(f) {
    napiCheck(env, napi_get_undefined(env, &receiver));
    napi_value flag;
    napiCheck(env, napi_get_named_property(env, fn, "tableCompatible", &flag));
    napi_valuetype type;
    napiCheck(env, napi_typeof(env, flag, &type));
    if (type == napi_boolean) napiCheck(env, napi_get_value_bool(env, flag, &nativeOk));
  }
  Str slug(View s) {
    auto it = memo.find(s);
    if (it != memo.end()) return it->second;
    Str out;
    if (nativeOk || isAscii(s)) out = nativeSlug(s);
    else {
      napi_value arg = jsString(env, s), result;
      napiCheck(env, napi_call_function(env, receiver, fn, 1, &arg, &result));
      out = nativeString(env, result);
    }
    memo.emplace(s, out);
    return out;
  }
};

inline Str uniqueSlug(Str slug, std::unordered_set<Str>& taken) {
  Str out = slug;
  for (size_t n = 2; taken.count(out); n++) out = slug + "-" + std::to_string(n);
  taken.insert(out);
  return out;
}

inline Str longDate(View s) {
  static const char* months[] = {"January","February","March","April","May","June","July","August","September","October","November","December"};
  if (s.empty()) return {};
  int m = (s[5] - '0') * 10 + s[6] - '0', d = (s[8] - '0') * 10 + s[9] - '0';
  return std::to_string(d) + " " + months[m - 1] + " " + Str(s.substr(0, 4));
}

// Words: runs of characters other than JavaScript whitespace.
#if defined(__x86_64__)
// AVX2 word count for pure-ASCII text. A word starts where a non-whitespace
// byte follows a whitespace byte (or the start of the text). Returns
// (size_t)-1 if any byte is >= 0x80: a multi-byte UTF-8 whitespace (NBSP,
// U+2000..U+200A, U+3000, ...) would need the scalar decoder, so the caller
// falls back to it. Bodies are almost always pure ASCII, so the fallback is
// rare; the scalar loop below decodes one UTF-8 code point per character
// (~1.7 ns/byte), the AVX2 path runs at several GB/s.
__attribute__((target("avx2")))
static size_t wordCountAvx2(View s) {
  size_t n = 0;
  bool prevWs = true; // start of text counts as a boundary
  size_t i = 0;
  const char* p = s.data();
  const size_t N = s.size();
  __m256i sp = _mm256_set1_epi8(' ');
  __m256i tb = _mm256_set1_epi8('\t');
  __m256i nl = _mm256_set1_epi8('\n');
  __m256i cr = _mm256_set1_epi8('\r');
  __m256i vt = _mm256_set1_epi8('\v');
  __m256i ff = _mm256_set1_epi8('\f');
  for (; i + 32 <= N; i += 32) {
    __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p + i));
    if (_mm256_movemask_epi8(v)) return size_t(-1); // a byte >= 0x80
    __m256i ws = _mm256_or_si256(_mm256_cmpeq_epi8(v, sp), _mm256_cmpeq_epi8(v, tb));
    ws = _mm256_or_si256(ws, _mm256_cmpeq_epi8(v, nl));
    ws = _mm256_or_si256(ws, _mm256_cmpeq_epi8(v, cr));
    ws = _mm256_or_si256(ws, _mm256_cmpeq_epi8(v, vt));
    ws = _mm256_or_si256(ws, _mm256_cmpeq_epi8(v, ff));
    uint32_t m = uint32_t(_mm256_movemask_epi8(ws));
    // Word start at j: byte j is not whitespace and its predecessor is.
    // (m << 1) moves each byte's whitespace bit to its successor's slot;
    // bit 0 takes the previous chunk's last byte.
    uint32_t pred = (m << 1) | (prevWs ? 1u : 0u);
    n += unsigned(__builtin_popcount((~m) & pred));
    prevWs = (m >> 31) & 1u;
  }
  bool in = !prevWs;
  for (; i < N; i++) {
    char c = p[i];
    if ((unsigned char)c >= 0x80) return size_t(-1);
    bool w = c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
    if (w) in = false;
    else if (!in) { ++n; in = true; }
  }
  return n;
}
#endif
inline size_t wordCount(View s) {
#if defined(__x86_64__)
  if (cpu().avx2) {
    size_t n = wordCountAvx2(s);
    if (n != size_t(-1)) return n;
  }
#endif
  size_t n = 0;
  bool in = false;
  for (size_t i = 0; i < s.size();) {
    bool w = isWs(readCp(s, i));
    if (w) in = false;
    else if (!in) { ++n; in = true; }
  }
  return n;
}

} // namespace zen
