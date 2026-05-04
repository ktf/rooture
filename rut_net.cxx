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
// libcurl write callback — appends incoming data into a growing heap buffer
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
  if (!tmp) return 0;   // signals CURL error (CURLE_WRITE_ERROR)
  buf->data = tmp;
  memcpy(buf->data + buf->size, ptr, incoming);
  buf->size += incoming;
  return incoming;
}
} // namespace

// ---------------------------------------------------------------------------
// (fetch-url url-string) → future
// ---------------------------------------------------------------------------
static lval* builtin_fetch_url(lenv* /*e*/, lval* a)
{
  LASSERT_NUM("fetch-url", a, 1);
  LASSERT_TYPE("fetch-url", a, 0, LVAL_STR);

  std::string url = a->cell[0]->str;
  lval_del(a);

  auto rf = std::make_shared<RutFuture>();

  rut_pool_submit([rf, url]() mutable {
    FetchBuf buf{};
    lval*    result = nullptr;

    CURL* curl = curl_easy_init();
    if (!curl) {
      result = lval_err("fetch-url: curl_easy_init failed");
    } else {
      curl_easy_setopt(curl, CURLOPT_URL,             url.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,   curl_write_cb);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA,       &buf);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION,  1L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER,  1L);
      curl_easy_setopt(curl, CURLOPT_USERAGENT,       "rooture/1.0");

      CURLcode rc = curl_easy_perform(curl);
      if (rc != CURLE_OK) {
        result = lval_err("fetch-url: %s", curl_easy_strerror(rc));
        free(buf.data);
      } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code < 200 || http_code >= 300) {
          result = lval_err("fetch-url: HTTP %ld for %s", http_code, url.c_str());
          free(buf.data);
        } else {
          // Dispatch TMemFile construction to the main thread.
          // rut_dispatch_work blocks until the main thread has executed the lambda.
          // TMemFile(char*, Long64_t, "READ") copies buf.data internally,
          // so we free it immediately after and the column stays self-contained.
          rut_dispatch_work([&] {
            TMemFile* mf = new TMemFile(url.c_str(), buf.data,
                                        static_cast<Long64_t>(buf.size), "READ");
            free(buf.data);
            buf.data = nullptr;
            if (mf && !mf->IsZombie()) {
              result = lval_tobj(mf, TClass::GetClass("TMemFile"));
            } else {
              delete mf;
              result = lval_err("fetch-url: TMemFile is zombie — not a valid ROOT file");
            }
          });
        }
      }
      curl_easy_cleanup(curl);
    }

    {
      std::lock_guard<std::mutex> lock(rf->mu);
      rf->result   = result;
      rf->realized = true;
    }
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
// (no compiled dictionary).  Uses TDataMember::GetOffset() + pointer
// arithmetic — no Cling involved, no type_info lookup.
//
// Supported member types (via TDataMember::GetTypeName()):
//   float / Float_t              → LVAL_FLOAT32
//   double / Double_t            → LVAL_FLOAT
//   int / Int_t / long / Long_t  → LVAL_NUM
//   std::array<float,N>          → LVAL_SEXPR (Q-expr list of LVAL_FLOAT32)
//   std::vector<double>          → LVAL_SEXPR (Q-expr list of LVAL_FLOAT)
// ---------------------------------------------------------------------------
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

  // Emulated classes (no compiled dictionary) store member info in TStreamerInfo,
  // not in GetListOfDataMembers().  Use GetStreamerInfo()->GetElements() instead.
  TStreamerInfo* si = static_cast<TStreamerInfo*>(cl->GetStreamerInfo());
  if (!si)
    return lval_err("tobj-member: no StreamerInfo for %s", cl->GetName());

  TObjArray* elems = si->GetElements();
  TStreamerElement* se = nullptr;
  for (int i = 0; i < elems->GetEntriesFast(); i++) {
    TStreamerElement* e = static_cast<TStreamerElement*>(elems->At(i));
    if (e && name == e->GetName()) { se = e; break; }
  }
  if (!se) {
    // Print available names to help debugging
    std::string avail;
    for (int i = 0; i < elems->GetEntriesFast(); i++) {
      TStreamerElement* e = static_cast<TStreamerElement*>(elems->At(i));
      if (e) { if (!avail.empty()) avail += ", "; avail += e->GetName(); }
    }
    return lval_err("tobj-member: member '%s' not found in %s. Available: %s",
                    name.c_str(), cl->GetName(), avail.c_str());
  }

  char*       base   = static_cast<char*>(obj);
  Long_t      offset = se->GetOffset();
  std::string tname  = se->GetTypeName();
  int         arrlen = se->GetArrayLength();  // >0 for fixed arrays
  bool        is_arr = arrlen > 0;

  // std::array<float,N> — stored as N contiguous floats
  if (tname.find("array<float") != std::string::npos ||
      tname.find("std::array<float") != std::string::npos) {
    // Infer N from the class size stored in the streamer
    TClass* arr_cl = TClass::GetClass(tname.c_str());
    int n = arr_cl ? (arr_cl->Size() / sizeof(float)) : arrlen;
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

  // std::vector<double>
  if (tname.find("vector<double") != std::string::npos ||
      tname.find("std::vector<double") != std::string::npos) {
    auto* vec = reinterpret_cast<std::vector<double>*>(base + offset);
    lval* lst = lval_qexpr();
    for (double d : *vec) lval_add(lst, lval_floating(d));
    return lst;
  }

  // Scalar float
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

  // Scalar double
  if (tname == "double" || tname == "Double_t")
    return lval_floating(*reinterpret_cast<double*>(base + offset));

  // Scalar int/long
  if (tname == "int" || tname == "Int_t")
    return lval_num(*reinterpret_cast<int*>(base + offset));
  if (tname == "long" || tname == "Long_t" || tname == "Long64_t")
    return lval_num(*reinterpret_cast<long*>(base + offset));

  return lval_err("tobj-member: unsupported type '%s' for member '%s'",
                  tname.c_str(), name.c_str());
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
