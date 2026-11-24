// Zen N-API addon: runJson (one case), runBatch (the whole cases file),
// slug (native slugify) and unicodeVersion. Written from scratch.
#include <node_api.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include "zen_site.hpp"

namespace {

using namespace zen;
using Clock = std::chrono::steady_clock;

napi_value fail(napi_env env, const char* msg) {
  napi_throw_error(env, nullptr, msg);
  napi_value u;
  napi_get_undefined(env, &u);
  return u;
}

Str readFile(const char* path) {
  // Sized read (fstat + read) instead of an ostringstream over rdbuf:
  // the input is a few MB and ostringstream grows it byte by byte.
  int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error(std::string("cannot read ") + path);
  struct stat st {};
  if (::fstat(fd, &st) < 0) { ::close(fd); throw std::runtime_error(std::string("cannot stat ") + path); }
  Str data(size_t(st.st_size > 0 ? st.st_size : 0), '\0');
  size_t n = 0;
  while (n < data.size()) {
    ssize_t got = ::read(fd, data.data() + n, data.size() - n);
    if (got < 0) { if (errno == EINTR) continue; ::close(fd); throw std::runtime_error(std::string("cannot read ") + path); }
    if (got == 0) break;
    n += size_t(got);
  }
  ::close(fd);
  data.resize(n);
  return data;
}

napi_value runJson(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  try {
    Str json = nativeString(env, argv[0]);
    Slugger slugs(env, argv[1]);
    Arena a;
    Vals v(a);
    Json parser(json, a, v);
    Val* input = parser.parse();
    Site site(a, v, slugs);
    Output out = site.build(input);
    Str text;
    out.json(text);
    return jsString(env, text);
  } catch (const std::exception& e) {
    return fail(env, e.what());
  }
}

napi_value runBatch(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  bool prof = std::getenv("ZEN_PROFILE") != nullptr;
  auto t0 = Clock::now();
  auto ms = [&]() { return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count(); };
  int fd = -1;
  try {
    Str inputPath = nativeString(env, argv[0]);
    Str outputPath = nativeString(env, argv[1]);
    Slugger slugs(env, argv[2]);
    Str content = readFile(inputPath.c_str());
    long readUs = ms();
    Arena batchArena;
    Vals batchVals(batchArena);
    Json parser(content, batchArena, batchVals);
    Val* cases = parser.parse();
    if (cases->kind != Val::Array) throw std::runtime_error("cases.json is not an array");
    long parseUs = ms();
    fd = ::open(outputPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) throw std::runtime_error(std::string("cannot write ") + outputPath);
    auto writeAll = [&](View data) {
      for (size_t n = 0; n < data.size();) {
        ssize_t got = ::write(fd, data.data() + n, data.size() - n);
        if (got < 0) { if (errno == EINTR) continue; throw std::runtime_error("failed writing answers"); }
        if (got == 0) throw std::runtime_error("failed writing answers");
        n += size_t(got);
      }
    };
    writeAll("[");
    bool first = true;
    long buildUs = 0, serUs = 0, writeUs = 0;
    Sink sink;
    for (Val* c : cases->array) {
      Val* input = batchVals.get(c, "input");
      // Build with the same arena that parsed the input: key ids are
      // arena-specific, so a fresh arena would miss every lookup.
      Site site(batchArena, batchVals, slugs);
      auto b0 = Clock::now();
      Output result = site.build(input);
      buildUs += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - b0).count();
      auto s0 = Clock::now();
      sink.clear();
      // Reserve generously so the raw writer never grows mid-case: the
      // escaped form is ~1.03x the raw size for HTML.
      size_t raw = 2;
      for (auto& f : result.files) raw += f.first.size() + f.second.size() + 8;
      // 2x, not ~1.06x: jsonQuoteSink relies on it — every input byte
      // becomes at most 2 output bytes except 6-byte control-char escapes,
      // which ensure() individually. The extra reserve costs one ~15 MB
      // allocation per process (reused across cases) and removes ~450k
      // ensure() calls per batch.
      sink.reserve(raw * 2 + 1024);
      result.jsonTo(sink);
      serUs += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - s0).count();
      auto w0 = Clock::now();
      if (!first) writeAll(",");
      first = false;
      writeAll(View(sink.buf, sink.len));
      writeUs += std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - w0).count();
    }
    writeAll("]");
    if (::close(fd) < 0) { fd = -1; throw std::runtime_error("failed writing answers"); }
    fd = -1;
    if (prof) {
      std::fprintf(stderr,
        "[zen] read %ld us, json-parse %ld us, build %ld us, serialize %ld us, write %ld us, total %ld us\n",
        readUs, parseUs - readUs, buildUs, serUs, writeUs, ms());
    }
    napi_value u;
    napi_get_undefined(env, &u);
    return u;
  } catch (const std::exception& e) {
    if (fd >= 0) ::close(fd);
    return fail(env, e.what());
  }
}

napi_value slugFn(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_get_cb_info(env, info, &argc, argv, nullptr, nullptr);
  try {
    Str s = nativeString(env, argv[0]);
    return jsString(env, nativeSlug(s));
  } catch (const std::exception& e) {
    return fail(env, e.what());
  }
}

napi_value init(napi_env env, napi_value exports) {
  auto set = [&](const char* name, napi_callback fn) {
    napi_value f;
    napiCheck(env, napi_create_function(env, name, NAPI_AUTO_LENGTH, fn, nullptr, &f));
    napiCheck(env, napi_set_named_property(env, exports, name, f));
  };
  set("runJson", runJson);
  set("runBatch", runBatch);
  set("slug", slugFn);
  napi_value ver;
  napiCheck(env, napi_create_string_utf8(env, ZEN_UNICODE_VERSION, NAPI_AUTO_LENGTH, &ver));
  napiCheck(env, napi_set_named_property(env, exports, "unicodeVersion", ver));
  return exports;
}

} // namespace

NAPI_MODULE(zen, init)
