/// @file rut_net.cxx
/// Async HTTP fetch → TMemFile for rooture.
///
/// (fetch-url url) → future of LVAL_TOBJ(TMemFile*)
///
///   Downloads url on a worker thread using libcurl (no ROOT involvement).
///   After download, dispatches TMemFile construction to the main thread.
///   TMemFile(char*, Long64_t, "READ") copies the downloaded bytes internally,
///   so the fetch buffer is freed immediately after construction.
///
///   The future resolves to a LVAL_TOBJ(TMemFile*) on success, or
///   LVAL_ERR on curl error, HTTP error, or zombie TMemFile.
///
/// Usage:
///   (= {f}   (deref (fetch-url "https://alice-ccdb.cern.ch/Analysis/PID/TPC/Response/1686000000000")))
///   (= {obj} (. Get f "ccdb_object"))
///   (= {cl}  (. IsA obj))
///   (= {dm}  (. GetDataMember cl "mBetheBlochParams"))

#include "rooture.h"
#include "TMemFile.h"
#include "TKey.h"
#include "TClass.h"
#include "TDataMember.h"
#include "TList.h"
#include "TStreamerInfo.h"
#include "TStreamerElement.h"
#include "TObjArray.h"
#include <curl/curl.h>
#include <cstdlib>
#include <cstring>

// ---------------------------------------------------------------------------
// libcurl callbacks
// ---------------------------------------------------------------------------
namespace {
struct FetchBuf {
  char*  data = nullptr;
  size_t size = 0;
};

static size_t curl_write_cb(void* ptr, size_t sz, size_t nmemb, void* userdata)
{
  size_t incoming = sz * nmemb;
  auto*  buf      = static_cast<FetchBuf*>(userdata);
  char*  tmp      = static_cast<char*>(realloc(buf->data, buf->size + incoming));
  if (!tmp) return 0;
  buf->data = tmp;
  memcpy(buf->data + buf->size, ptr, incoming);
  buf->size += incoming;
  return incoming;
}

// Captures content-location header (CCDB redirect protocol).
struct HeaderState { std::string content_location; };

static size_t curl_header_cb(char* buf, size_t sz, size_t n, void* userdata)
{
  size_t len = sz * n;
  auto*  hs  = static_cast<HeaderState*>(userdata);
  std::string line(buf, len);
  std::string low = line;
  for (auto& c : low) c = (char)tolower((unsigned char)c);
  const std::string key = "content-location:";
  if (low.rfind(key, 0) == 0) {
    auto val = line.substr(key.size());
    auto s = val.find_first_not_of(" \t");
    auto e = val.find_last_not_of(" \t\r\n");
    hs->content_location = (s != std::string::npos) ? val.substr(s, e-s+1) : "";
  }
  return len;
}

// Parse content-location (comma-separated), return first http(s):// URL.
// If proxy_base is non-empty, rewrite the chosen URL to use that base instead
// of the original host (so downloads through a local proxy don't hit the
// external host directly).
static std::string pick_https_url(const std::string& cl,
                                   const std::string& proxy_base = {})
{
  std::stringstream ss(cl);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    auto s = tok.find_first_not_of(" \t");
    auto e = tok.find_last_not_of(" \t\r\n");
    if (s == std::string::npos) continue;
    tok = tok.substr(s, e-s+1);
    if (tok.rfind("https://", 0) == 0 || tok.rfind("http://", 0) == 0) {
      if (!proxy_base.empty()) {
        // Extract path component (everything after the third '/')
        // e.g. https://alice-ccdb.cern.ch/download/UUID → proxy_base + /download/UUID
        auto scheme_end = tok.find("://");
        if (scheme_end != std::string::npos) {
          auto path_start = tok.find('/', scheme_end + 3);
          if (path_start != std::string::npos)
            return proxy_base + tok.substr(path_start);
        }
      }
      return tok;
    }
  }
  return {};
}

// Extract base URL (scheme + host + port) from a URL.
static std::string url_base(const std::string& url)
{
  auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) return {};
  auto path_start = url.find('/', scheme_end + 3);
  if (path_start == std::string::npos) return url;
  return url.substr(0, path_start);
}

} // namespace

// ---------------------------------------------------------------------------
// (fetch-url url-string) → future
// ---------------------------------------------------------------------------
static lval* builtin_fetch_url(lenv* /*e*/, lval* a)
{
  LASSERT(a, a->count >= 1 && a->count <= 3,
          "fetch-url: expects 1-3 arguments (url [headers] [proxy])");
  LASSERT_TYPE("fetch-url", a, 0, LVAL_STR);

  std::string url = a->cell[0]->str;

  // Optional second arg: q-expression of "Header: value" strings
  std::vector<std::string> extra_headers;
  if (a->count >= 2) {
    LASSERT(a, a->cell[1]->type == LVAL_QEXPR,
            "fetch-url: second argument must be a Q-expression of header strings");
    for (int i = 0; i < a->cell[1]->count; i++) {
      LASSERT(a, a->cell[1]->cell[i]->type == LVAL_STR,
              "fetch-url: each header must be a string");
      extra_headers.push_back(a->cell[1]->cell[i]->str);
    }
  }

  // Optional third arg: proxy URL string
  std::string proxy;
  if (a->count >= 3) {
    LASSERT_TYPE("fetch-url", a, 2, LVAL_STR);
    proxy = a->cell[2]->str;
  }

  lval_del(a);

  auto rf = std::make_shared<RutFuture>();

  rut_pool_submit([rf, url, extra_headers, proxy]() mutable {
    FetchBuf    buf{};
    HeaderState hs{};
    lval*       result = nullptr;

    // Helper: run one curl GET, no auto-follow, capture content-location header.
    // Returns true on success (fills buf), false on error (sets result).
    auto do_get = [&](const std::string& dl_url) -> bool {
      CURL* curl = curl_easy_init();
      if (!curl) { result = lval_err("fetch-url: curl_easy_init failed"); return false; }

      buf = FetchBuf{};  // reset buffer for each attempt
      hs  = HeaderState{};

      struct curl_slist* hdrs = nullptr;
      for (auto& h : extra_headers)
        hdrs = curl_slist_append(hdrs, h.c_str());

      curl_easy_setopt(curl, CURLOPT_URL,             dl_url.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   curl_write_cb);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &buf);
      curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,  curl_header_cb);
      curl_easy_setopt(curl, CURLOPT_HEADERDATA,      &hs);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  0L);  // handle redirects manually
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST,  0L);
      curl_easy_setopt(curl, CURLOPT_USERAGENT,       "rooture/1.0");
      if (!proxy.empty())
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
      if (hdrs)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

      CURLcode rc = curl_easy_perform(curl);
      long http_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
      if (hdrs) curl_slist_free_all(hdrs);
      curl_easy_cleanup(curl);

      if (rc != CURLE_OK) {
        result = lval_err("fetch-url: %s", curl_easy_strerror(rc));
        free(buf.data); buf.data = nullptr; buf.size = 0;
        return false;
      }
      if (http_code == 303) {
        // CCDB redirect: caller handles via hs.content_location
        free(buf.data); buf.data = nullptr; buf.size = 0;
        return false;  // signal: check hs for redirect URL
      }
      if (http_code < 200 || http_code >= 300) {
        result = lval_err("fetch-url: HTTP %ld for %s", http_code, dl_url.c_str());
        free(buf.data); buf.data = nullptr; buf.size = 0;
        return false;
      }
      return true;
    };

    bool ok = do_get(url);
    if (!ok && !result && !hs.content_location.empty()) {
      // CCDB 303: pick the first http(s):// URL from content-location.
      // If the original request went through a local proxy (non-alice-ccdb host),
      // rewrite the redirect URL to use the same proxy base so downloads don't
      // bypass it and hit the external host directly.
      std::string base     = url_base(url);
      std::string ext_base = "https://alice-ccdb.cern.ch";
      bool use_proxy = !base.empty() && base != ext_base &&
                       base != "http://alice-ccdb.cern.ch";
      std::string redirect = pick_https_url(hs.content_location,
                                             use_proxy ? base : std::string{});
      if (redirect.empty()) {
        result = lval_err("fetch-url: CCDB 303 but no http(s):// URL in content-location: %s",
                          hs.content_location.c_str());
      } else {
        ok = do_get(redirect);
      }
    }

    if (ok) {
      rut_dispatch_work([&] {
        TMemFile* mf = new TMemFile(url.c_str(), buf.data,
                                    static_cast<Long64_t>(buf.size), "READ");
        free(buf.data); buf.data = nullptr;
        if (mf && !mf->IsZombie())
          result = lval_tobj(mf, TClass::GetClass("TMemFile"));
        else {
          delete mf;
          result = lval_err("fetch-url: TMemFile is zombie — not a valid ROOT file");
        }
      });
    } else if (!result) {
      result = lval_err("fetch-url: unknown error");
    }

    std::lock_guard<std::mutex> lock(rf->mu);
    rf->result   = result;
    rf->realized = true;
    rf->cv.notify_all();
  });

  return lval_future_new(std::move(rf));
}

// ---------------------------------------------------------------------------
// (tfile-get tfile key-name) → LVAL_TOBJ
//
// Reads a named key from a TFile/TMemFile without going through Cling.
// Cling's JIT wrapper for .Get crashes on TClass::GetClass(type_info) when
// no compiled dictionary exists.  This builtin calls the C++ API directly:
//   TClass::GetClass(const char*) — returns emulated class from StreamerInfo
//   TKey::ReadObjectAny(cl)      — deserialises using that emulated class
// Neither path touches type_info, so it works without a compiled dictionary.
// Reading from TMemFile is in-memory (microseconds), so no blocking concern.
// ---------------------------------------------------------------------------
static lval* builtin_tfile_get(lenv* /*e*/, lval* a)
{
  LASSERT_NUM("tfile-get", a, 2);
  LASSERT_TYPE("tfile-get", a, 0, LVAL_TOBJ);
  LASSERT_TYPE("tfile-get", a, 1, LVAL_STR);

  TFile* f       = static_cast<TFile*>(a->cell[0]->obj);
  std::string key = a->cell[1]->str;
  lval_del(a);

  TKey* tk = f->GetKey(key.c_str());
  if (!tk)
    return lval_err("tfile-get: key '%s' not found", key.c_str());

  TClass* cl = TClass::GetClass(tk->GetClassName());
  if (!cl)
    return lval_err("tfile-get: no TClass for '%s'", tk->GetClassName());

  void* obj = tk->ReadObjectAny(cl);
  if (!obj)
    return lval_err("tfile-get: ReadObjectAny failed for key '%s'", key.c_str());

  return lval_tobj(obj, cl);
}

// ---------------------------------------------------------------------------
// (tobj-member obj member-name) → lval
//
// Reads a named data member from an object whose TClass may be emulated
// (no compiled dictionary).  Traverses base classes and nested compound
// members recursively (depth-limited to 4 levels).
// ---------------------------------------------------------------------------

// Forward declaration so the recursive helper can be declared before use.
static lval* tobj_read_value(char* base, Long_t offset,
                              const std::string& tname, int arrlen);

// Recursive member search: search si's elements for `name`, also descending
// into BASE and kObject (compound) sub-objects.  `ptr` points to the start
// of the object described by si.  depth prevents infinite loops.
static lval* tobj_find_member(char* ptr, TStreamerInfo* si,
                               const std::string& name, int depth = 0)
{
  if (!si || depth > 4) return nullptr;
  TObjArray* elems = si->GetElements();

  for (int i = 0; i < elems->GetEntriesFast(); i++) {
    TStreamerElement* e = static_cast<TStreamerElement*>(elems->At(i));
    if (!e) continue;

    bool is_base = (std::string(e->IsA()->GetName()) == "TStreamerBase");
    int  tcode   = e->GetType();
    // kObject=61, kObjectp=62, kObjectP=64, kAny=69, kAnyp=70
    bool is_obj  = (!is_base && (tcode == 61 || tcode == 62 || tcode == 64 ||
                                  tcode == 69 || tcode == 70));

    if (name == e->GetName() && !is_base) {
      return tobj_read_value(ptr, e->GetOffset(),
                              e->GetTypeName(), e->GetArrayLength());
    }

    // Recurse into base class or compound member sub-object
    if (is_base || is_obj) {
      // BASE elements: class name == element name; kObject elements: use GetTypeName()
      const char* clname = is_base ? e->GetName() : e->GetTypeName();
      TClass* sub_cl = TClass::GetClass(clname);
      if (!sub_cl) continue;
      TStreamerInfo* sub_si = static_cast<TStreamerInfo*>(sub_cl->GetStreamerInfo());
      if (!sub_si) continue;
      lval* v = tobj_find_member(ptr + e->GetOffset(), sub_si, name, depth + 1);
      if (v) return v;
    }
  }
  return nullptr;
}

static lval* tobj_read_value(char* base, Long_t offset,
                              const std::string& tname, int arrlen)
{
  bool is_arr = arrlen > 0;

  if (tname.find("array<float") != std::string::npos ||
      tname.find("std::array<float") != std::string::npos) {
    TClass* arr_cl = TClass::GetClass(tname.c_str());
    int n = arr_cl ? (arr_cl->Size() / (int)sizeof(float)) : arrlen;
    lval* lst = lval_qexpr();
    float* fp = reinterpret_cast<float*>(base + offset);
    for (int i = 0; i < n; i++) {
      lval* v = (lval*)malloc(sizeof(lval));
      v->type = LVAL_FLOAT32; v->floating = fp[i];
      v->err = v->sym = v->str = nullptr; v->obj = nullptr;
      v->cls = nullptr; v->method = nullptr; v->methodArgs = nullptr;
      v->builtin = nullptr; v->env = nullptr;
      v->formals = v->body = nullptr; v->count = 0; v->cell = nullptr;
      lval_add(lst, v);
    }
    return lst;
  }
  if (tname.find("vector<float") != std::string::npos ||
      tname.find("std::vector<float") != std::string::npos) {
    auto* vec = reinterpret_cast<std::vector<float>*>(base + offset);
    lval* lst = lval_qexpr();
    for (float f : *vec) {
      lval* v = (lval*)malloc(sizeof(lval));
      v->type = LVAL_FLOAT32; v->floating = f;
      v->err = v->sym = v->str = nullptr; v->obj = nullptr;
      v->cls = nullptr; v->method = nullptr; v->methodArgs = nullptr;
      v->builtin = nullptr; v->env = nullptr;
      v->formals = v->body = nullptr; v->count = 0; v->cell = nullptr;
      lval_add(lst, v);
    }
    return lst;
  }
  if (tname.find("vector<double") != std::string::npos ||
      tname.find("std::vector<double") != std::string::npos) {
    auto* vec = reinterpret_cast<std::vector<double>*>(base + offset);
    lval* lst = lval_qexpr();
    for (double d : *vec) lval_add(lst, lval_floating(d));
    return lst;
  }
  if (tname == "float" || tname == "Float_t") {
    if (is_arr) {
      lval* lst = lval_qexpr();
      float* fp = reinterpret_cast<float*>(base + offset);
      for (int i = 0; i < arrlen; i++) {
        lval* v = (lval*)malloc(sizeof(lval));
        v->type = LVAL_FLOAT32; v->floating = fp[i];
        v->err = v->sym = v->str = nullptr; v->obj = nullptr;
        v->cls = nullptr; v->method = nullptr; v->methodArgs = nullptr;
        v->builtin = nullptr; v->env = nullptr;
        v->formals = v->body = nullptr; v->count = 0; v->cell = nullptr;
        lval_add(lst, v);
      }
      return lst;
    }
    lval* v = (lval*)malloc(sizeof(lval));
    v->type = LVAL_FLOAT32; v->floating = *reinterpret_cast<float*>(base + offset);
    v->err = v->sym = v->str = nullptr; v->obj = nullptr;
    v->cls = nullptr; v->method = nullptr; v->methodArgs = nullptr;
    v->builtin = nullptr; v->env = nullptr;
    v->formals = v->body = nullptr; v->count = 0; v->cell = nullptr;
    return v;
  }
  if (tname == "double" || tname == "Double_t")
    return lval_floating(*reinterpret_cast<double*>(base + offset));
  if (tname == "int" || tname == "Int_t")
    return lval_num(*reinterpret_cast<int*>(base + offset));
  if (tname == "long" || tname == "Long_t" || tname == "Long64_t")
    return lval_num(*reinterpret_cast<long*>(base + offset));
  if (tname == "unsigned long" || tname == "ULong_t" ||
      tname == "ULong64_t" || tname == "unsigned long long")
    return lval_num((long)*reinterpret_cast<unsigned long*>(base + offset));
  if (tname == "unsigned int" || tname == "UInt_t")
    return lval_num((long)*reinterpret_cast<unsigned int*>(base + offset));
  if (tname == "short" || tname == "Short_t")
    return lval_num((long)*reinterpret_cast<short*>(base + offset));
  if (tname == "unsigned short" || tname == "UShort_t")
    return lval_num((long)*reinterpret_cast<unsigned short*>(base + offset));
  if (tname == "bool" || tname == "Bool_t")
    return lval_num((long)*reinterpret_cast<bool*>(base + offset));
  // Generic fallback: class whose size is a multiple of float → float array
  {
    TClass* ft = TClass::GetClass(tname.c_str());
    if (ft && ft->Size() > 0 && ft->Size() % sizeof(float) == 0) {
      int n = ft->Size() / (int)sizeof(float);
      lval* lst = lval_qexpr();
      float* fp = reinterpret_cast<float*>(base + offset);
      for (int k = 0; k < n; k++) {
        lval* v = (lval*)malloc(sizeof(lval));
        v->type = LVAL_FLOAT32; v->floating = fp[k];
        v->err = v->sym = v->str = nullptr; v->obj = nullptr;
        v->cls = nullptr; v->method = nullptr; v->methodArgs = nullptr;
        v->builtin = nullptr; v->env = nullptr;
        v->formals = v->body = nullptr; v->count = 0; v->cell = nullptr;
        lval_add(lst, v);
      }
      return lst;
    }
  }
  return nullptr;  // caller will emit error
}

static lval* builtin_tobj_member(lenv* /*e*/, lval* a)
{
  LASSERT_NUM("tobj-member", a, 2);
  LASSERT_TYPE("tobj-member", a, 0, LVAL_TOBJ);
  LASSERT_TYPE("tobj-member", a, 1, LVAL_STR);

  void*       obj  = a->cell[0]->obj;
  TClass*     cl   = a->cell[0]->cls;
  std::string name = a->cell[1]->str;
  lval_del(a);

  if (!cl)
    return lval_err("tobj-member: object has no TClass");

  TStreamerInfo* si = static_cast<TStreamerInfo*>(cl->GetStreamerInfo());
  if (!si)
    return lval_err("tobj-member: no StreamerInfo for %s", cl->GetName());

  lval* found = tobj_find_member(static_cast<char*>(obj), si, name);
  if (found) return found;

  // Build available-member list for error message
  {
    std::string avail;
    std::function<void(TStreamerInfo*, int)> collect = [&](TStreamerInfo* csi, int depth) {
      if (!csi || depth > 3) return;
      TObjArray* elems = csi->GetElements();
      for (int i = 0; i < elems->GetEntriesFast(); i++) {
        TStreamerElement* e = static_cast<TStreamerElement*>(elems->At(i));
        if (!e) continue;
        bool is_base = std::string(e->IsA()->GetName()) == "TStreamerBase";
        if (!avail.empty()) avail += ", ";
        avail += e->GetName();
        if (is_base) {
          TClass* bc = TClass::GetClass(e->GetName());
          if (bc) collect(static_cast<TStreamerInfo*>(bc->GetStreamerInfo()), depth+1);
        }
      }
    };
    collect(si, 0);
    return lval_err("tobj-member: member '%s' not found in %s. Available: %s",
                    name.c_str(), cl->GetName(), avail.c_str());
  }

}

// ---------------------------------------------------------------------------
// Registration — curl_global_init called once here, before the thread pool
// is created (lenv_add_builtins runs before rut_pool_create in rut_repl.cxx).
// ---------------------------------------------------------------------------
void lenv_add_builtins_net(lenv* e)
{
  curl_global_init(CURL_GLOBAL_DEFAULT);
  lenv_add_builtin(e, "fetch-url",   builtin_fetch_url);
  lenv_add_builtin(e, "tfile-get",   builtin_tfile_get);
  lenv_add_builtin(e, "tobj-member", builtin_tobj_member);
}
