// Zen site pipeline: the makefun-site 1.0.0 build, following vendor/index.js
// exactly. Independent implementation, C++17.
#pragma once
#include "zen_core.hpp"
#include "zen_md.hpp"
#include "zen_tpl.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>

namespace zen {

inline Str stripTrailingSlashes(View s) {
  while (!s.empty() && s.back() == '/') s.remove_suffix(1);
  return Str(s);
}
inline View dirOf(View path) {
  size_t i = path.rfind('/');
  return i == View::npos ? View{} : path.substr(0, i);
}
inline bool isMd(View path) { return hasSuffix(path, ".md"); }

struct Source;
struct Tag {
  Str name, slug, url;
  std::vector<Source*> posts;
};
struct Source {
  Str path;
  View text; // source text: a view into the cases.json buffer (LF sources)
             // or into the arena (CRLF-normalised); never a copy of its own.
             // The buffer lives for the whole runBatch and arena pointers
             // never move, so the view stays valid for the case.
  Val* data = nullptr;  // front-matter values
  View body;
  bool post = false;
  Str layout;
  Str slug, outDir, url;
  Str html, excerpt;
  Val* toc = nullptr;
  std::vector<Tag*> tagRefs;
  Source* newer = nullptr;
  Source* older = nullptr;
  Val* card = nullptr;
  Val* link = nullptr;   // cached linkOf(this); built twice per page otherwise
};

struct Site {
  Arena& a;
  Vals& v;
  Slugger& slugs;
  std::vector<Str> report;
  std::unordered_set<Str> broken;
  std::vector<Source> sources;
  std::vector<std::pair<Str, Str>> statics; // (path, content), sorted by path
  std::unordered_map<Str, Str> sourceUrls;
  std::unordered_set<Str> staticPaths;
  std::vector<Source*> posts;
  std::deque<Tag> tagStore;      // owns every Tag; pointers into a deque stay put
  std::vector<Tag*> tags;        // b.tags: post tags only, count desc then slug
  std::unordered_map<Str, std::vector<TNode>> templates; // inlined, by name
  Output output;

  Site(Arena& ar, Vals& vv, Slugger& ss) : a(ar), v(vv), slugs(ss) {}

  Str resolveLink(View dir, View rel) {
    std::vector<View> parts;
    if (!dir.empty()) {
      for (size_t i = 0;;) {
        size_t e = dir.find('/', i);
        parts.push_back(dir.substr(i, e == View::npos ? View::npos : e - i));
        if (e == View::npos) break;
        i = e + 1;
      }
    }
    for (size_t i = 0;;) {
      size_t e = rel.find('/', i);
      View seg = rel.substr(i, e == View::npos ? View::npos : e - i);
      if (seg == "..") {
        if (parts.empty()) return {};
        parts.pop_back();
      } else if (!seg.empty() && seg != ".") parts.push_back(seg);
      if (e == View::npos) break;
      i = e + 1;
    }
    Str out;
    for (auto p : parts) { if (!out.empty()) out += '/'; out.append(p); }
    return out;
  }

  Val* linkOf(Source* s) {
    if (!s) return v.make(Val::Null);
    if (s->link) return s->link;
    Val* o = v.obj();
    o->object.reserve(4);
    Val* date = v.get(s->data, "date");
    v.put(o, "title", v.get(s->data, "title"));
    // The url lives in the Source for the whole case, so a view is enough:
    // own() would copy it into the arena (and, for an lvalue, once more
    // into the by-value parameter first).
    v.put(o, "url", v.str(s->url));
    v.put(o, "date", date->kind == Val::Missing ? v.str("") : date);
    v.put(o, "date_long", date->kind == Val::Missing ? v.str("") : v.own(longDate(date->text)));
    s->link = o;
    return o;
  }
  Val* cardOf(Source* s) {
    Val* o = v.obj();
    // 8 card fields, plus 6 more when pageOf() reuses this object as a
    // page (slug, content, toc, newer, older, is_post): reserve for all 14
    // so the page path never reallocates.
    o->object.reserve(14);
    Val* date = v.get(s->data, "date");
    v.put(o, "title", v.get(s->data, "title"));
    v.put(o, "url", v.str(s->url));
    v.put(o, "date", date->kind == Val::Missing ? v.str("") : date);
    v.put(o, "date_long", date->kind == Val::Missing ? v.str("") : v.own(longDate(date->text)));
    Val* summary = v.get(s->data, "summary");
    v.put(o, "summary", summary->kind == Val::Missing ? v.str("") : summary);
    v.put(o, "excerpt", v.str(s->excerpt));
    size_t mins = (wordCount(s->body) + 199) / 200;
    v.put(o, "reading_minutes", v.num(double(mins < 1 ? 1 : mins)));
    Val* tagsVal = v.arr();
    for (Tag* t : s->tagRefs) {
      Val* to = v.obj();
      to->object.reserve(3);
      v.put(to, "name", v.str(t->name));
      v.put(to, "slug", v.str(t->slug));
      v.put(to, "url", v.str(t->url));
      tagsVal->array.push_back(to);
    }
    v.put(o, "tags", tagsVal);
    return o;
  }
  Val* pageOf(Source* s) {
    Val* o;
    if (s->card) {
      // Reuse the post's card (same fields) instead of rebuilding it.
      o = v.obj();
      // One allocation for the whole object: the card pairs plus the six
      // page-only fields, instead of growing by push_back (which
      // reallocates ~4 times, copying the pairs each time).
      o->object.reserve(s->card->object.size() + 6);
      o->object.insert(o->object.end(), s->card->object.begin(), s->card->object.end());
    } else {
      o = cardOf(s);
    }
    v.put(o, "slug", v.str(s->slug));
    // The html lives in the Source for the whole case: a view, not a copy.
    // own() here copied the ~7 KB page body twice (parameter + arena).
    v.put(o, "content", v.str(s->html));
    v.put(o, "toc", s->toc ? s->toc : v.arr());
    v.put(o, "newer", linkOf(s->newer));
    v.put(o, "older", linkOf(s->older));
    v.put(o, "is_post", v.boolean(s->post));
    return o;
  }
  Val* rootVars(Val* siteVal, Val* allTags, Val* recent) {
    Val* root = v.obj();
    root->object.reserve(4);
    v.put(root, "site", siteVal);
    v.put(root, "tags", allTags);
    v.put(root, "recent", recent);
    return root;
  }

  Output build(Val* input) {
    bool prof = std::getenv("ZEN_PROFILE") != nullptr;
    auto t0 = std::chrono::steady_clock::now();
    auto lap = [&](const char* what, std::chrono::steady_clock::time_point& last) {
      if (!prof) return;
      auto now = std::chrono::steady_clock::now();
      std::fprintf(stderr, "[zen]   %-12s %ld us\n", what,
        std::chrono::duration_cast<std::chrono::microseconds>(now - last).count());
      last = now;
    };
    auto last = t0;
    Val* siteIn = v.get(input, "site");
    Val* templatesIn = v.get(input, "templates");
    Val* filesIn = v.get(input, "files");
    Str base = stripTrailingSlashes(v.get(siteIn, "base_url")->text);
    Val* siteTitle = v.get(siteIn, "title");
    double perPage = v.get(siteIn, "per_page")->number;
    double recentN = v.get(siteIn, "recent")->number;

    // Templates, parsed in code-unit name order; a template that does not
    // parse is absent (and reported).
    std::unordered_map<Str, std::vector<TNode>> raw;
    std::vector<Str> names;
    if (templatesIn->kind == Val::Object)
      for (auto& p : templatesIn->object) names.push_back(a.names[p.first]);
    std::sort(names.begin(), names.end(), [](const Str& x, const Str& y) { return lessCodeUnits(x, y); });
    for (auto& name : names) {
      Val* src = v.get(templatesIn, name);
      try {
        raw.emplace(name, parseTemplate(name, src->text, a));
      } catch (const TplFail& e) {
        report.push_back(e.what());
      }
    }
    int loopKey = a.key("loop");
    std::unordered_set<int> stable = {a.key("site"), a.key("tags"), a.key("recent")};
    for (auto& p : raw) {
      auto inlined = inlineIncludes(p.second, raw, 0);
      mergeText(inlined);
      collectDeps(inlined, loopKey, stable);
      groupStable(inlined, stable);
      templates.emplace(p.first, std::move(inlined));
    }
    Renderer renderer(a, v, templates);
    lap("templates", last);

    // 1-3. Sources, front matter and permalinks in ONE pass over the files
    //    in code-unit path order. (Previously three passes with a `kept`
    //    vector in between; the passes only re-walked the same Sources.)
    //    A .md source is text with line ends made "\n", anything else is
    //    static. A refused source is reported and dropped, as is a draft.
    //    Each page's output directory and url, its slug unique in its
    //    directory, in source order.
    std::vector<std::pair<Str, Val*>> files;
    if (filesIn->kind == Val::Array)
      for (Val* f : filesIn->array) files.emplace_back(Str(v.get(f, "path")->text), f);
    std::sort(files.begin(), files.end(), [](const auto& x, const auto& y) { return lessCodeUnits(x.first, y.first); });
    sources.reserve(files.size());
    std::unordered_map<Str, std::unordered_set<Str>> taken;
    taken[""] = {"posts", "tags", "page"};
    for (auto& [path, f] : files) {
      Val* content = v.get(f, "content");
      if (!isMd(path)) {
        statics.emplace_back(path, Str(content->text));
        staticPaths.insert(path);
        continue;
      }
      Source s;
      s.path = path;
      // No copy for LF sources: the content is a view into the cases.json
      // buffer, which lives for the whole runBatch. Only CRLF sources pay
      // for a normalisation, and that copy goes to the arena (stable).
      if (content->text.find('\r') == View::npos) s.text = content->text;
      else s.text = a.keep(lineEnds(content->text));
      Front fm = parseFront(s.text, a, v);
      if (!fm.error.empty()) { report.push_back("refused " + s.path + ": " + fm.error); continue; }
      s.data = fm.data;
      s.body = fm.body;
      s.post = hasPrefix(s.path, "posts/");
      if (!s.post && (hasPrefix(s.path, "tags/") || hasPrefix(s.path, "page/"))) {
        report.push_back("refused " + s.path + ": " + (hasPrefix(s.path, "tags/") ? "tags" : "page") + "/ is the generator's own");
        continue;
      }
      if (s.post && !v.has(s.data, "date")) { report.push_back("refused " + s.path + ": a post needs a date"); continue; }
      Val* layout = v.get(s.data, "layout");
      s.layout = layout->kind == Val::Missing ? (s.post ? "post" : "page") : Str(layout->text);
      if (templates.find(s.layout) == templates.end()) {
        report.push_back("refused " + s.path + ": layout \"" + s.layout + "\" is no template that parses");
        continue;
      }
      Val* draft = v.get(s.data, "draft");
      if (draft->kind == Val::Bool && draft->boolean) { report.push_back("draft " + s.path); continue; }
      Str dir = s.post ? "posts" : Str(dirOf(s.path));
      auto& set = taken[dir];
      Val* slugVal = v.get(s.data, "slug");
      Str slug = slugVal->kind == Val::Missing ? slugs.slug(v.get(s.data, "title")->text) : Str(slugVal->text);
      s.slug = uniqueSlug(std::move(slug), set);
      s.outDir = dir.empty() ? s.slug : dir + "/" + s.slug;
      s.url = base + "/" + s.outDir + "/";
      sourceUrls.emplace(s.path, s.url);
      sources.push_back(std::move(s));
    }
    lap("load", last);

    // 4. Markdown, links resolved to the pages and static files they name.
    for (auto& s : sources) {
      LinkEnv env{base, s.path, dirOf(s.path), sourceUrls, staticPaths, broken};
      MdResult md = renderMarkdown(s.body, env, a, v, slugs);
      s.html = std::move(md.html);
      s.excerpt = std::move(md.excerpt);
      s.toc = md.toc;
    }
    lap("markdown", last);

    // 5. Collections: the posts, newest first and then by slug.
    for (auto& s : sources) if (s.post) posts.push_back(&s);
    std::sort(posts.begin(), posts.end(), [&](Source* x, Source* y) {
      View dx = v.get(x->data, "date")->text, dy = v.get(y->data, "date")->text;
      if (dx != dy) return lessCodeUnits(dy, dx);
      return lessCodeUnits(x->slug, y->slug);
    });
    for (size_t k = 0; k < posts.size(); k++) {
      posts[k]->newer = k == 0 ? nullptr : posts[k - 1];
      posts[k]->older = k + 1 == posts.size() ? nullptr : posts[k + 1];
    }

    // 6. Tags, grouped by their slug, each named as first met in the posts'
    //    order, ordered by count and then slug.
    std::unordered_map<Str, Tag*> bySlug;
    auto ownTags = [&](Source* s) {
      std::vector<std::pair<Str, Str>> out; // (name, slug), each slug once
      std::unordered_set<Str> seen;
      Val* tagsVal = v.get(s->data, "tags");
      if (tagsVal->kind != Val::Array) return out;
      for (Val* t : tagsVal->array) {
        Str slug = slugs.slug(t->text);
        if (seen.insert(slug).second) out.emplace_back(Str(t->text), std::move(slug));
      }
      return out;
    };
    for (Source* p : posts) {
      for (auto& [name, slug] : ownTags(p)) {
        auto it = bySlug.find(slug);
        Tag* tag = nullptr;
        if (it == bySlug.end()) {
          tagStore.emplace_back();
          tag = &tagStore.back();
          tag->name = name;
          tag->slug = slug;
          tag->url = base + "/tags/" + slug + "/";
          bySlug.emplace(slug, tag);
        } else tag = it->second;
        tag->posts.push_back(p);
        p->tagRefs.push_back(tag);
      }
    }
    for (auto& s : sources) {
      if (s.post) continue;
      for (auto& [name, slug] : ownTags(&s)) {
        auto it = bySlug.find(slug);
        if (it != bySlug.end()) s.tagRefs.push_back(it->second);
        else {
          tagStore.emplace_back();
          Tag* t = &tagStore.back();
          t->name = name;
          t->slug = slug;
          t->url = "";
          s.tagRefs.push_back(t);
        }
      }
    }
    for (auto& [slug, tag] : bySlug) tags.push_back(tag);
    std::sort(tags.begin(), tags.end(), [](Tag* x, Tag* y) {
      if (x->posts.size() != y->posts.size()) return x->posts.size() > y->posts.size();
      return lessCodeUnits(x->slug, y->slug);
    });
    lap("collections", last);

    // 7. Layouts.
    std::unordered_map<Str, size_t> lastOut; // last output size per layout, for reserve()
    for (Source* p : posts) p->card = cardOf(p);
    Val* siteVal = v.obj();
    siteVal->object.reserve(2);
    v.put(siteVal, "title", siteTitle);
    v.put(siteVal, "base_url", v.str(base));
    Val* allTags = v.arr();
    for (Tag* t : tags) {
      Val* to = v.obj();
      to->object.reserve(4);
      v.put(to, "name", v.str(t->name));
      v.put(to, "slug", v.str(t->slug));
      v.put(to, "url", v.str(t->url));
      v.put(to, "count", v.num(double(t->posts.size())));
      allTags->array.push_back(to);
    }
    Val* recent = v.arr();
    {
      size_t n = recentN <= 0 ? 0 : size_t(recentN);
      if (n > posts.size()) n = posts.size();
      for (size_t k = 0; k < n; k++) recent->array.push_back(posts[k]->card);
    }
    auto renderInto = [&](Str& out, View name, std::initializer_list<std::pair<View, Val*>> extra) -> bool {
      auto it = templates.find(Str(name));
      if (it == templates.end()) return false;
      // Reserve the last output size for this layout (8192 first time):
      // a page render grows by ~150 appends, and growing from empty
      // reallocates ~9 times, copying ~2x the final size each page.
      auto ls = lastOut.find(Str(name));
      out.reserve(ls == lastOut.end() ? 8192 : ls->second);
      Val* root = rootVars(siteVal, allTags, recent);
      for (auto& kv : extra) v.put(root, kv.first, kv.second);
      Scope scope{root, nullptr};
      renderer.render(out, it->second, &scope);
      lastOut[Str(name)] = out.size();
      return true;
    };
    // Pages.
    long pageRenders = 0;
    for (auto& s : sources) {
      Str out;
      renderInto(out, s.layout, {{"page", pageOf(&s)}});
      ++pageRenders;
      output.put(s.outDir + "/index.html", std::move(out));
    }
    lap("layouts-pages", last);
    // Index pages.
    {
      size_t per = perPage <= 0 ? 1 : size_t(perPage);
      size_t pages = posts.empty() ? 1 : (posts.size() + per - 1) / per;
      if (pages < 1) pages = 1;
      auto indexUrl = [&](size_t k) { return k == 1 ? base + "/" : base + "/page/" + std::to_string(k) + "/"; };
      auto it = templates.find("index");
      if (it != templates.end()) {
        for (size_t k = 1; k <= pages; k++) {
          Val* postsVal = v.arr();
          size_t from = (k - 1) * per, to = k * per;
          if (to > posts.size()) to = posts.size();
          for (size_t i = from; i < to; i++) postsVal->array.push_back(posts[i]->card);
          Val* pager = v.obj();
          v.put(pager, "number", v.num(double(k)));
          v.put(pager, "total", v.num(double(pages)));
          v.put(pager, "url", v.own(indexUrl(k)));
          v.put(pager, "newer_url", v.own(k == 1 ? "" : indexUrl(k - 1)));
          v.put(pager, "older_url", v.own(k == pages ? "" : indexUrl(k + 1)));
          Str out;
          renderInto(out, "index", {{"posts", postsVal}, {"pager", pager}});
          output.put(k == 1 ? "index.html" : "page/" + std::to_string(k) + "/index.html", std::move(out));
        }
      }
    }
    lap("layouts-index", last);
    // Tag pages.
    {
      auto it = templates.find("tag");
      if (it != templates.end()) {
        for (Tag* t : tags) {
          Val* tagVal = v.obj();
          tagVal->object.reserve(5);
          v.put(tagVal, "name", v.str(t->name));
          v.put(tagVal, "slug", v.str(t->slug));
          v.put(tagVal, "url", v.str(t->url));
          v.put(tagVal, "count", v.num(double(t->posts.size())));
          Val* postsVal = v.arr();
          for (Source* p : t->posts) postsVal->array.push_back(p->card);
          v.put(tagVal, "posts", postsVal);
          Str out;
          renderInto(out, "tag", {{"tag", tagVal}});
          output.put("tags/" + t->slug + "/index.html", std::move(out));
        }
      }
    }
    lap("layouts-tags", last);
    // Tags index.
    {
      Str out;
      if (renderInto(out, "tags", {})) output.put("tags/index.html", std::move(out));
    }
    // Statics.
    for (auto& [path, content] : statics) {
      if (output.has(path)) report.push_back("shadowed " + path + ": a built page has its path");
      else output.put(path, content);
    }
    lap("layouts", last);
    if (prof) {
      std::fprintf(stderr, "[zen]   tpl-cache hits %ld misses %ld\n", renderer.cacheHits, renderer.cacheMisses);
      std::fprintf(stderr, "[zen]   nodes Text %ld Var %ld Raw %ld If %ld For %ld Inc %ld Blk %ld lookups %ld emits %ld escBytes %ld pageRenders %ld\n",
        renderer.nodeVisits[0], renderer.nodeVisits[1], renderer.nodeVisits[2], renderer.nodeVisits[3],
        renderer.nodeVisits[4], renderer.nodeVisits[5], renderer.nodeVisits[6],
        renderer.lookupCalls, renderer.emitCalls, renderer.escapeBytes, pageRenders);
    }

    // 8. Report.
    std::vector<Str> lines = report;
    for (auto& b : broken) lines.push_back(b);
    std::sort(lines.begin(), lines.end(), [](const Str& x, const Str& y) { return lessCodeUnits(x, y); });
    size_t pages = 0;
    for (auto& s : sources) if (!s.post) ++pages;
    lines.push_back("built " + std::to_string(pages) + " pages, " + std::to_string(posts.size()) +
                    " posts, " + std::to_string(tags.size()) + " tags and " + std::to_string(statics.size()) + " static files");
    Str reportText;
    for (auto& l : lines) { reportText += l; reportText += '\n'; }
    output.put("build-report.txt", std::move(reportText));
    return std::move(output);
  }
};

} // namespace zen
