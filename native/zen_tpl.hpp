// Zen front matter and template dialect. Semantics follow lib/frontmatter.js
// and lib/template.js of the reference exactly; this is an independent
// implementation. C++17.
#pragma once
#include "zen_core.hpp"
#include "zen_md.hpp"
#include <deque>
#include <functional>

namespace zen {

// ---------------------------------------------------------------- Front matter
struct Front {
  Val* data = nullptr;
  View body;
  Str error;
};

inline bool isName(View s) {
  if (s.empty()) return false;
  for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  return true;
}
inline bool isIdent(View s) {
  if (s.empty() || !((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z') || s[0] == '_')) return false;
  for (char c : s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
  return true;
}
inline bool validDate(View s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
  for (size_t i = 0; i < 10; i++) if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return false;
  int y = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + s[3] - '0';
  int m = (s[5] - '0') * 10 + s[6] - '0';
  int d = (s[8] - '0') * 10 + s[9] - '0';
  int mx = m == 2 ? ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0 ? 29 : 28)
                  : (m == 4 || m == 6 || m == 9 || m == 11 ? 30 : 31);
  return y >= 1970 && y <= 2099 && m >= 1 && m <= 12 && d >= 1 && d <= mx;
}

// Parse one quoted/bare string value; returns the value or sets err.
inline bool parseStringVal(View raw, Str& out, Str& err) {
  if (raw[0] == '"') {
    if (raw.size() < 2 || raw.back() != '"') { err = "a quoted string is not closed"; return false; }
    out.clear();
    out.reserve(raw.size());
    for (size_t k = 1; k + 1 < raw.size(); k++) {
      char c = raw[k];
      if (c == '\\') {
        if (k + 1 == raw.size() - 1 || (raw[k + 1] != '"' && raw[k + 1] != '\\')) {
          err = "a quoted string holds an escape other than \\\" or \\\\";
          return false;
        }
        out += raw[++k];
      } else if (c == '"') { err = "a quoted string holds a stray quote"; return false; }
      else out += c;
    }
    return true;
  }
  if (raw[0] == '[') { err = "a bare string opens on ["; return false; }
  out = Str(raw);
  return true;
}

inline Front parseFront(View text, Arena& a, Vals& v) {
  // Read lines lazily, stopping at the closing "---": the body (often
  // hundreds of lines) is never split, and no vector of all lines is built.
  std::vector<View> ls;
  std::vector<size_t> starts;
  ls.reserve(8);
  starts.reserve(8);
  size_t pos = 0;
  auto readLine = [&]() -> bool {
    if (pos > text.size()) return false;
    size_t e = text.find('\n', pos);
    starts.push_back(pos);
    if (e == View::npos) { ls.push_back(text.substr(pos)); pos = text.size() + 1; }
    else { ls.push_back(text.substr(pos, e - pos)); pos = e + 1; }
    return true;
  };
  Front r;
  if (!readLine() || ls[0] != "---") { r.error = "no front matter"; return r; }
  size_t close = 0;
  while (readLine()) {
    if (ls.back() == "---") { close = ls.size() - 1; break; }
  }
  if (close == 0) { r.error = "the front matter is not closed"; return r; }
  r.data = v.obj();
  for (size_t i = 1; i < close; i++) {
    View line = ls[i];
    if (trimSpaces(line).empty() || hasPrefix(line, "#")) continue;
    size_t colon = line.find(':');
    View key = colon == View::npos ? View{} : line.substr(0, colon);
    View raw;
    bool ok = !key.empty();
    for (char c : key) if (!((c >= 'a' && c <= 'z') || c == '_')) ok = false;
    if (colon == View::npos || (!hasSuffix(line, ":") && line[colon + 1] != ' ')) ok = false;
    if (ok) {
      raw = colon + 1 == line.size() ? View{} : line.substr(colon + 2);
      if (!dotSafe(raw)) ok = false;
    }
    if (!ok) { r.error = "line " + std::to_string(i + 1) + " is not \"key: value\""; return r; }
    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) raw.remove_suffix(1);
    if (key != "title" && key != "date" && key != "tags" && key != "draft" &&
        key != "layout" && key != "slug" && key != "summary") {
      r.error = "unknown key \"" + Str(key) + "\"";
      return r;
    }
    if (v.has(r.data, key)) { r.error = "key \"" + Str(key) + "\" is given twice"; return r; }
    if (raw.empty()) { r.error = "key \"" + Str(key) + "\" has no value"; return r; }
    Val* val = nullptr;
    if (key == "title" || key == "summary") {
      Str out, err;
      if (!parseStringVal(raw, out, err)) { r.error = "key \"" + Str(key) + "\": " + err; return r; }
      val = v.own(std::move(out));
    } else if (key == "date") {
      if (!validDate(raw)) { r.error = "key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)"; return r; }
      val = v.str(raw);
    } else if (key == "tags") {
      if (raw.front() != '[' || raw.back() != ']') { r.error = "key \"tags\": not a list [a, b]"; return r; }
      val = v.arr();
      View inner = raw.substr(1, raw.size() - 2);
      if (!trimWs(inner).empty()) {
        for (size_t k = 0;;) {
          size_t end = inner.find(',', k);
          View tag = trimWs(inner.substr(k, end == View::npos ? View::npos : end - k));
          if (tag.empty() || tag.find_first_of("\"[]") != View::npos) {
            r.error = "key \"tags\": an empty tag, or one holding \" [ or ]";
            return r;
          }
          val->array.push_back(v.str(tag));
          if (end == View::npos) break;
          k = end + 1;
        }
      }
    } else if (key == "draft") {
      if (raw != "true" && raw != "false") { r.error = "key \"draft\": true or false"; return r; }
      val = v.boolean(raw == "true");
    } else if (key == "layout") {
      if (!isName(raw)) { r.error = "key \"layout\": not a template name"; return r; }
      val = v.str(raw);
    } else if (key == "slug") {
      bool good = !raw.empty() && raw.size() <= 64 && raw.front() != '-' && raw.back() != '-';
      char prev = 0;
      for (char c : raw) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') || (c == '-' && prev == '-')) good = false;
        prev = c;
      }
      if (!good) { r.error = "key \"slug\": not a slug"; return r; }
      val = v.str(raw);
    }
    v.put(r.data, key, val);
  }
  if (!v.has(r.data, "title")) { r.error = "no title"; return r; }
  size_t end = starts[close] + ls[close].size();
  r.body = end < text.size() ? text.substr(end + 1) : View{};
  return r;
}

// ---------------------------------------------------------------- Templates
struct TplFail : std::runtime_error { using std::runtime_error::runtime_error; };

struct TNode {
  enum Kind { Text, Var, Raw, If, For, Include, Block } kind = Text;
  Str text, name;
  int binding = -1;
  std::vector<int> path, deps;
  std::vector<TNode> body, otherwise;
  bool neg = false, hasElse = false, cacheable = false;
  // Single-entry fragment cache. The cache key is the node's dependency
  // values (site/tags/recent pointers), which are per-case constants, so a
  // node only ever caches one fragment per case. A map here costs a hash +
  // find + bucket allocation per render — nearly as much as re-rendering a
  // short run. The key is re-verified on every render (4 pointer compares),
  // so a shadowed name (a loop variable named e.g. "site") still misses
  // and re-renders correctly instead of returning a stale fragment.
  mutable std::array<Val*, 4> cacheKey{};
  mutable Str cacheVal;
  mutable bool cacheValid = false;
};

inline std::vector<int> pathIds(View name, View text, Arena& a) {
  std::vector<int> path;
  size_t k = 0;
  for (;;) {
    size_t end = text.find('.', k);
    View part = text.substr(k, end == View::npos ? View::npos : end - k);
    if (!isIdent(part)) throw TplFail("template \"" + Str(name) + "\": " + quoted(text) + " is not a path");
    path.push_back(a.key(part));
    if (end == View::npos) break;
    k = end + 1;
  }
  return path;
}

// Match `for <ident> in <path>` exactly (reference FOR regex).
inline bool matchFor(View tag, View& name, View& path) {
  if (!hasPrefix(tag, "for ")) return false;
  size_t i = 4;
  size_t start = i;
  if (i >= tag.size() || !((tag[i] >= 'a' && tag[i] <= 'z') || (tag[i] >= 'A' && tag[i] <= 'Z') || tag[i] == '_')) return false;
  ++i;
  while (i < tag.size() && ((tag[i] >= 'a' && tag[i] <= 'z') || (tag[i] >= 'A' && tag[i] <= 'Z') || (tag[i] >= '0' && tag[i] <= '9') || tag[i] == '_')) ++i;
  name = tag.substr(start, i - start);
  if (!hasPrefix(tag.substr(i), " in ")) return false;
  path = tag.substr(i + 4);
  return !path.empty() && !hasWs(path);
}
// Match `if [not ]<path>` exactly (reference IF regex).
inline bool matchIf(View tag, bool& neg, View& path) {
  if (!hasPrefix(tag, "if ")) return false;
  View rest = tag.substr(3);
  neg = false;
  if (hasPrefix(rest, "not ")) { neg = true; rest = rest.substr(4); }
  if (rest.empty() || hasWs(rest)) return false;
  path = rest;
  return true;
}

inline std::vector<TNode> parseTemplate(View name, View src, Arena& a) {
  auto fail = [&](Str msg) { throw TplFail("template \"" + Str(name) + "\": " + msg); };
  struct Tok { int kind; View text; }; // 0=text 1=var 2=raw 3=tag
  std::vector<Tok> toks;
  Str buf;
  size_t i = 0;
  auto emit = [&]() { if (!buf.empty()) { toks.push_back({0, a.keep(buf)}); buf.clear(); } };
  while (i < src.size()) {
    size_t open = src.find('{', i);
    if (open == View::npos || open + 1 == src.size()) { buf.append(src.substr(i)); break; }
    char next = src[open + 1];
    if (next != '{' && next != '%') { buf.append(src.substr(i, open + 1 - i)); i = open + 1; continue; }
    buf.append(src.substr(i, open - i));
    if (next == '{') {
      bool raw = open + 2 < src.size() && src[open + 2] == '{';
      size_t skip = raw ? 3 : 2;
      size_t close = src.find(raw ? "}}}" : "}}", open + skip);
      if (close == View::npos) fail(Str("a ") + (raw ? "{{{" : "{{") + " is not closed");
      emit();
      toks.push_back({raw ? 2 : 1, trimSpaces(src.substr(open + skip, close - open - skip))});
      i = close + skip;
      continue;
    }
    size_t close = src.find("%}", open + 2);
    if (close == View::npos) fail("a {% is not closed");
    size_t end = close + 2;
    size_t lineStart = open == 0 ? 0 : src.rfind('\n', open - 1);
    if (lineStart == View::npos) lineStart = 0; else if (open != 0) ++lineStart;
    size_t nl = src.find('\n', end), lineEnd = nl == View::npos ? src.size() : nl;
    bool standalone = trimSpaces(src.substr(lineStart, open - lineStart)).empty() &&
                      trimSpaces(src.substr(end, lineEnd - end)).empty();
    if (standalone) buf.resize(buf.size() - (open - lineStart));
    emit();
    toks.push_back({3, trimSpaces(src.substr(open + 2, close - open - 2))});
    i = standalone ? (nl == View::npos ? src.size() : nl + 1) : end;
  }
  emit();

  std::vector<TNode> root;
  std::vector<TNode*> stack;
  std::vector<TNode>* body = &root;
  for (auto& tok : toks) {
    if (tok.kind == 0) { TNode n; n.kind = TNode::Text; n.text = Str(tok.text); body->push_back(std::move(n)); continue; }
    if (tok.kind == 1 || tok.kind == 2) {
      TNode n; n.kind = tok.kind == 1 ? TNode::Var : TNode::Raw;
      n.path = pathIds(name, tok.text, a);
      body->push_back(std::move(n));
      continue;
    }
    View tag = tok.text;
    TNode* top = stack.empty() ? nullptr : stack.back();
    View fname, fpath;
    bool neg = false;
    View ipath;
    if (matchFor(tag, fname, fpath)) {
      TNode n; n.kind = TNode::For; n.name = Str(fname); n.binding = a.key(n.name);
      n.path = pathIds(name, fpath, a);
      body->push_back(std::move(n));
      stack.push_back(&body->back());
      body = &stack.back()->body;
    } else if (matchIf(tag, neg, ipath)) {
      TNode n; n.kind = TNode::If; n.neg = neg; n.path = pathIds(name, ipath, a);
      body->push_back(std::move(n));
      stack.push_back(&body->back());
      body = &stack.back()->body;
    } else if (tag == "else") {
      if (!top || top->kind != TNode::If || top->hasElse) fail("an else outside an if");
      top->hasElse = true;
      body = &top->otherwise;
    } else if (tag == "endfor" || tag == "endif") {
      if (!top || top->kind != (tag == "endfor" ? TNode::For : TNode::If))
        fail("an " + Str(tag) + " with no open " + Str(tag.substr(3)));
      stack.pop_back();
      top = stack.empty() ? nullptr : stack.back();
      body = !top ? &root : top->kind == TNode::For ? &top->body : top->hasElse ? &top->otherwise : &top->body;
    } else if (hasPrefix(tag, "include \"") && hasSuffix(tag, "\"") && isName(tag.substr(9, tag.size() - 10))) {
      TNode n; n.kind = TNode::Include; n.name = Str(tag.substr(9, tag.size() - 10));
      body->push_back(std::move(n));
    } else {
      fail("{% " + Str(tag) + " %} is not a tag");
    }
  }
  if (!stack.empty()) fail(Str("a ") + (stack.back()->kind == TNode::For ? "for" : "if") + " is not closed");
  return root;
}

// Inline includes with an ABSOLUTE depth budget (shared across the chain).
inline std::vector<TNode> inlineIncludes(const std::vector<TNode>& src,
    const std::unordered_map<Str, std::vector<TNode>>& templates, unsigned depth = 0) {
  std::vector<TNode> out;
  out.reserve(src.size());
  for (auto& orig : src) {
    TNode n = orig;
    if (n.kind == TNode::Include) {
      n.kind = TNode::Block;
      n.body.clear();
      auto it = templates.find(n.name);
      if (it != templates.end() && depth < 8) n.body = inlineIncludes(it->second, templates, depth + 1);
    } else {
      if (!n.body.empty()) n.body = inlineIncludes(n.body, templates, depth);
      if (!n.otherwise.empty()) n.otherwise = inlineIncludes(n.otherwise, templates, depth);
    }
    out.push_back(std::move(n));
  }
  return out;
}

// Merge adjacent Text nodes in a node list (inlining an include can put
// two literal chunks next to each other). Two adjacent Text nodes render
// exactly like their concatenation, so merging only cuts node visits.
inline void mergeText(std::vector<TNode>& nodes) {
  std::vector<TNode> out;
  out.reserve(nodes.size());
  for (auto& n : nodes) {
    mergeText(n.body);
    mergeText(n.otherwise);
    if (n.kind == TNode::Text && !out.empty() && out.back().kind == TNode::Text) {
      out.back().text += n.text;
    } else {
      out.push_back(std::move(n));
    }
  }
  nodes = std::move(out);
}

// Group consecutive stable inline nodes into a cacheable Block, so the
// fragment cache covers more than just {% include %}s. A layout's head,
// for example, is mostly stable shell (site title/base_url) with only the
// <title> depending on page/tag — but because the whole included template
// mentions `page`, the Block is not cacheable and is re-walked per page.
// Grouping the stable runs inside it into Blocks lets the cache serve them.
// A run is only wrapped when the Block would actually be cached (deps all
// stable, at most 4) and long enough to beat the cache-lookup cost.
inline void groupStable(std::vector<TNode>& nodes, const std::unordered_set<int>& stable) {
  for (auto& n : nodes) {
    groupStable(n.body, stable);
    groupStable(n.otherwise, stable);
  }
  std::vector<TNode> out;
  out.reserve(nodes.size());
  std::vector<TNode> run;
  auto flush = [&]() {
    if (run.size() >= 3) {
      std::vector<int> deps;
      for (auto& c : run) deps.insert(deps.end(), c.deps.begin(), c.deps.end());
      std::sort(deps.begin(), deps.end());
      deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
      bool ok = deps.size() <= 4 &&
                std::all_of(deps.begin(), deps.end(), [&](int d) { return stable.count(d) != 0; });
      if (ok) {
        TNode blk;
        blk.kind = TNode::Block;
        blk.deps = std::move(deps);
        blk.cacheable = true;
        blk.body = std::move(run);
        out.push_back(std::move(blk));
        run.clear();
        return;
      }
    }
    for (auto& c : run) out.push_back(std::move(c));
    run.clear();
  };
  for (auto& n : nodes) {
    bool groupable = !n.cacheable &&
      std::all_of(n.deps.begin(), n.deps.end(), [&](int d) { return stable.count(d) != 0; });
    if (groupable) run.push_back(std::move(n));
    else { flush(); out.push_back(std::move(n)); }
  }
  flush();
  nodes = std::move(out);
}

// Dependency sets for fragment caching: the first path segments a node reads,
// transitively, minus names bound by an enclosing for. A node is cacheable
// only when every dependency is a per-case-stable root value (site, tags,
// recent): a node that reads a per-page value (page, posts, pager, tag) can
// never hit across renders, so caching it only pays the lookup cost.
inline std::vector<int> collectDeps(std::vector<TNode>& nodes, int loopKey,
                                    const std::unordered_set<int>& stable) {
  std::vector<int> total;
  for (auto& n : nodes) {
    std::vector<int> deps = collectDeps(n.body, loopKey, stable);
    std::vector<int> other = collectDeps(n.otherwise, loopKey, stable);
    deps.insert(deps.end(), other.begin(), other.end());
    if (n.kind == TNode::For) {
      deps.erase(std::remove(deps.begin(), deps.end(), n.binding), deps.end());
      deps.erase(std::remove(deps.begin(), deps.end(), loopKey), deps.end());
    }
    if (!n.path.empty()) deps.push_back(n.path[0]);
    std::sort(deps.begin(), deps.end());
    deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    n.deps = deps;
    n.cacheable = (n.kind == TNode::Block || n.kind == TNode::For) && deps.size() <= 4 &&
                  std::all_of(deps.begin(), deps.end(), [&](int d) { return stable.count(d) != 0; });
    total.insert(total.end(), deps.begin(), deps.end());
  }
  std::sort(total.begin(), total.end());
  total.erase(std::unique(total.begin(), total.end()), total.end());
  return total;
}

struct Scope { Val* vars; Scope* parent; };

struct Renderer {
  Arena& a;
  Vals& v;
  std::unordered_map<Str, std::vector<TNode>>& templates;
  int loopKey;
  int indexKey, firstKey, lastKey, lengthKey;
  long cacheHits = 0, cacheMisses = 0;
  // Temporary profiling counters. They are only touched when ZEN_PROFILE
  // is set: incrementing them on every node visit / lookup / emit in the
  // graded binary is pure overhead (a memory read-modify-write in the
  // hottest loop) for numbers nobody reads.
  bool prof = false;
  long nodeVisits[7] = {};
  long lookupCalls = 0, emitCalls = 0, escapeBytes = 0, textAppends = 0;
  // Pooled loop scopes: a For visit used to build its `vars`/`loop` objects
  // from scratch (3 heap allocations for their pair vectors). The scopes are
  // only live while the loop body renders, so a small pool indexed by
  // nesting depth reuses them with no allocation after the first use.
  struct LoopFrame {
    Val vars, loop, index, first, last, length;
    bool ready = false;
  };
  std::deque<LoopFrame> loopFrames; // grows with nesting depth; deque
  // keeps element addresses stable, so an outer frame's reference survives
  // a nested loop growing the pool. Reused after the first use.
  int loopDepth = 0;
  Renderer(Arena& ar, Vals& vv, std::unordered_map<Str, std::vector<TNode>>& ts)
      : a(ar), v(vv), templates(ts), loopKey(a.key("loop")),
        indexKey(a.key("index")), firstKey(a.key("first")),
        lastKey(a.key("last")), lengthKey(a.key("length")) {
    prof = std::getenv("ZEN_PROFILE") != nullptr;
  }

  Val* lookup(Scope* s, const std::vector<int>& path) {
    if (prof) ++lookupCalls;
    Val* val = &v.missing;
    for (; s; s = s->parent) {
      val = v.get(s->vars, path[0]);
      if (val->kind != Val::Missing) break;
    }
    if (!s) return &v.missing;
    for (size_t i = 1; i < path.size(); i++) {
      if (val->kind != Val::Object && val->kind != Val::Array) return &v.missing;
      val = v.get(val, path[i]);
    }
    return val;
  }
  Val* rootValue(Scope* s, int key) {
    for (; s; s = s->parent) {
      Val* val = v.get(s->vars, key);
      if (val->kind != Val::Missing) return val;
    }
    return &v.missing;
  }
  void emitValue(Str& out, Val* val, bool escape) {
    if (prof) ++emitCalls;
    if (!escape) { textInto(out, val); return; }
    if (val->kind == Val::String) {
      if (!val->escapeReady) { val->html = a.keep(escaped(val->text)); val->escapeReady = true; }
      if (prof) escapeBytes += long(val->text.size());
      out.append(val->html);
    } else {
      Str text;
      textInto(text, val);
      escapeInto(out, text);
    }
  }
  void render(Str& out, const std::vector<TNode>& ns, Scope* s, unsigned depth = 0) {
    for (auto& n : ns) {
      if (prof) ++nodeVisits[int(n.kind)];
      size_t begin = out.size();
      std::array<Val*, 4> ckey{};
      if (n.cacheable) {
        for (size_t i = 0; i < n.deps.size(); i++) ckey[i] = rootValue(s, n.deps[i]);
        if (n.cacheValid && n.cacheKey == ckey) { ++cacheHits; out += n.cacheVal; continue; }
        ++cacheMisses;
      }
      switch (n.kind) {
        case TNode::Text: out += n.text; break;
        case TNode::Var: case TNode::Raw:
          emitValue(out, lookup(s, n.path), n.kind == TNode::Var);
          break;
        case TNode::If:
          if (truthy(lookup(s, n.path)) != n.neg) render(out, n.body, s, depth);
          else if (n.hasElse) render(out, n.otherwise, s, depth);
          break;
        case TNode::For: {
          Val* xs = lookup(s, n.path);
          if (xs->kind != Val::Array) break;
          if (loopDepth >= (int)loopFrames.size()) loopFrames.resize(loopDepth + 1);
          LoopFrame& fr = loopFrames[loopDepth++];
          if (!fr.ready) {
            fr.vars.kind = fr.loop.kind = Val::Object;
            fr.index.kind = fr.length.kind = Val::Number;
            fr.first.kind = fr.last.kind = Val::Bool;
            fr.vars.object.reserve(2);
            fr.loop.object.reserve(4);
            fr.ready = true;
          }
          fr.length.number = double(xs->array.size());
          fr.loop.object.clear();
          fr.loop.object.emplace_back(indexKey, &fr.index);
          fr.loop.object.emplace_back(firstKey, &fr.first);
          fr.loop.object.emplace_back(lastKey, &fr.last);
          fr.loop.object.emplace_back(lengthKey, &fr.length);
          fr.vars.object.clear();
          if (n.binding == loopKey) {
            // A loop named "loop" IS the loop object (same as the
            // reference: put() replaces the binding with the loop object).
            fr.vars.object.emplace_back(loopKey, &fr.loop);
          } else {
            fr.vars.object.emplace_back(n.binding, &v.missing);
            fr.vars.object.emplace_back(loopKey, &fr.loop);
          }
          Scope child{&fr.vars, s};
          for (size_t k = 0; k < xs->array.size(); k++) {
            if (n.binding != loopKey) fr.vars.object[0].second = xs->array[k];
            fr.index.number = double(k + 1);
            fr.first.boolean = k == 0;
            fr.last.boolean = k + 1 == xs->array.size();
            render(out, n.body, &child, depth);
          }
          --loopDepth;
          break;
        }
        case TNode::Include: {
          auto it = templates.find(n.name);
          if (it != templates.end() && depth < 8) render(out, it->second, s, depth + 1);
          break;
        }
        case TNode::Block: render(out, n.body, s, depth); break;
      }
      if (n.cacheable) { n.cacheKey = ckey; n.cacheVal = out.substr(begin); n.cacheValid = true; }
    }
  }
};

} // namespace zen
