// Zen Markdown: the makefun-site subset, single-pass. Semantics follow
// lib/markdown.js of the reference exactly; this is an independent
// implementation written for speed (no token allocation, SIMD inline scan).
#pragma once
#include "zen_core.hpp"

namespace zen {

inline bool isBlank(View s) {
  for (char c : s) if (c != ' ' && c != '\t') return false;
  return true;
}
inline bool fenceOpen(View s, View& lang) {
  if (!hasPrefix(s, "```")) return false;
  size_t k = 3;
  while (k < s.size()) {
    char c = s[k];
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '+' || c == '-') ++k;
    else break;
  }
  lang = s.substr(3, k - 3);
  return isBlank(s.substr(k));
}
inline bool fenceClose(View s) { return hasPrefix(s, "```") && isBlank(s.substr(3)); }
// Heading level (0 = not a heading) and its text. A line whose text holds a
// line terminator is not a heading (the reference regex `.` cannot cross it).
inline int headingLevel(View s, View& text) {
  size_t k = 0;
  while (k < s.size() && s[k] == '#') ++k;
  if (k < 1 || k > 6 || k == s.size() || (s[k] != ' ' && s[k] != '\t')) return 0;
  int level = int(k);
  while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
  text = s.substr(k);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
  if (text.empty() || !dotSafe(text)) return 0;
  return level;
}
inline bool isHr(View s) { return (hasPrefix(s, "---") || hasPrefix(s, "***")) && isBlank(s.substr(3)); }
inline bool endsPara(View s) {
  View dummy;
  return isBlank(s) || fenceOpen(s, dummy) || headingLevel(s, dummy) || isHr(s);
}
// A list item marker: bullet (- or * + space) or ordered (digits + ". ").
inline bool listMarker(View s, bool ordered, View& item, int& start) {
  if (!ordered) {
    if (s.size() < 2 || (s[0] != '-' && s[0] != '*') || s[1] != ' ') return false;
    item = s.substr(2);
    return dotSafe(item);
  }
  size_t k = 0;
  while (k < s.size() && s[k] >= '0' && s[k] <= '9') ++k;
  if (k < 1 || k > 9 || k + 1 >= s.size() || s[k] != '.' || s[k + 1] != ' ') return false;
  start = 0;
  for (size_t i = 0; i < k; i++) start = start * 10 + s[i] - '0';
  item = s.substr(k + 2);
  return dotSafe(item);
}
// A continuation line: two or more spaces then a non-whitespace character.
inline bool isContinued(View s) {
  size_t k = 0;
  while (k < s.size() && s[k] == ' ') ++k;
  if (k < 2 || k == s.size()) return false;
  return !isWs(readCp(s, k));
}
inline bool escapable(char c) {
  return c == '\\' || c == '`' || c == '*' || c == '_' || c == '[' || c == ']' ||
    c == '(' || c == ')' || c == '#' || c == '+' || c == '-' || c == '.' || c == '!' || c == '>';
}
// First index >= from where `tok` occurs outside a backslash escape, or npos.
// NOTE: an AVX2 version of this (and of findLoneStar) was tried and reverted:
// the lone-star pairing must not count an escaped `*` toward the run, and
// getting that parity right under SIMD chunking proved too error-prone for
// the small win. The scalar loop is the reference behaviour, kept verbatim.
inline size_t findUnescaped(View s, View tok, size_t from) {
  for (size_t i = from; i + tok.size() <= s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size() && escapable(s[i + 1])) { ++i; continue; }
    if (s.substr(i, tok.size()) == tok) return i;
  }
  return View::npos;
}
// First lone `*` (>= from), one not beside another `*`, outside an escape.
inline size_t findLoneStar(View s, size_t from) {
  for (size_t i = from; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size() && escapable(s[i + 1])) { ++i; continue; }
    if (s[i] != '*') continue;
    if (i + 1 < s.size() && s[i + 1] == '*') { ++i; continue; }
    return i;
  }
  return View::npos;
}

struct LinkEnv {
  View base, source, dir;
  const std::unordered_map<Str, Str>& sourceUrls;
  const std::unordered_set<Str>& staticPaths;
  std::unordered_set<Str>& broken;
  Str resolve(View target) {
    bool scheme = false;
    if (!target.empty() && ((target[0] >= 'a' && target[0] <= 'z') || (target[0] >= 'A' && target[0] <= 'Z'))) {
      size_t i = 1;
      while (i < target.size() && ((target[i] >= 'a' && target[i] <= 'z') || (target[i] >= 'A' && target[i] <= 'Z') ||
             (target[i] >= '0' && target[i] <= '9') || target[i] == '+' || target[i] == '.' || target[i] == '-')) ++i;
      scheme = i < target.size() && target[i] == ':';
    }
    if (scheme || hasPrefix(target, "/") || hasPrefix(target, "#")) return Str(target);
    size_t hash = target.find('#');
    View rel = target.substr(0, hash), frag = hash == View::npos ? View{} : target.substr(hash);
    std::vector<View> parts;
    if (!dir.empty()) {
      for (size_t i = 0;;) {
        size_t e = dir.find('/', i);
        parts.push_back(dir.substr(i, e == View::npos ? View::npos : e - i));
        if (e == View::npos) break;
        i = e + 1;
      }
    }
    bool ok = !rel.empty();
    for (size_t i = 0; ok;) {
      size_t e = rel.find('/', i);
      View seg = rel.substr(i, e == View::npos ? View::npos : e - i);
      if (seg == "..") { if (parts.empty()) ok = false; else parts.pop_back(); }
      else if (!seg.empty() && seg != ".") parts.push_back(seg);
      if (e == View::npos) break;
      i = e + 1;
    }
    if (ok) {
      Str path;
      for (auto part : parts) { if (!path.empty()) path += '/'; path.append(part); }
      auto it = sourceUrls.find(path);
      if (it != sourceUrls.end()) return it->second + Str(frag);
      if (staticPaths.count(path)) return Str(base) + "/" + path + Str(frag);
    }
    broken.insert("broken-link " + Str(source) + " -> " + Str(target));
    return Str(target);
  }
};

inline void renderInline(Str& out, View s, LinkEnv& env, unsigned depth = 0) {
  if (depth > 1024) throw std::runtime_error("Markdown nesting limit exceeded");
  for (size_t i = 0; i < s.size();) {
    size_t next = scanSpecial(s, i, true);
    out.append(s.substr(i, next - i));
    i = next;
    if (i == s.size()) break;
    char c = s[i];
    if (c == '\\' && i + 1 < s.size() && escapable(s[i + 1])) { escapeChar(out, s[i + 1]); i += 2; continue; }
    if (c == '`') {
      size_t end = s.find('`', i + 1);
      if (end != View::npos && end > i + 1) {
        out += "<code>";
        escapeInto(out, s.substr(i + 1, end - i - 1));
        out += "</code>";
        i = end + 1;
      } else { out += '`'; ++i; }
      continue;
    }
    if (c == '*') {
      if (i + 1 < s.size() && s[i + 1] == '*') {
        size_t end = findUnescaped(s, "**", i + 2);
        if (end != View::npos && end > i + 2) {
          out += "<strong>";
          View inner = s.substr(i + 2, end - i - 2);
          // Fast path: inner text with no inline specials needs no
          // recursion — it is literal text. (scanSpecial's set is exactly
          // the characters renderInline would act on, so "no specials"
          // means nothing to escape or nest.) This skips a function call
          // plus a whole scan loop for the common plain-word case.
          if (scanSpecial(inner, 0, true) == inner.size()) out.append(inner);
          else renderInline(out, inner, env, depth + 1);
          out += "</strong>";
          i = end + 2;
        } else { out += "**"; i += 2; }
        continue;
      }
      size_t end = findLoneStar(s, i + 1);
      if (end != View::npos && end > i + 1) {
        out += "<em>";
        View inner = s.substr(i + 1, end - i - 1);
        if (scanSpecial(inner, 0, true) == inner.size()) out.append(inner);
        else renderInline(out, inner, env, depth + 1);
        out += "</em>";
        i = end + 1;
      } else { out += '*'; ++i; }
      continue;
    }
    if (c == '[') {
      size_t k = findUnescaped(s, "]", i + 1);
      size_t m = k != View::npos && k + 1 < s.size() && s[k + 1] == '(' ? s.find(')', k + 2) : View::npos;
      View target = m == View::npos ? View{} : s.substr(k + 2, m - k - 2);
      if (!target.empty() && !hasWs(target)) {
        out += "<a href=\"";
        Str href = env.resolve(target);
        escapeInto(out, href);
        out += "\">";
        View inner = s.substr(i + 1, k - i - 1);
        if (scanSpecial(inner, 0, true) == inner.size()) out.append(inner);
        else renderInline(out, inner, env, depth + 1);
        out += "</a>";
        i = m + 1;
        continue;
      }
    }
    escapeChar(out, c);
    ++i;
  }
}

struct MdResult {
  Str html;
  Str excerpt;
  Val* toc = nullptr;
};

inline MdResult renderMarkdown(View src, LinkEnv& env, Arena& a, Vals& v, Slugger& slugs) {
  auto lines = splitLines(src);
  MdResult result;
  result.html.reserve(src.size() * 2);
  result.toc = v.arr();
  bool gotExcerpt = false;
  std::unordered_set<Str> ids;
  // Scratch buffers, hoisted out of the block loop and reused. The block
  // pass was allocation-bound: a fresh std::string per paragraph (~16k of
  // them), per heading html, and per list item, plus ~8 temporaries for
  // every "<hN ...>...</hN>" concatenation chain. Reusing the buffers
  // keeps their capacity, so the per-block cost is a clear() plus the
  // bytes themselves — no malloc/free per block. All of these hold only
  // views or copies of the current block, so reuse is safe.
  Str paraBody, quoteBody, headHtml;
  struct ListItem { View view; Str owned; bool multi = false; };
  std::vector<ListItem> items;
  size_t i = 0;
  while (i < lines.size()) {
    View line = lines[i], lang, text, item;
    int start = 1;
    if (isBlank(line)) { ++i; continue; }
    if (fenceOpen(line, lang)) {
      result.html += "<pre><code";
      if (!lang.empty()) { result.html += " class=\"language-"; escapeInto(result.html, lang); result.html += '"'; }
      result.html += '>';
      ++i;
      while (i < lines.size() && !fenceClose(lines[i])) { escapeInto(result.html, lines[i++]); result.html += '\n'; }
      ++i;
      result.html += "</code></pre>\n";
      continue;
    }
    int level = headingLevel(line, text);
    if (level) {
      Str id = uniqueSlug(slugs.slug(text), ids);
      headHtml.clear();
      renderInline(headHtml, text, env);
      // Appended piece by piece: the old "<h" + to_string(level) + ...
      // chain built ~8 temporary strings per heading. Same bytes out.
      result.html += "<h";
      result.html += char('0' + level);
      result.html += " id=\"";
      result.html += id;
      result.html += "\">";
      result.html += headHtml;
      result.html += "</h";
      result.html += char('0' + level);
      result.html += ">\n";
      if (level == 2 || level == 3) {
        Val* h = v.obj();
        v.put(h, "level", v.num(level));
        v.put(h, "id", v.own(std::move(id)));
        v.put(h, "html", v.own(View(headHtml)));
        result.toc->array.push_back(h);
      }
      ++i;
      continue;
    }
    if (isHr(line)) { result.html += "<hr>\n"; ++i; continue; }
    if (hasPrefix(line, ">")) {
      quoteBody.clear();
      while (i < lines.size() && hasPrefix(lines[i], ">")) {
        View w = lines[i++].substr(1);
        if (hasPrefix(w, " ")) w.remove_prefix(1);
        w = trimSpaces(w);
        if (!w.empty()) { if (!quoteBody.empty()) quoteBody += '\n'; quoteBody.append(w); }
      }
      result.html += "<blockquote>\n";
      if (!quoteBody.empty()) { result.html += "<p>"; renderInline(result.html, quoteBody, env); result.html += "</p>\n"; }
      result.html += "</blockquote>\n";
      continue;
    }
    bool bullet = listMarker(line, false, item, start);
    bool ordered = !bullet && listMarker(line, true, item, start);
    if (bullet || ordered) {
      const char* tag = ordered ? "ol" : "ul";
      // Items are views into the source (no copy) unless a continuation
      // line joins onto one — only then is that item materialised.
      items.clear();
      for (;;) {
        int dummy = 0;
        if (i == lines.size()) break;
        if (listMarker(lines[i], ordered, item, dummy)) {
          items.push_back({trimSpaces(item), {}, false});
          ++i;
        } else if (!items.empty() && isContinued(lines[i])) {
          ListItem& back = items.back();
          if (!back.multi) { back.owned = Str(back.view); back.multi = true; }
          back.owned += '\n';
          back.owned.append(trimSpaces(lines[i++]));
        } else break;
      }
      result.html += '<';
      result.html += tag;
      if (ordered && start != 1) result.html += " start=\"" + std::to_string(start) + "\"";
      result.html += ">\n";
      for (auto& it : items) {
        result.html += "<li>";
        renderInline(result.html, it.multi ? View(it.owned) : it.view, env);
        result.html += "</li>\n";
      }
      result.html += "</";
      result.html += tag;
      result.html += ">\n";
      continue;
    }
    paraBody.clear();
    paraBody.append(trimSpaces(line));
    ++i;
    while (i < lines.size() && !endsPara(lines[i])) { paraBody += '\n'; paraBody.append(trimSpaces(lines[i++])); }
    result.html += "<p>";
    size_t begin = result.html.size();
    renderInline(result.html, paraBody, env);
    if (!gotExcerpt) { result.excerpt = result.html.substr(begin); gotExcerpt = true; }
    result.html += "</p>\n";
  }
  return result;
}

} // namespace zen
