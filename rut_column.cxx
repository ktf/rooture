/// @file rut_column.cxx
/// Columnar branch loading and operations for AO2D / TTree data.
///
/// A Column (LVAL_COLUMN) is a fully-loaded, typed, flat buffer of one TBranch.
/// Columns from the same TTree are always index-aligned (same n entries).
///
/// Loading is two-phase:
///   1. rut_open_file() — dispatches TFile::Open to the main thread (safe).
///   2. GetBulkRead() loop on the calling (future) thread — I/O + decompress.
///
/// Column operations (col-map, col-filter, col-reduce, col-zip) accept a
/// jit-fn whose compiled C++ function pointer is resolved once via rut_calc,
/// then applied in a tight loop — no interpreter overhead per element.

#include "rooture.h"
#include "TFile.h"
#include "TTree.h"
#include "TBranch.h"
#include "TLeaf.h"
#include "TString.h"
#include "TBufferFile.h"
#include "TH1.h"
#include "TH2.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>

// ---------------------------------------------------------------------------
// File cache — one TFile handle per thread per path.
// TFile::Open is dispatched to the main thread (ROOT global-state safety),
// but the handle is stored thread-locally so concurrent workers each have
// their own file descriptor and can call GetBulkEntries without racing.
// ---------------------------------------------------------------------------
static thread_local std::unordered_map<std::string, TFile*> t_file_cache;

// Look up or open a thread-local file handle. Safe to call from any thread.
static TFile* file_cache_get(const std::string& path) {
  auto it = t_file_cache.find(path);
  if (it != t_file_cache.end()) {
    if (it->second && !it->second->IsZombie()) return it->second;
    delete it->second;
    t_file_cache.erase(it);
  }
  // TFile::Open touches ROOT global state — dispatch to main thread.
  TFile* f = nullptr;
  rut_dispatch_work([&]{
    f = TFile::Open(path.c_str(), "READ");
    if (f && f->IsZombie()) { delete f; f = nullptr; }
  });
  if (f) t_file_cache[path] = f;
  return f;
}

// ---------------------------------------------------------------------------
// Column cache — one shared result per (path, tree, branch).
// Holds weak_ptr so columns are freed automatically when no lval references
// them.  The shared_future deduplicates concurrent requests: the first caller
// does the I/O while others wait, then all share the same buffer.
//
// Lifetime: a column lives as long as some lval (or pre-fetch layer) holds a
// RutColumnPtr (shared_ptr<RutColumn>).  The cache entry survives as an
// expired weak_ptr and is evicted on the next access for the same key.
// ---------------------------------------------------------------------------
static std::mutex g_col_cache_mu;
static std::unordered_map<std::string,
                          std::shared_future<std::weak_ptr<RutColumn>>> g_col_cache;

// Close all cached files on the calling thread and clear the column cache.
void rut_file_cache_clear() {
  for (auto& kv : t_file_cache) { kv.second->Close(); delete kv.second; }
  t_file_cache.clear();
  std::lock_guard<std::mutex> lock(g_col_cache_mu);
  g_col_cache.clear();
}

// ---------------------------------------------------------------------------
// Aligned allocation for column data buffers.
// 128 bytes = Apple Silicon L1/L2 cache-line boundary; also covers AVX-512.
// aligned_alloc requires size to be a multiple of the alignment.
// ---------------------------------------------------------------------------
static constexpr size_t kColAlign = 128;
static inline void* col_alloc(size_t n_bytes) {
  size_t sz = (n_bytes + kColAlign - 1) & ~(kColAlign - 1);
  if (sz == 0) sz = kColAlign;
  return aligned_alloc(kColAlign, sz);
}

// ---------------------------------------------------------------------------
// Helpers
// ColDtype letter mapping: F=float32 D=float64 I=int32 i=uint32
//                          S=int16   s=uint16  B=int8  b=uint8  O=bool
// ---------------------------------------------------------------------------
static size_t col_dtype_size(int d) {
  switch (d) {
    case COL_FLOAT64:
    case COL_INT64:
    case COL_UINT64:               return 8;
    case COL_FLOAT32:
    case COL_INT32:
    case COL_UINT32:               return 4;
    case COL_INT16:
    case COL_UINT16:               return 2;
    case COL_INT8:
    case COL_UINT8:
    case COL_BOOL:                 return 1;
    default:                       return 0;
  }
}

static const char* col_dtype_name(int d) {
  switch (d) {
    case COL_FLOAT32: return "float";
    case COL_FLOAT64: return "double";
    case COL_INT64:   return "long long";
    case COL_UINT64:  return "unsigned long long";
    case COL_INT32:   return "int";
    case COL_UINT32:  return "unsigned int";
    case COL_INT16:   return "short";
    case COL_UINT16:  return "unsigned short";
    case COL_INT8:    return "char";
    case COL_UINT8:   return "unsigned char";
    case COL_BOOL:    return "bool";
    default:          return "unknown";
  }
}

// Parse a C type name string to a ColDtype, returns -1 if unknown.
static int col_dtype_from_name(const char* name) {
  if (strcmp(name, "float")              == 0) return COL_FLOAT32;
  if (strcmp(name, "double")             == 0) return COL_FLOAT64;
  if (strcmp(name, "long long")          == 0) return COL_INT64;
  if (strcmp(name, "unsigned long long") == 0) return COL_UINT64;
  if (strcmp(name, "ULong64_t")          == 0) return COL_UINT64;
  if (strcmp(name, "Long64_t")           == 0) return COL_INT64;
  if (strcmp(name, "int")                == 0) return COL_INT32;
  if (strcmp(name, "unsigned int")       == 0) return COL_UINT32;
  if (strcmp(name, "short")              == 0) return COL_INT16;
  if (strcmp(name, "unsigned short")     == 0) return COL_UINT16;
  if (strcmp(name, "char")               == 0) return COL_INT8;
  if (strcmp(name, "unsigned char")      == 0) return COL_UINT8;
  if (strcmp(name, "bool")               == 0) return COL_BOOL;
  return -1;
}

static int branch_dtype(TBranch* br) {
  TString t = br->GetTitle();
  if (t.EndsWith("/F")) return COL_FLOAT32;
  if (t.EndsWith("/D")) return COL_FLOAT64;
  if (t.EndsWith("/L")) return COL_INT64;
  if (t.EndsWith("/l")) return COL_UINT64;
  if (t.EndsWith("/I")) return COL_INT32;
  if (t.EndsWith("/i")) return COL_UINT32;
  if (t.EndsWith("/S")) return COL_INT16;
  if (t.EndsWith("/s")) return COL_UINT16;
  if (t.EndsWith("/B")) return COL_INT8;
  if (t.EndsWith("/b")) return COL_UINT8;
  if (t.EndsWith("/O")) return COL_BOOL;
  return -1;
}

// ---------------------------------------------------------------------------
// lval constructor for LVAL_COLUMN (used by rut_column.cxx only)
// ---------------------------------------------------------------------------
static lval* lval_column(RutColumnPtr col) {
  lval* v = (lval*)malloc(sizeof(lval));
  v->type = LVAL_COLUMN;
  v->obj  = new RutColumnPtr(std::move(col));
  return v;
}

static RutColumnPtr& col_of(lval* v) {
  return *(RutColumnPtr*)v->obj;
}

// ---------------------------------------------------------------------------
// Core loader — safe to call from any thread.
// file_cache_get dispatches TFile::Open to the main thread but stores the
// handle in thread-local storage, so every subsequent operation (Get<TTree>,
// GetBranch, GetBulkEntries) runs on the calling thread using its own file
// descriptor — no shared mutable state, no races.
// ---------------------------------------------------------------------------
static RutColumnPtr load_branch_impl(const char* path,
                                      const char* tree_path,
                                      const char* branch_name) {
  std::string p(path), tp(tree_path), bn(branch_name);

  TFile* f = file_cache_get(p);
  if (!f) return nullptr;

  // TTree deserialization (TTree::Streamer) touches ROOT's global StreamerInfo
  // registry (TClass::RegisterStreamerInfo) which is not thread-safe.  Dispatch
  // to the main thread so registrations are always serialised, even though each
  // worker thread has its own TFile handle.
  TTree*   tree  = nullptr;
  TBranch* br    = nullptr;
  int      dtype = -1;
  Long64_t total = 0;
  TFile*   f_cap = f;
  rut_dispatch_work([&]{
    tree = f_cap->Get<TTree>(tp.c_str());
    if (!tree) return;
    br    = tree->GetBranch(bn.c_str());
    if (!br) return;
    dtype = branch_dtype(br);
    total = br->GetEntries();
  });
  if (!tree || !br || dtype < 0 || total <= 0) return nullptr;

  size_t esz = col_dtype_size(dtype);
  void*  buf = col_alloc((size_t)total * esz);
  if (!buf) return nullptr;

  auto& bulk = br->GetBulkRead();
  TBufferFile tbuf(TBuffer::kRead, 512 * 1024);
  Long64_t entry  = 0;
  size_t   cursor = 0;

  while (entry < total) {
    Int_t n = bulk.GetBulkEntries(entry, tbuf);
    if (n <= 0) break;
    memcpy((char*)buf + cursor * esz, tbuf.GetCurrent(), (size_t)n * esz);
    cursor += (size_t)n;
    entry  += n;
  }

  auto col = std::make_shared<RutColumn>();
  col->dtype = dtype;
  col->n     = cursor;
  col->data  = buf;
  return col;
}

// ---------------------------------------------------------------------------
// Cached loader — wraps load_branch_impl with the column cache.
// First caller for a given (path, tree, branch) does the I/O; concurrent
// callers for the same key wait on the shared_future and get the same result.
// ---------------------------------------------------------------------------
static RutColumnPtr load_branch_cached(const char* path,
                                        const char* tree_path,
                                        const char* branch_name) {
  std::string key = std::string(path) + "\t" + tree_path + "\t" + branch_name;

  while (true) {
    std::shared_future<std::weak_ptr<RutColumn>> fut;
    bool i_will_load = false;
    std::promise<std::weak_ptr<RutColumn>> my_promise;

    {
      std::lock_guard<std::mutex> lock(g_col_cache_mu);
      auto it = g_col_cache.find(key);
      if (it != g_col_cache.end()) {
        if (it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
          // Future resolved — try to promote the weak_ptr.
          if (auto sp = it->second.get().lock()) return sp;  // still alive
          g_col_cache.erase(it);   // expired — evict, fall through to reload
        } else {
          fut = it->second;         // still loading — wait below
        }
      }
      if (!fut.valid()) {
        // Not in cache (or just evicted) — this thread will load.
        fut = my_promise.get_future().share();
        g_col_cache[key] = fut;
        i_will_load = true;
      }
    }

    if (i_will_load) {
      try {
        RutColumnPtr col = load_branch_impl(path, tree_path, branch_name);
        my_promise.set_value(std::weak_ptr<RutColumn>(col));
        return col;
      } catch (...) {
        { std::lock_guard<std::mutex> lock(g_col_cache_mu); g_col_cache.erase(key); }
        my_promise.set_exception(std::current_exception());
        throw;
      }
    }

    // Wait for the loading thread, draining the Cling queue so the main thread
    // can service TFile::Open dispatches from the loading thread.
    while (fut.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout)
      rut_drain_cling_queue();

    if (auto sp = fut.get().lock()) return sp;

    // Rare: loader finished but the column was freed before we could lock().
    // Evict the stale entry and retry from the top.
    {
      std::lock_guard<std::mutex> lock(g_col_cache_mu);
      auto it = g_col_cache.find(key);
      if (it != g_col_cache.end() &&
          it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready &&
          !it->second.get().lock())
        g_col_cache.erase(it);
    }
    // loop
  }
}

// ---------------------------------------------------------------------------
// (load-branch path tree-path branch-name) → Column
// Dispatches to a future internally when called from the main thread so the
// I/O doesn't block the event loop.  When already in a future, runs directly.
// ---------------------------------------------------------------------------
static lval* builtin_load_branch(lenv* e, lval* a) {
  LASSERT_NUM("load-branch", a, 3);
  LASSERT_TYPE("load-branch", a, 0, LVAL_STR);
  LASSERT_TYPE("load-branch", a, 1, LVAL_STR);
  LASSERT_TYPE("load-branch", a, 2, LVAL_STR);

  std::string path   = a->cell[0]->str;
  std::string tree   = a->cell[1]->str;
  std::string branch = a->cell[2]->str;
  lval_del(a);

  RutColumnPtr col = load_branch_cached(path.c_str(), tree.c_str(), branch.c_str());
  if (!col)
    return lval_err("load-branch: failed to load '%s' from '%s' in '%s'",
                    branch.c_str(), tree.c_str(), path.c_str());

  return lval_column(std::move(col));
}

// (load-branches path tree-path {name1 name2 …}) → {Column Column …}
// Loads all listed branches in parallel (one future per branch).
static lval* builtin_load_branches(lenv* e, lval* a) {
  LASSERT_NUM("load-branches", a, 3);
  LASSERT_TYPE("load-branches", a, 0, LVAL_STR);
  LASSERT_TYPE("load-branches", a, 1, LVAL_STR);
  LASSERT_TYPE("load-branches", a, 2, LVAL_QEXPR);

  std::string path = a->cell[0]->str;
  std::string tree = a->cell[1]->str;

  lval* names = a->cell[2];
  int   nb    = names->count;
  for (int i = 0; i < nb; i++) {
    LASSERT(a, names->cell[i]->type == LVAL_STR,
            "load-branches: branch name %d must be a string", i);
  }

  // Collect branch names before freeing `a`.
  std::vector<std::string> bnames;
  bnames.reserve(nb);
  for (int i = 0; i < nb; i++) bnames.push_back(names->cell[i]->str);
  lval_del(a);

  // Load in parallel: one task per branch.
  std::vector<std::shared_future<RutColumnPtr>> futs;
  futs.reserve(nb);
  for (int i = 0; i < nb; i++) {
    auto task = std::make_shared<std::packaged_task<RutColumnPtr()>>(
      [path, tree, bn = bnames[i]]() -> RutColumnPtr {
        return load_branch_cached(path.c_str(), tree.c_str(), bn.c_str());
      });
    futs.push_back(task->get_future().share());
    rut_pool_submit([task]() { (*task)(); });
  }

  // Collect results (drain the Cling queue while waiting so main-thread
  // trampolining inside load_branch_impl doesn't deadlock).
  lval* result = lval_qexpr();
  for (int i = 0; i < nb; i++) {
    while (futs[i].wait_for(std::chrono::milliseconds(1)) ==
           std::future_status::timeout) {
      rut_drain_cling_queue();
    }
    RutColumnPtr col = futs[i].get();
    if (!col) {
      lval_del(result);
      return lval_err("load-branches: failed to load branch '%s'", bnames[i].c_str());
    }
    lval_add(result, lval_column(std::move(col)));
  }
  return result;
}

// ---------------------------------------------------------------------------
// Resolve a jit-fn symbol to a raw C function pointer via rut_calc.
// Dispatches to the main thread if called from a future.
// ---------------------------------------------------------------------------
static void* jitfn_ptr(lval* fn) {
  std::string expr = "(Long_t)&" + std::string(fn->sym);
  return (void*)rut_calc(expr.c_str());
}

// ---------------------------------------------------------------------------
// (jitfn-ptr jitfn) → Number  — resolve once on the main thread, reuse in futures
// ---------------------------------------------------------------------------
static lval* builtin_jitfn_ptr(lenv* /*e*/, lval* a) {
  LASSERT_NUM("jitfn-ptr", a, 1);
  LASSERT_TYPE("jitfn-ptr", a, 0, LVAL_JITFN);
  void* fp = jitfn_ptr(a->cell[0]);
  lval_del(a);
  return lval_num((long)fp);
}

// ---------------------------------------------------------------------------
// Shared column operation implementations — take a raw void* fp directly.
// These are called by both the jitfn variants (which resolve fp first) and
// the ptr variants (which receive fp as a pre-resolved LVAL_NUM).
// ---------------------------------------------------------------------------
// out_dtype == -1 means "same as input dtype"
static lval* col_map_impl(void* fp, RutColumnPtr& in_col, int out_dtype = -1) {
  size_t n      = in_col->n;
  int    idtype = in_col->dtype;
  int    odtype = (out_dtype < 0) ? idtype : out_dtype;
  size_t oesz   = col_dtype_size(odtype);
  void*  out    = col_alloc(n ? n * oesz : oesz);
  if (!out) return lval_err("col-map: out of memory");

#define TYPED_MAP(Tin, Tout) do { \
    auto f = (Tout(*)(Tin))fp; \
    Tin*  in  = (Tin*)in_col->data; \
    Tout* out2 = (Tout*)out; \
    for (size_t i = 0; i < n; i++) out2[i] = f(in[i]); \
  } while (0)
#define CROSS_DISPATCH(Tin) \
    switch (odtype) { \
      case COL_FLOAT32: TYPED_MAP(Tin, float);    break; \
      case COL_FLOAT64: TYPED_MAP(Tin, double);   break; \
      case COL_INT64:   TYPED_MAP(Tin, int64_t);  break; \
      case COL_UINT64:  TYPED_MAP(Tin, uint64_t); break; \
      case COL_INT32:   TYPED_MAP(Tin, int32_t);  break; \
      case COL_UINT32:  TYPED_MAP(Tin, uint32_t); break; \
      case COL_INT16:   TYPED_MAP(Tin, int16_t);  break; \
      case COL_UINT16:  TYPED_MAP(Tin, uint16_t); break; \
      case COL_INT8:    TYPED_MAP(Tin, int8_t);   break; \
      case COL_UINT8:   TYPED_MAP(Tin, uint8_t);  break; \
      default: free(out); return lval_err("col-map: unsupported output dtype"); \
    }
  switch (idtype) {
    case COL_FLOAT32: CROSS_DISPATCH(float);    break;
    case COL_FLOAT64: CROSS_DISPATCH(double);   break;
    case COL_INT64:   CROSS_DISPATCH(int64_t);  break;
    case COL_UINT64:  CROSS_DISPATCH(uint64_t); break;
    case COL_INT32:   CROSS_DISPATCH(int32_t);  break;
    case COL_UINT32:  CROSS_DISPATCH(uint32_t); break;
    case COL_INT16:   CROSS_DISPATCH(int16_t);  break;
    case COL_UINT16:  CROSS_DISPATCH(uint16_t); break;
    case COL_INT8:    CROSS_DISPATCH(int8_t);   break;
    case COL_UINT8:   CROSS_DISPATCH(uint8_t);  break;
    case COL_BOOL:    CROSS_DISPATCH(char);      break;
    default: free(out); return lval_err("col-map: unsupported input dtype");
  }
#undef CROSS_DISPATCH
#undef TYPED_MAP
  auto c = std::make_shared<RutColumn>();
  c->dtype = odtype; c->n = n; c->data = out;
  return lval_column(std::move(c));
}

static lval* col_filter_impl(void* fp, RutColumnPtr& in_col) {
  size_t n     = in_col->n;
  int    dtype = in_col->dtype;
  size_t esz   = col_dtype_size(dtype);
  void*  out   = col_alloc(n * esz);
  if (!out) return lval_err("col-filter: out of memory");
  size_t out_n = 0;

#define TYPED_FILTER(T) do { \
    auto f = (bool(*)(T))fp; \
    T* in  = (T*)in_col->data; \
    T* o   = (T*)out; \
    for (size_t i = 0; i < n; i++) if (f(in[i])) o[out_n++] = in[i]; \
  } while (0)
  switch (dtype) {
    case COL_FLOAT32: TYPED_FILTER(float);    break;
    case COL_FLOAT64: TYPED_FILTER(double);   break;
    case COL_INT64:   TYPED_FILTER(int64_t);  break;
    case COL_UINT64:  TYPED_FILTER(uint64_t); break;
    case COL_INT32:   TYPED_FILTER(int32_t);  break;
    case COL_UINT32:  TYPED_FILTER(uint32_t); break;
    case COL_INT16:   TYPED_FILTER(int16_t);  break;
    case COL_UINT16:  TYPED_FILTER(uint16_t); break;
    case COL_INT8:    TYPED_FILTER(int8_t);   break;
    case COL_UINT8:   TYPED_FILTER(uint8_t);  break;
    case COL_BOOL:    TYPED_FILTER(char);     break;
    default: free(out); return lval_err("col-filter: unsupported dtype");
  }
#undef TYPED_FILTER
  void* s = realloc(out, (out_n ? out_n : 1) * esz);
  if (s) out = s;
  auto c = std::make_shared<RutColumn>();
  c->dtype = dtype; c->n = out_n; c->data = out;
  return lval_column(std::move(c));
}

static lval* col_reduce_impl(void* fp, lval* init, RutColumnPtr& col) {
  size_t n     = col->n;
  int    dtype = col->dtype;
  lval*  result = nullptr;

#define TYPED_REDUCE(T) do { \
    auto f = (T(*)(T,T))fp; \
    T acc = (T)(init->type == LVAL_FLOAT ? init->floating : (double)init->num); \
    T* p  = (T*)col->data; \
    for (size_t i = 0; i < n; i++) acc = f(acc, p[i]); \
    result = (dtype == COL_FLOAT32 || dtype == COL_FLOAT64) \
             ? lval_floating((double)acc) : lval_num((long)acc); \
  } while (0)
  switch (dtype) {
    case COL_FLOAT32: TYPED_REDUCE(float);    break;
    case COL_FLOAT64: TYPED_REDUCE(double);   break;
    case COL_INT64:   TYPED_REDUCE(int64_t);  break;
    case COL_UINT64:  TYPED_REDUCE(uint64_t); break;
    case COL_INT32:   TYPED_REDUCE(int32_t);  break;
    case COL_UINT32:  TYPED_REDUCE(uint32_t); break;
    case COL_INT16:   TYPED_REDUCE(int16_t);  break;
    case COL_UINT16:  TYPED_REDUCE(uint16_t); break;
    case COL_INT8:    TYPED_REDUCE(int8_t);   break;
    case COL_UINT8:   TYPED_REDUCE(uint8_t);  break;
    case COL_BOOL:    TYPED_REDUCE(char);     break;
    default: return lval_err("col-reduce: unsupported dtype");
  }
#undef TYPED_REDUCE
  return result;
}

static lval* col_zip_impl(void* fp, int ncols, lval* a, int first_col_idx) {
  auto& c0  = col_of(a->cell[first_col_idx]);
  size_t n  = c0->n;
  int dtype = c0->dtype;
  size_t esz = col_dtype_size(dtype);
  void* out  = col_alloc(n * esz);
  if (!out) return lval_err("col-zip: out of memory");

#define ZIP2(T) do { \
    auto f=(T(*)(T,T))fp; T* pa=(T*)col_of(a->cell[first_col_idx])->data; \
    T* pb=(T*)col_of(a->cell[first_col_idx+1])->data; T* po=(T*)out; \
    for (size_t i=0;i<n;i++) po[i]=f(pa[i],pb[i]); } while(0)
#define ZIP3(T) do { \
    auto f=(T(*)(T,T,T))fp; T* pa=(T*)col_of(a->cell[first_col_idx])->data; \
    T* pb=(T*)col_of(a->cell[first_col_idx+1])->data; \
    T* pc=(T*)col_of(a->cell[first_col_idx+2])->data; T* po=(T*)out; \
    for (size_t i=0;i<n;i++) po[i]=f(pa[i],pb[i],pc[i]); } while(0)
#define ZIP4(T) do { \
    auto f=(T(*)(T,T,T,T))fp; T* pa=(T*)col_of(a->cell[first_col_idx])->data; \
    T* pb=(T*)col_of(a->cell[first_col_idx+1])->data; \
    T* pc=(T*)col_of(a->cell[first_col_idx+2])->data; \
    T* pd=(T*)col_of(a->cell[first_col_idx+3])->data; T* po=(T*)out; \
    for (size_t i=0;i<n;i++) po[i]=f(pa[i],pb[i],pc[i],pd[i]); } while(0)
#define DISPATCH_ZIP(T) switch(ncols){case 2:ZIP2(T);break;case 3:ZIP3(T);break;case 4:ZIP4(T);break;}
  switch (dtype) {
    case COL_FLOAT32: DISPATCH_ZIP(float);    break;
    case COL_FLOAT64: DISPATCH_ZIP(double);   break;
    case COL_INT64:   DISPATCH_ZIP(int64_t);  break;
    case COL_UINT64:  DISPATCH_ZIP(uint64_t); break;
    case COL_INT32:   DISPATCH_ZIP(int32_t);  break;
    case COL_UINT32:  DISPATCH_ZIP(uint32_t); break;
    case COL_INT16:   DISPATCH_ZIP(int16_t);  break;
    case COL_UINT16:  DISPATCH_ZIP(uint16_t); break;
    case COL_INT8:    DISPATCH_ZIP(int8_t);   break;
    case COL_UINT8:   DISPATCH_ZIP(uint8_t);  break;
    case COL_BOOL:    DISPATCH_ZIP(char);     break;
    default: free(out); return lval_err("col-zip: unsupported dtype");
  }
#undef ZIP2
#undef ZIP3
#undef ZIP4
#undef DISPATCH_ZIP
  auto c = std::make_shared<RutColumn>();
  c->dtype = dtype; c->n = n; c->data = out;
  return lval_column(std::move(c));
}

// ---------------------------------------------------------------------------
// (col-length col) → Number
// ---------------------------------------------------------------------------
static lval* builtin_col_length(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-length", a, 1);
  LASSERT_TYPE("col-length", a, 0, LVAL_COLUMN);
  size_t n = col_of(a->cell[0])->n;
  lval_del(a);
  return lval_num((long)n);
}

// ---------------------------------------------------------------------------
// (col-nrows col) → Number — alias for col-length, matches the pmap idiom
// ---------------------------------------------------------------------------
static lval* builtin_col_nrows(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-nrows", a, 1);
  LASSERT_TYPE("col-nrows", a, 0, LVAL_COLUMN);
  long n = (long)col_of(a->cell[0])->n;
  lval_del(a);
  return lval_num(n);
}

// ---------------------------------------------------------------------------
// (col-slice col start count) → Column — copy of rows [start, start+count).
// Used to split a column across workers for within-TF parallelism.
// ---------------------------------------------------------------------------
static lval* builtin_col_slice(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-slice", a, 3);
  LASSERT_TYPE("col-slice", a, 0, LVAL_COLUMN);
  LASSERT_TYPE("col-slice", a, 1, LVAL_NUM);
  LASSERT_TYPE("col-slice", a, 2, LVAL_NUM);
  auto& col   = col_of(a->cell[0]);
  size_t start = (size_t)a->cell[1]->num;
  size_t count = (size_t)a->cell[2]->num;
  lval_del(a);
  if (start >= col->n) count = 0;
  else if (start + count > col->n) count = col->n - start;
  size_t esz = col_dtype_size(col->dtype);
  void*  buf = col_alloc(count ? count * esz : esz);
  if (!buf) return lval_err("col-slice: out of memory");
  if (count) memcpy(buf, (char*)col->data + start * esz, count * esz);
  auto c = std::make_shared<RutColumn>();
  c->dtype = col->dtype; c->n = count; c->data = buf;
  return lval_column(std::move(c));
}

// ---------------------------------------------------------------------------
// (col-dtype col) → String — returns the C type name of the element type
// ---------------------------------------------------------------------------
static lval* builtin_col_dtype(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-dtype", a, 1);
  LASSERT_TYPE("col-dtype", a, 0, LVAL_COLUMN);
  const char* name = col_dtype_name(col_of(a->cell[0])->dtype);
  lval_del(a);
  return lval_str(name);
}

// ---------------------------------------------------------------------------
// (col-ref col i) → Number or Floating
// ---------------------------------------------------------------------------
static lval* builtin_col_ref(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-ref", a, 2);
  LASSERT_TYPE("col-ref", a, 0, LVAL_COLUMN);
  LASSERT_TYPE("col-ref", a, 1, LVAL_NUM);
  auto& col = col_of(a->cell[0]);
  long idx  = a->cell[1]->num;
  LASSERT(a, idx >= 0 && (size_t)idx < col->n,
          "col-ref: index %ld out of range (n=%zu)", idx, col->n);
  lval* r;
  switch (col->dtype) {
    case COL_FLOAT32: r = lval_floating(((float*)col->data)[idx]);         break;
    case COL_FLOAT64: r = lval_floating(((double*)col->data)[idx]);        break;
    case COL_INT64:   r = lval_num((long)((int64_t*)col->data)[idx]);      break;
    case COL_UINT64:  r = lval_num((long)((uint64_t*)col->data)[idx]);     break;
    case COL_INT32:   r = lval_num(((int32_t*)col->data)[idx]);            break;
    case COL_UINT32:  r = lval_num((long)((uint32_t*)col->data)[idx]);     break;
    case COL_INT16:   r = lval_num(((int16_t*)col->data)[idx]);            break;
    case COL_UINT16:  r = lval_num((long)((uint16_t*)col->data)[idx]);     break;
    case COL_INT8:    r = lval_num(((int8_t*)col->data)[idx]);             break;
    case COL_UINT8:   r = lval_num((long)((uint8_t*)col->data)[idx]);      break;
    case COL_BOOL:    r = lval_num(((char*)col->data)[idx] ? 1 : 0);       break;
    default:          r = lval_err("col-ref: unsupported dtype"); break;
  }
  lval_del(a);
  return r;
}

// ---------------------------------------------------------------------------
// (col->list col) → Q-expression
// Materialises up to 'limit' elements (default 1000) to avoid flooding the REPL.
// ---------------------------------------------------------------------------
static lval* builtin_col_to_list(lenv* /*e*/, lval* a) {
  LASSERT(a, a->count >= 1 && a->count <= 2,
          "'col->list' expects 1 or 2 arguments");
  LASSERT_TYPE("col->list", a, 0, LVAL_COLUMN);
  auto& col  = col_of(a->cell[0]);
  size_t lim = (a->count == 2 && a->cell[1]->type == LVAL_NUM)
               ? (size_t)a->cell[1]->num : col->n;
  if (lim > col->n) lim = col->n;
  lval* q = lval_qexpr();
  for (size_t i = 0; i < lim; i++) {
    lval* elem;
    switch (col->dtype) {
      case COL_FLOAT32: elem = lval_floating(((float*)col->data)[i]);      break;
      case COL_FLOAT64: elem = lval_floating(((double*)col->data)[i]);     break;
      case COL_INT64:   elem = lval_num((long)((int64_t*)col->data)[i]);   break;
      case COL_UINT64:  elem = lval_num((long)((uint64_t*)col->data)[i]);  break;
      case COL_INT32:   elem = lval_num(((int32_t*)col->data)[i]);         break;
      case COL_UINT32:  elem = lval_num((long)((uint32_t*)col->data)[i]);  break;
      case COL_INT16:   elem = lval_num(((int16_t*)col->data)[i]);         break;
      case COL_UINT16:  elem = lval_num((long)((uint16_t*)col->data)[i]);  break;
      case COL_INT8:    elem = lval_num(((int8_t*)col->data)[i]);          break;
      case COL_UINT8:   elem = lval_num((long)((uint8_t*)col->data)[i]);   break;
      case COL_BOOL:    elem = lval_num(((char*)col->data)[i] ? 1 : 0);    break;
      default:          elem = lval_num(0); break;
    }
    lval_add(q, elem);
  }
  lval_del(a);
  return q;
}

// ---------------------------------------------------------------------------
// col-map / col-map-ptr
// ---------------------------------------------------------------------------
static lval* builtin_col_map(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-map", a, 2);
  LASSERT_TYPE("col-map", a, 0, LVAL_JITFN);
  LASSERT_TYPE("col-map", a, 1, LVAL_COLUMN);
  LASSERT(a, a->cell[0]->num == 1,
          "'col-map' jit-fn must take 1 parameter, got %ld", a->cell[0]->num);
  void* fp = jitfn_ptr(a->cell[0]);
  auto& col = col_of(a->cell[1]);
  lval* r = col_map_impl(fp, col);
  lval_del(a);
  return r;
}
// (col-map-ptr fp col)          — output dtype same as input
// (col-map-ptr fp col "float")  — cross-type: output is float32 (or any named type)
static lval* builtin_col_map_ptr(lenv* /*e*/, lval* a) {
  LASSERT(a, a->count == 2 || a->count == 3,
          "'col-map-ptr' expects 2 or 3 arguments");
  LASSERT_TYPE("col-map-ptr", a, 0, LVAL_NUM);
  LASSERT_TYPE("col-map-ptr", a, 1, LVAL_COLUMN);
  void* fp = (void*)a->cell[0]->num;
  auto& col = col_of(a->cell[1]);
  int out_dtype = -1;
  if (a->count == 3) {
    LASSERT_TYPE("col-map-ptr", a, 2, LVAL_STR);
    out_dtype = col_dtype_from_name(a->cell[2]->str);
    LASSERT(a, out_dtype >= 0,
            "col-map-ptr: unknown output type '%s'", a->cell[2]->str);
  }
  lval* r = col_map_impl(fp, col, out_dtype);
  lval_del(a);
  return r;
}

// ---------------------------------------------------------------------------
// (col-test-bit col bit) → bool column
// Returns true (1) where (col[i] >> bit) & 1 is set, false (0) otherwise.
// bit < 0 means "skip check" — returns all true.
// col may be any integer dtype.
// ---------------------------------------------------------------------------
static lval* builtin_col_test_bit(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-test-bit", a, 2);
  LASSERT_TYPE("col-test-bit", a, 0, LVAL_COLUMN);
  LASSERT_TYPE("col-test-bit", a, 1, LVAL_NUM);
  auto& col  = col_of(a->cell[0]);
  int   bit  = (int)a->cell[1]->num;
  size_t n   = col->n;
  char* out  = (char*)col_alloc(n ? n : 1);
  if (!out) { lval_del(a); return lval_err("col-test-bit: out of memory"); }
  if (bit < 0) {
    memset(out, 1, n);
    lval_del(a);
    auto c = std::make_shared<RutColumn>();
    c->dtype = COL_BOOL; c->n = n; c->data = out;
    return lval_column(std::move(c));
  }
#define TEST_BIT(T) do { \
    T* p = (T*)col->data; \
    for (size_t i = 0; i < n; i++) out[i] = ((p[i] >> bit) & (T)1) ? 1 : 0; \
  } while (0)
  switch (col->dtype) {
    case COL_UINT64:  TEST_BIT(uint64_t); break;
    case COL_INT64:   TEST_BIT(int64_t);  break;
    case COL_UINT32:  TEST_BIT(uint32_t); break;
    case COL_INT32:   TEST_BIT(int32_t);  break;
    case COL_UINT16:  TEST_BIT(uint16_t); break;
    case COL_INT16:   TEST_BIT(int16_t);  break;
    case COL_UINT8:   TEST_BIT(uint8_t);  break;
    case COL_INT8:    TEST_BIT(int8_t);   break;
    default:
      free(out); lval_del(a);
      return lval_err("col-test-bit: column must be an integer dtype");
  }
#undef TEST_BIT
  lval_del(a);
  auto c = std::make_shared<RutColumn>();
  c->dtype = COL_BOOL; c->n = n; c->data = out;
  return lval_column(std::move(c));
}


// ---------------------------------------------------------------------------
// col-unique-mask
// (col-unique-mask col) → COL_BOOL
// Returns true for each element that appears exactly once in col.
// Useful for kNoSameBunchPileup: (col-unique-mask fIndexBCs) keeps only
// collisions whose BC index is not shared with another collision.
// Supports any integer dtype (int32, uint32, int64, uint64, …).
// ---------------------------------------------------------------------------
static lval* builtin_col_unique_mask(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-unique-mask", a, 1);
  LASSERT_TYPE("col-unique-mask", a, 0, LVAL_COLUMN);
  auto col = *static_cast<RutColumnPtr*>(a->cell[0]->obj);
  size_t n = col->n;

  uint8_t* out = (uint8_t*)malloc(n);
  if (!out) { lval_del(a); return lval_err("col-unique-mask: out of memory"); }

  std::unordered_map<uint64_t, uint32_t> freq;
  freq.reserve(n);

#define COUNT_FREQ(T) do { \
  const T* d = (const T*)col->data; \
  for (size_t i = 0; i < n; i++) freq[(uint64_t)(d[i])]++; \
  for (size_t i = 0; i < n; i++) out[i] = (freq[(uint64_t)(d[i])] == 1) ? 1 : 0; \
} while(0)

  switch (col->dtype) {
    case COL_UINT64: COUNT_FREQ(uint64_t); break;
    case COL_INT64:  COUNT_FREQ(int64_t);  break;
    case COL_UINT32: COUNT_FREQ(uint32_t); break;
    case COL_INT32:  COUNT_FREQ(int32_t);  break;
    case COL_UINT16: COUNT_FREQ(uint16_t); break;
    case COL_INT16:  COUNT_FREQ(int16_t);  break;
    case COL_UINT8:  COUNT_FREQ(uint8_t);  break;
    case COL_INT8:   COUNT_FREQ(int8_t);   break;
    default:
      free(out); lval_del(a);
      return lval_err("col-unique-mask: column must be an integer dtype");
  }
#undef COUNT_FREQ

  lval_del(a);
  auto c = std::make_shared<RutColumn>();
  c->dtype = COL_BOOL; c->n = n; c->data = out;
  return lval_column(std::move(c));
}

// ---------------------------------------------------------------------------
// col-filter / col-filter-ptr
// ---------------------------------------------------------------------------
static lval* builtin_col_filter(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-filter", a, 2);
  LASSERT_TYPE("col-filter", a, 0, LVAL_JITFN);
  LASSERT_TYPE("col-filter", a, 1, LVAL_COLUMN);
  LASSERT(a, a->cell[0]->num == 1,
          "'col-filter' jit-fn must take 1 parameter, got %ld", a->cell[0]->num);
  void* fp = jitfn_ptr(a->cell[0]);
  auto& col = col_of(a->cell[1]);
  lval* r = col_filter_impl(fp, col);
  lval_del(a);
  return r;
}
static lval* builtin_col_filter_ptr(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-filter-ptr", a, 2);
  LASSERT_TYPE("col-filter-ptr", a, 0, LVAL_NUM);
  LASSERT_TYPE("col-filter-ptr", a, 1, LVAL_COLUMN);
  void* fp = (void*)a->cell[0]->num;
  auto& col = col_of(a->cell[1]);
  lval* r = col_filter_impl(fp, col);
  lval_del(a);
  return r;
}

// ---------------------------------------------------------------------------
// col-reduce / col-reduce-ptr
// ---------------------------------------------------------------------------
static lval* builtin_col_reduce(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-reduce", a, 3);
  LASSERT_TYPE("col-reduce", a, 0, LVAL_JITFN);
  LASSERT(a, a->cell[1]->type == LVAL_NUM || a->cell[1]->type == LVAL_FLOAT,
          "'col-reduce' init must be a number");
  LASSERT_TYPE("col-reduce", a, 2, LVAL_COLUMN);
  LASSERT(a, a->cell[0]->num == 2,
          "'col-reduce' jit-fn must take 2 parameters, got %ld", a->cell[0]->num);
  void* fp = jitfn_ptr(a->cell[0]);
  auto& col = col_of(a->cell[2]);
  lval* r = col_reduce_impl(fp, a->cell[1], col);
  lval_del(a);
  return r;
}
static lval* builtin_col_reduce_ptr(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-reduce-ptr", a, 3);
  LASSERT_TYPE("col-reduce-ptr", a, 0, LVAL_NUM);
  LASSERT(a, a->cell[1]->type == LVAL_NUM || a->cell[1]->type == LVAL_FLOAT,
          "'col-reduce-ptr' init must be a number");
  LASSERT_TYPE("col-reduce-ptr", a, 2, LVAL_COLUMN);
  void* fp = (void*)a->cell[0]->num;
  auto& col = col_of(a->cell[2]);
  lval* r = col_reduce_impl(fp, a->cell[1], col);
  lval_del(a);
  return r;
}

// ---------------------------------------------------------------------------
// col-zip / col-zip-ptr
// ---------------------------------------------------------------------------
static lval* builtin_col_zip(lenv* /*e*/, lval* a) {
  LASSERT(a, a->count >= 3 && a->count <= 5,
          "'col-zip' expects 3–5 arguments (jitfn + 2–4 columns), got %i", a->count);
  LASSERT_TYPE("col-zip", a, 0, LVAL_JITFN);
  int ncols = a->count - 1;
  for (int i = 1; i <= ncols; i++)
    LASSERT(a, a->cell[i]->type == LVAL_COLUMN,
            "'col-zip' argument %d must be a Column", i);
  LASSERT(a, a->cell[0]->num == ncols,
          "'col-zip' jit-fn takes %ld params but %d columns given",
          a->cell[0]->num, ncols);
  for (int i = 2; i <= ncols; i++)
    LASSERT(a, col_of(a->cell[i])->n == col_of(a->cell[1])->n,
            "'col-zip' all columns must have the same length");
  void* fp = jitfn_ptr(a->cell[0]);
  lval* r = col_zip_impl(fp, ncols, a, 1);
  lval_del(a);
  return r;
}
static lval* builtin_col_zip_ptr(lenv* /*e*/, lval* a) {
  LASSERT(a, a->count >= 3 && a->count <= 5,
          "'col-zip-ptr' expects 3–5 arguments (ptr + 2–4 columns), got %i", a->count);
  LASSERT_TYPE("col-zip-ptr", a, 0, LVAL_NUM);
  int ncols = a->count - 1;
  for (int i = 1; i <= ncols; i++)
    LASSERT(a, a->cell[i]->type == LVAL_COLUMN,
            "'col-zip-ptr' argument %d must be a Column", i);
  for (int i = 2; i <= ncols; i++)
    LASSERT(a, col_of(a->cell[i])->n == col_of(a->cell[1])->n,
            "'col-zip-ptr' all columns must have the same length");
  void* fp = (void*)a->cell[0]->num;
  lval* r = col_zip_impl(fp, ncols, a, 1);
  lval_del(a);
  return r;
}

// ---------------------------------------------------------------------------
// (col-group-sum val-col mask-col idx-col) → float32 column
//
// Scatter-add: returns a float32 column of length max(idx)+1 where
//   out[g] = Σ_{i : mask[i]>0.5 ∧ idx[i]==g} val[i]
//
// val-col  — float32, track-level values
// mask-col — float32, track-level mask (non-zero = include)
// idx-col  — int32,   group index per track (e.g. fIndexCollisions)
//
// Negative idx entries are skipped (O2 sentinel for ambiguous collision).
// ---------------------------------------------------------------------------
static lval* builtin_col_group_sum(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-group-sum", a, 3);
  LASSERT_TYPE("col-group-sum", a, 0, LVAL_COLUMN);
  LASSERT_TYPE("col-group-sum", a, 1, LVAL_COLUMN);
  LASSERT_TYPE("col-group-sum", a, 2, LVAL_COLUMN);

  auto& vals = col_of(a->cell[0]);
  auto& mask = col_of(a->cell[1]);
  auto& idxs = col_of(a->cell[2]);

  LASSERT(a, vals->dtype == COL_FLOAT32,
          "col-group-sum: val column must be float32");
  LASSERT(a, mask->dtype == COL_FLOAT32,
          "col-group-sum: mask column must be float32");
  LASSERT(a, idxs->dtype == COL_INT32,
          "col-group-sum: idx column must be int32");
  LASSERT(a, vals->n == mask->n && vals->n == idxs->n,
          "col-group-sum: column length mismatch (%zu / %zu / %zu)",
          vals->n, mask->n, idxs->n);

  size_t   n = vals->n;
  float*   v = (float*)vals->data;
  float*   m = (float*)mask->data;
  int32_t* g = (int32_t*)idxs->data;

  int32_t gmax = -1;
  for (size_t i = 0; i < n; i++)
    if (g[i] > gmax) gmax = g[i];

  size_t ng  = (gmax < 0) ? 0 : (size_t)gmax + 1;
  size_t esz = ng ? ng * sizeof(float) : sizeof(float);
  float* out = (float*)col_alloc(esz);
  if (!out) { lval_del(a); return lval_err("col-group-sum: out of memory"); }
  for (size_t i = 0; i < ng; i++) out[i] = 0.0f;

  for (size_t i = 0; i < n; i++)
    if (m[i] > 0.5f && g[i] >= 0)
      out[(size_t)g[i]] += v[i];

  lval_del(a);
  auto c = std::make_shared<RutColumn>();
  c->dtype = COL_FLOAT32; c->n = ng; c->data = out;
  return lval_column(std::move(c));
}

// ---------------------------------------------------------------------------
// (col-fill-h1 hist col) → nil
// Fills a TH1 histogram directly from a column buffer — no interpreter
// overhead per element.  Safe to call from a future thread provided the
// histogram is not shared with any other thread.
// ---------------------------------------------------------------------------
static lval* builtin_col_fill_h1(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-fill-h1", a, 2);
  LASSERT_TYPE("col-fill-h1", a, 0, LVAL_TOBJ);
  LASSERT_TYPE("col-fill-h1", a, 1, LVAL_COLUMN);

  TH1*   h   = (TH1*)a->cell[0]->obj;
  auto&  col = col_of(a->cell[1]);
  size_t n   = col->n;

#define FILL_LOOP(T) do { \
    T* p = (T*)col->data; \
    for (size_t i = 0; i < n; i++) h->Fill((double)p[i]); \
  } while (0)

  switch (col->dtype) {
    case COL_FLOAT32: FILL_LOOP(float);    break;
    case COL_FLOAT64: FILL_LOOP(double);   break;
    case COL_INT64:   FILL_LOOP(int64_t);  break;
    case COL_UINT64:  FILL_LOOP(uint64_t); break;
    case COL_INT32:   FILL_LOOP(int32_t);  break;
    case COL_UINT32:  FILL_LOOP(uint32_t); break;
    case COL_INT16:   FILL_LOOP(int16_t);  break;
    case COL_UINT16:  FILL_LOOP(uint16_t); break;
    case COL_INT8:    FILL_LOOP(int8_t);   break;
    case COL_UINT8:   FILL_LOOP(uint8_t);  break;
    case COL_BOOL:    FILL_LOOP(char);     break;
    default:
      lval_del(a);
      return lval_err("col-fill-h1: unsupported dtype");
  }
#undef FILL_LOOP

  lval_del(a);
  return lval_qexpr();
}

// ---------------------------------------------------------------------------
// (col-fill-mean-h1 hist values-col group-col) → nil
// Computes the per-group mean of values-col (grouped by group-col, which must
// be COL_INT32) and fills hist with one entry per group that has at least one
// element.  Entries with group index < 0 are skipped (O2 sentinel for
// ambiguous/no collision).  values-col must be COL_FLOAT32.
// ---------------------------------------------------------------------------
static lval* builtin_col_fill_mean_h1(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-fill-mean-h1", a, 3);
  LASSERT_TYPE("col-fill-mean-h1", a, 0, LVAL_TOBJ);
  LASSERT_TYPE("col-fill-mean-h1", a, 1, LVAL_COLUMN);
  LASSERT_TYPE("col-fill-mean-h1", a, 2, LVAL_COLUMN);

  TH1*  h    = (TH1*)a->cell[0]->obj;
  auto& vals = col_of(a->cell[1]);
  auto& grps = col_of(a->cell[2]);

  LASSERT(a, vals->dtype == COL_FLOAT32,
          "col-fill-mean-h1: values column must be float32");
  LASSERT(a, grps->dtype == COL_INT32,
          "col-fill-mean-h1: group column must be int32");
  LASSERT(a, vals->n == grps->n,
          "col-fill-mean-h1: columns have different lengths (%zu vs %zu)",
          vals->n, grps->n);

  size_t   n  = vals->n;
  float*   v  = (float*)vals->data;
  int32_t* g  = (int32_t*)grps->data;

  // Find max valid group index to size accumulators.
  int32_t gmax = -1;
  for (size_t i = 0; i < n; i++)
    if (g[i] > gmax) gmax = g[i];

  if (gmax < 0) { lval_del(a); return lval_qexpr(); }  // all sentinel

  size_t ng = (size_t)gmax + 1;
  std::vector<double>  sum(ng, 0.0);
  std::vector<int32_t> cnt(ng, 0);

  for (size_t i = 0; i < n; i++) {
    if (g[i] < 0) continue;
    sum[(size_t)g[i]] += (double)v[i];
    cnt[(size_t)g[i]]++;
  }

  for (size_t j = 0; j < ng; j++)
    if (cnt[j] > 0) h->Fill(sum[j] / cnt[j]);

  lval_del(a);
  return lval_qexpr();
}

// ---------------------------------------------------------------------------
// (col-fill-h2 hist col-x col-y) → nil
// Fills a TH2 from two float32 columns element-wise.
// ---------------------------------------------------------------------------
static lval* builtin_col_fill_h2(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-fill-h2", a, 3);
  LASSERT_TYPE("col-fill-h2", a, 0, LVAL_TOBJ);
  LASSERT_TYPE("col-fill-h2", a, 1, LVAL_COLUMN);
  LASSERT_TYPE("col-fill-h2", a, 2, LVAL_COLUMN);
  TH2*  h  = (TH2*)a->cell[0]->obj;
  auto& cx = col_of(a->cell[1]);
  auto& cy = col_of(a->cell[2]);
  LASSERT(a, cx->n == cy->n,
          "col-fill-h2: columns have different lengths (%zu vs %zu)", cx->n, cy->n);
  LASSERT(a, cx->dtype == COL_FLOAT32, "col-fill-h2: x column must be float32");
  LASSERT(a, cy->dtype == COL_FLOAT32, "col-fill-h2: y column must be float32");
  float* px = (float*)cx->data;
  float* py = (float*)cy->data;
  size_t n  = cx->n;
  for (size_t i = 0; i < n; i++) h->Fill((double)px[i], (double)py[i]);
  lval_del(a);
  return lval_qexpr();
}

// ---------------------------------------------------------------------------
// (col-cast-f32 col) → float32 column
// Widens any numeric column to float32.  No-op copy if already float32.
// ---------------------------------------------------------------------------
static lval* builtin_col_cast_f32(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-cast-f32", a, 1);
  LASSERT_TYPE("col-cast-f32", a, 0, LVAL_COLUMN);
  auto& col = col_of(a->cell[0]);
  size_t n   = col->n;
  float* out = (float*)col_alloc(n ? n * sizeof(float) : sizeof(float));
  if (!out) { lval_del(a); return lval_err("col-cast-f32: out of memory"); }
#define CAST32(T) do { T* p=(T*)col->data; for(size_t i=0;i<n;i++) out[i]=(float)p[i]; } while(0)
  switch (col->dtype) {
    case COL_FLOAT32: CAST32(float);    break;
    case COL_FLOAT64: CAST32(double);   break;
    case COL_INT64:   CAST32(int64_t);  break;
    case COL_UINT64:  CAST32(uint64_t); break;
    case COL_INT32:   CAST32(int32_t);  break;
    case COL_UINT32:  CAST32(uint32_t); break;
    case COL_INT16:   CAST32(int16_t);  break;
    case COL_UINT16:  CAST32(uint16_t); break;
    case COL_INT8:    CAST32(int8_t);   break;
    case COL_UINT8:   CAST32(uint8_t);  break;
    case COL_BOOL:    CAST32(char);     break;
    default: free(out); lval_del(a); return lval_err("col-cast-f32: unsupported dtype");
  }
#undef CAST32
  lval_del(a);
  auto c = std::make_shared<RutColumn>();
  c->dtype = COL_FLOAT32; c->n = n; c->data = out;
  return lval_column(std::move(c));
}

// ---------------------------------------------------------------------------
// (col-mask mask-col data-col) → filtered data-col
// mask-col: float32 — non-zero entries are kept.
// data-col: float32 — must be same length as mask-col.
// ---------------------------------------------------------------------------
static lval* builtin_col_mask(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-mask", a, 2);
  LASSERT_TYPE("col-mask", a, 0, LVAL_COLUMN);
  LASSERT_TYPE("col-mask", a, 1, LVAL_COLUMN);
  auto& mask = col_of(a->cell[0]);
  auto& data = col_of(a->cell[1]);
  LASSERT(a, mask->n == data->n,
          "col-mask: columns have different lengths (%zu vs %zu)", mask->n, data->n);
  LASSERT(a, mask->dtype == COL_FLOAT32, "col-mask: mask column must be float32");
  LASSERT(a, data->dtype == COL_FLOAT32, "col-mask: data column must be float32");
  size_t n   = mask->n;
  float* m   = (float*)mask->data;
  float* d   = (float*)data->data;
  float* out = (float*)col_alloc(n ? n * sizeof(float) : sizeof(float));
  if (!out) { lval_del(a); return lval_err("col-mask: out of memory"); }
  size_t out_n = 0;
  for (size_t i = 0; i < n; i++) if (m[i] != 0.f) out[out_n++] = d[i];
  float* s = (float*)realloc(out, (out_n ? out_n : 1) * sizeof(float));
  if (s) out = s;
  lval_del(a);
  auto c = std::make_shared<RutColumn>();
  c->dtype = COL_FLOAT32; c->n = out_n; c->data = out;
  return lval_column(std::move(c));
}

// ---------------------------------------------------------------------------
// (col-cat {col1 col2 ...}) → concatenated column
// All columns must have the same dtype.
// ---------------------------------------------------------------------------
static lval* builtin_col_cat(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-cat", a, 1);
  LASSERT_TYPE("col-cat", a, 0, LVAL_QEXPR);
  lval* list = a->cell[0];
  LASSERT(a, list->count > 0, "col-cat: empty list");
  LASSERT(a, list->cell[0]->type == LVAL_COLUMN, "col-cat: element 0 is not a column");
  int    dtype = col_of(list->cell[0])->dtype;
  size_t esz   = col_dtype_size(dtype);
  size_t total = 0;
  for (int i = 0; i < list->count; i++) {
    LASSERT(a, list->cell[i]->type == LVAL_COLUMN,
            "col-cat: element %d is not a column", i);
    LASSERT(a, col_of(list->cell[i])->dtype == dtype,
            "col-cat: dtype mismatch at element %d", i);
    total += col_of(list->cell[i])->n;
  }
  void* out = col_alloc(total ? total * esz : esz);
  if (!out) { lval_del(a); return lval_err("col-cat: out of memory"); }
  size_t off = 0;
  for (int i = 0; i < list->count; i++) {
    auto& ci = col_of(list->cell[i]);
    memcpy((char*)out + off * esz, ci->data, ci->n * esz);
    off += ci->n;
  }
  lval_del(a);
  auto c = std::make_shared<RutColumn>();
  c->dtype = dtype; c->n = total; c->data = out;
  return lval_column(std::move(c));
}

// ---------------------------------------------------------------------------
// V0 geometry helper — helix propagation in uniform Bz field.
//
// Track parameters follow the ALICE/O2 convention:
//   alpha  — rotation angle of the local frame around z
//   xl, yl — track position in local frame (xl ≈ radius, yl ≈ transverse)
//   zl     — z coordinate (same in local and global)
//   snp    — sin(track azimuth in local frame)
//   tgl    — tan(dip angle) = pz/pT
//   s1pt   — signed 1/pT = charge/pT  [(GeV/c)⁻¹]
//   bz     — solenoid field along z [T]
//
// Unit conventions: distances in cm, pT in GeV/c, B in T.
//   crv [1/cm] = s1pt * bz * 0.003
//   R   [cm]   = 1/|crv|
//
// Helix center (corrected sign convention, verified for ALICE +/- polarity):
//   cx = xg + sin(φ)/crv,   cy = yg − cos(φ)/crv
// where φ = alpha + asin(snp) is the global azimuthal direction.
// ---------------------------------------------------------------------------
struct V0Geo { float vx, vy, vz, dca; };

static V0Geo compute_v0_geo(
    float s1pt_p, float alpha_p, float snp_p, float tgl_p, float xl_p, float yl_p, float zl_p,
    float s1pt_n, float alpha_n, float snp_n, float tgl_n, float xl_n, float yl_n, float zl_n,
    float bz)
{
  static constexpr float kB2C = 0.003f;
  static constexpr float kPi  = 3.14159265f;

  // ── signed curvature [1/cm] ─────────────────────────────────────────────
  float crv_p = s1pt_p * bz * kB2C;
  float crv_n = s1pt_n * bz * kB2C;
  if (std::abs(crv_p) < 1e-9f) crv_p = (crv_p >= 0.f ? 1e-9f : -1e-9f);
  if (std::abs(crv_n) < 1e-9f) crv_n = (crv_n >= 0.f ? 1e-9f : -1e-9f);
  float R_p = 1.f / std::abs(crv_p);
  float R_n = 1.f / std::abs(crv_n);

  // ── local → global XY ───────────────────────────────────────────────────
  float ca_p = std::cos(alpha_p), sa_p = std::sin(alpha_p);
  float ca_n = std::cos(alpha_n), sa_n = std::sin(alpha_n);
  float xg_p = xl_p*ca_p - yl_p*sa_p,  yg_p = xl_p*sa_p + yl_p*ca_p;
  float xg_n = xl_n*ca_n - yl_n*sa_n,  yg_n = xl_n*sa_n + yl_n*ca_n;

  // ── global track direction (φ = alpha + asin(snp)) ──────────────────────
  float c1_p  = std::sqrt(std::max(0.f, 1.f - snp_p*snp_p));
  float c1_n  = std::sqrt(std::max(0.f, 1.f - snp_n*snp_n));
  float cph_p = ca_p*c1_p - sa_p*snp_p,  sph_p = sa_p*c1_p + ca_p*snp_p;
  float cph_n = ca_n*c1_n - sa_n*snp_n,  sph_n = sa_n*c1_n + ca_n*snp_n;

  // ── helix centers: cx = xg + sinφ/crv,  cy = yg − cosφ/crv ─────────────
  float cx_p = xg_p + sph_p/crv_p,  cy_p = yg_p - cph_p/crv_p;
  float cx_n = xg_n + sph_n/crv_n,  cy_n = yg_n - cph_n/crv_n;

  // ── circle-circle intersection → decay vertex XY ────────────────────────
  float dcx = cx_n - cx_p,  dcy = cy_n - cy_p;
  float d2  = dcx*dcx + dcy*dcy;
  float d   = std::sqrt(d2);

  float vx, vy;
  if (d < 1e-6f) {
    vx = 0.5f*(xg_p + xg_n);
    vy = 0.5f*(yg_p + yg_n);
  } else {
    float inv_d = 1.f / d;
    float t  = 0.5f * (d + (R_p*R_p - R_n*R_n) * inv_d);
    float h2 = R_p*R_p - t*t;
    float mx = cx_p + t*dcx*inv_d;
    float my = cy_p + t*dcy*inv_d;

    if (h2 <= 0.f) {
      // non-intersecting — midpoint between nearest points on each circle
      vx = 0.5f*((cx_p + R_p*dcx*inv_d) + (cx_n - R_n*dcx*inv_d));
      vy = 0.5f*((cy_p + R_p*dcy*inv_d) + (cy_n - R_n*dcy*inv_d));
    } else {
      float h   = std::sqrt(h2);
      float px1 = mx + h*(-dcy)*inv_d,  py1 = my + h*dcx*inv_d;
      float px2 = mx - h*(-dcy)*inv_d,  py2 = my - h*dcx*inv_d;
      // choose the intersection forward from the positive-charge daughter
      float dot1 = (px1-xg_p)*cph_p + (py1-yg_p)*sph_p;
      float dot2 = (px2-xg_p)*cph_p + (py2-yg_p)*sph_p;
      if (dot1 >= dot2) { vx = px1; vy = py1; }
      else              { vx = px2; vy = py2; }
    }
  }

  // ── z at vertex via arc-length propagation ───────────────────────────────
  // crv > 0 → clockwise  (theta decreasing forward)
  // crv < 0 → counterclockwise (theta increasing forward)
  auto z_at_vtx = [](float xg, float yg, float z_ref,
                     float tgl, float crv, float R,
                     float cx, float cy, float tvx, float tvy) -> float {
    constexpr float kPi = 3.14159265f;
    float dth = std::atan2(tvy - cy, tvx - cx) - std::atan2(yg - cy, xg - cx);
    if (crv < 0.f) {
      while (dth < 0.f) dth += 2.f*kPi;
      if (dth > kPi) dth -= 2.f*kPi;
    } else {
      while (dth > 0.f) dth -= 2.f*kPi;
      if (dth < -kPi) dth += 2.f*kPi;
    }
    return z_ref + R * std::abs(dth) * tgl;
  };

  float vz_p = z_at_vtx(xg_p, yg_p, zl_p, tgl_p, crv_p, R_p, cx_p, cy_p, vx, vy);
  float vz_n = z_at_vtx(xg_n, yg_n, zl_n, tgl_n, crv_n, R_n, cx_n, cy_n, vx, vy);
  float vz   = 0.5f*(vz_p + vz_n);

  // ── 3D DCA: distance between nearest points on each circle ──────────────
  auto nearest_xy = [](float pvx, float pvy, float cx, float cy, float R)
      -> std::pair<float,float> {
    float dx = pvx - cx,  dy = pvy - cy;
    float dd = std::sqrt(dx*dx + dy*dy);
    if (dd < 1e-8f) return {cx + R, cy};
    return {cx + R*dx/dd, cy + R*dy/dd};
  };

  auto [cpx_p, cpy_p] = nearest_xy(vx, vy, cx_p, cy_p, R_p);
  auto [cpx_n, cpy_n] = nearest_xy(vx, vy, cx_n, cy_n, R_n);
  float dxy = std::sqrt((cpx_p-cpx_n)*(cpx_p-cpx_n) + (cpy_p-cpy_n)*(cpy_p-cpy_n));
  float dca = std::sqrt(dxy*dxy + (vz_p-vz_n)*(vz_p-vz_n));

  return {vx, vy, vz, dca};
}

// ---------------------------------------------------------------------------
// (col-dca-v0 s1pt-p alpha-p snp-p tgl-p x-p y-p z-p
//             s1pt-n alpha-n snp-n tgl-n x-n y-n z-n
//             bz)
// → {vtx-x vtx-y vtx-z dca-daughters}   Q-expr of 4 float32 columns
//
// All 14 track-parameter columns must be float32 (same length).
// bz is the solenoid field in Tesla (signed; typically ±0.5 for ALICE Run 3).
// Distances are in cm.
// ---------------------------------------------------------------------------
static lval* builtin_col_dca_v0(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-dca-v0", a, 15);
  for (int i = 0; i < 14; i++) {
    LASSERT_TYPE("col-dca-v0", a, i, LVAL_COLUMN);
    LASSERT(a, col_of(a->cell[i])->dtype == COL_FLOAT32,
            "col-dca-v0: column %d must be float32", i);
  }
  LASSERT(a, a->cell[14]->type == LVAL_NUM || a->cell[14]->type == LVAL_FLOAT,
          "col-dca-v0: bz must be a number");

  size_t n = col_of(a->cell[0])->n;
  for (int i = 1; i < 14; i++)
    LASSERT(a, col_of(a->cell[i])->n == n,
            "col-dca-v0: column %d length mismatch (%zu vs %zu)",
            i, col_of(a->cell[i])->n, n);

  float bz = (a->cell[14]->type == LVAL_FLOAT) ? (float)a->cell[14]->floating
                                                 : (float)a->cell[14]->num;
  float* s1pt_p  = (float*)col_of(a->cell[0])->data;
  float* alpha_p = (float*)col_of(a->cell[1])->data;
  float* snp_p   = (float*)col_of(a->cell[2])->data;
  float* tgl_p   = (float*)col_of(a->cell[3])->data;
  float* x_p     = (float*)col_of(a->cell[4])->data;
  float* y_p     = (float*)col_of(a->cell[5])->data;
  float* z_p     = (float*)col_of(a->cell[6])->data;
  float* s1pt_n  = (float*)col_of(a->cell[7])->data;
  float* alpha_n = (float*)col_of(a->cell[8])->data;
  float* snp_n   = (float*)col_of(a->cell[9])->data;
  float* tgl_n   = (float*)col_of(a->cell[10])->data;
  float* x_n     = (float*)col_of(a->cell[11])->data;
  float* y_n     = (float*)col_of(a->cell[12])->data;
  float* z_n     = (float*)col_of(a->cell[13])->data;

  float* out_vx  = (float*)col_alloc((n ? n : 1) * sizeof(float));
  float* out_vy  = (float*)col_alloc((n ? n : 1) * sizeof(float));
  float* out_vz  = (float*)col_alloc((n ? n : 1) * sizeof(float));
  float* out_dca = (float*)col_alloc((n ? n : 1) * sizeof(float));
  if (!out_vx || !out_vy || !out_vz || !out_dca) {
    free(out_vx); free(out_vy); free(out_vz); free(out_dca);
    lval_del(a);
    return lval_err("col-dca-v0: out of memory");
  }

  for (size_t i = 0; i < n; i++) {
    V0Geo g = compute_v0_geo(
        s1pt_p[i], alpha_p[i], snp_p[i], tgl_p[i], x_p[i], y_p[i], z_p[i],
        s1pt_n[i], alpha_n[i], snp_n[i], tgl_n[i], x_n[i], y_n[i], z_n[i], bz);
    out_vx[i] = g.vx; out_vy[i] = g.vy; out_vz[i] = g.vz; out_dca[i] = g.dca;
  }
  lval_del(a);

  auto make_col32 = [](float* data, size_t nn) {
    auto c = std::make_shared<RutColumn>();
    c->dtype = COL_FLOAT32; c->n = nn; c->data = data;
    return lval_column(std::move(c));
  };
  lval* result = lval_qexpr();
  lval_add(result, make_col32(out_vx,  n));
  lval_add(result, make_col32(out_vy,  n));
  lval_add(result, make_col32(out_vz,  n));
  lval_add(result, make_col32(out_dca, n));
  return result;
}

// ---------------------------------------------------------------------------
// (col-cpa vtx-x vtx-y vtx-z pv-x pv-y pv-z px py pz) → float32
// Cosine of the V0 pointing angle:
//   cpa = (vtx−pv)·p / (|vtx−pv| × |p|)
// All 9 columns must be float32 and the same length.
// ---------------------------------------------------------------------------
static lval* builtin_col_cpa(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-cpa", a, 9);
  for (int i = 0; i < 9; i++) {
    LASSERT_TYPE("col-cpa", a, i, LVAL_COLUMN);
    LASSERT(a, col_of(a->cell[i])->dtype == COL_FLOAT32,
            "col-cpa: column %d must be float32", i);
  }
  size_t n = col_of(a->cell[0])->n;
  for (int i = 1; i < 9; i++)
    LASSERT(a, col_of(a->cell[i])->n == n,
            "col-cpa: column %d length mismatch", i);

  float* vx  = (float*)col_of(a->cell[0])->data;
  float* vy  = (float*)col_of(a->cell[1])->data;
  float* vz  = (float*)col_of(a->cell[2])->data;
  float* pvx = (float*)col_of(a->cell[3])->data;
  float* pvy = (float*)col_of(a->cell[4])->data;
  float* pvz = (float*)col_of(a->cell[5])->data;
  float* px  = (float*)col_of(a->cell[6])->data;
  float* py  = (float*)col_of(a->cell[7])->data;
  float* pz  = (float*)col_of(a->cell[8])->data;

  float* out = (float*)col_alloc((n ? n : 1) * sizeof(float));
  if (!out) { lval_del(a); return lval_err("col-cpa: out of memory"); }

  for (size_t i = 0; i < n; i++) {
    float dx = vx[i] - pvx[i], dy = vy[i] - pvy[i], dz = vz[i] - pvz[i];
    float r  = std::sqrt(dx*dx + dy*dy + dz*dz);
    float p  = std::sqrt(px[i]*px[i] + py[i]*py[i] + pz[i]*pz[i]);
    out[i] = (r < 1e-8f || p < 1e-8f) ? 0.f
           : (dx*px[i] + dy*py[i] + dz*pz[i]) / (r * p);
  }
  lval_del(a);
  auto c = std::make_shared<RutColumn>();
  c->dtype = COL_FLOAT32; c->n = n; c->data = out;
  return lval_column(std::move(c));
}

// ---------------------------------------------------------------------------
// (col-armenteros pos-px pos-py pos-pz neg-px neg-py neg-pz v0-px v0-py v0-pz)
//   → {alpha qt}  (Q-expr of two float32 columns)
//
// Armenteros-Podolanski variables:
//   pL_pos = (pos·v0_hat),   pL_neg = (neg·v0_hat)
//   alpha  = (pL_pos - pL_neg) / (pL_pos + pL_neg)
//   qT     = sqrt(|pos|² − pL_pos²)   [transverse momentum of pos daughter]
// ---------------------------------------------------------------------------
static lval* builtin_col_armenteros(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-armenteros", a, 9);
  for (int i = 0; i < 9; i++) {
    LASSERT_TYPE("col-armenteros", a, i, LVAL_COLUMN);
    LASSERT(a, col_of(a->cell[i])->dtype == COL_FLOAT32,
            "col-armenteros: column %d must be float32", i);
  }
  size_t n = col_of(a->cell[0])->n;
  for (int i = 1; i < 9; i++)
    LASSERT(a, col_of(a->cell[i])->n == n,
            "col-armenteros: column %d length mismatch", i);

  float* ppx = (float*)col_of(a->cell[0])->data;
  float* ppy = (float*)col_of(a->cell[1])->data;
  float* ppz = (float*)col_of(a->cell[2])->data;
  float* npx = (float*)col_of(a->cell[3])->data;
  float* npy = (float*)col_of(a->cell[4])->data;
  float* npz = (float*)col_of(a->cell[5])->data;
  float* vpx = (float*)col_of(a->cell[6])->data;
  float* vpy = (float*)col_of(a->cell[7])->data;
  float* vpz = (float*)col_of(a->cell[8])->data;

  size_t nbytes = (n ? n : 1) * sizeof(float);
  float* alpha = (float*)col_alloc(nbytes);
  float* qt    = (float*)col_alloc(nbytes);
  if (!alpha || !qt) {
    free(alpha); free(qt);
    lval_del(a);
    return lval_err("col-armenteros: out of memory");
  }

  for (size_t i = 0; i < n; i++) {
    float v2 = vpx[i]*vpx[i] + vpy[i]*vpy[i] + vpz[i]*vpz[i];
    float vp  = std::sqrt(v2 < 0.f ? 0.f : v2);
    if (vp < 1e-8f) { alpha[i] = 0.f; qt[i] = 0.f; continue; }
    float inv_vp = 1.f / vp;
    float pL_p = (ppx[i]*vpx[i] + ppy[i]*vpy[i] + ppz[i]*vpz[i]) * inv_vp;
    float pL_n = (npx[i]*vpx[i] + npy[i]*vpy[i] + npz[i]*vpz[i]) * inv_vp;
    float denom = pL_p + pL_n;
    alpha[i] = (std::abs(denom) < 1e-8f) ? 0.f : (pL_p - pL_n) / denom;
    float p2 = ppx[i]*ppx[i] + ppy[i]*ppy[i] + ppz[i]*ppz[i];
    float qt2 = p2 - pL_p*pL_p;
    qt[i] = std::sqrt(qt2 > 0.f ? qt2 : 0.f);
  }
  lval_del(a);

  auto ca = std::make_shared<RutColumn>(); ca->dtype = COL_FLOAT32; ca->n = n; ca->data = alpha;
  auto cq = std::make_shared<RutColumn>(); cq->dtype = COL_FLOAT32; cq->n = n; cq->data = qt;

  lval* res = lval_qexpr();
  lval_add(res, lval_column(std::move(ca)));
  lval_add(res, lval_column(std::move(cq)));
  return res;
}

// ---------------------------------------------------------------------------
// (col-gather data-col idx-col) → new column (same dtype as data-col)
// result[i] = data[idx[i]]  for i in 0..idx-col->n-1.
// idx-col must be COL_INT32.  Out-of-range or negative indices produce 0.
// ---------------------------------------------------------------------------
static lval* builtin_col_gather(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-gather", a, 2);
  LASSERT_TYPE("col-gather", a, 0, LVAL_COLUMN);
  LASSERT_TYPE("col-gather", a, 1, LVAL_COLUMN);
  auto& data = col_of(a->cell[0]);
  auto& idx  = col_of(a->cell[1]);
  LASSERT(a, idx->dtype == COL_INT32,
          "col-gather: index column must be int32, got %s",
          col_dtype_name(idx->dtype));

  int    dtype = data->dtype;
  size_t esz   = col_dtype_size(dtype);
  size_t n_out = idx->n;
  size_t n_dat = data->n;
  void*  out   = col_alloc(n_out ? n_out * esz : esz);
  if (!out) { lval_del(a); return lval_err("col-gather: out of memory"); }
  memset(out, 0, n_out * esz);

  int32_t* ix  = (int32_t*)idx->data;
  char*    dst = (char*)out;
  char*    src = (char*)data->data;
  for (size_t i = 0; i < n_out; i++) {
    int32_t j = ix[i];
    if (j >= 0 && (size_t)j < n_dat)
      memcpy(dst + i * esz, src + (size_t)j * esz, esz);
    // else: zero already (memset above)
  }

  lval_del(a);
  auto c = std::make_shared<RutColumn>();
  c->dtype = dtype; c->n = n_out; c->data = out;
  return lval_column(std::move(c));
}

// ---------------------------------------------------------------------------
// (col-fill-minv2-h1 hist px1 py1 pz1 px2 py2 pz2 m1 m2) → nil
// All 6 momentum columns must be float32 and the same length.
// m1 and m2 are mass hypotheses (numbers in GeV/c²).
// Fills hist with  sqrt((E1+E2)² - (px1+px2)² - (py1+py2)² - (pz1+pz2)²).
// ---------------------------------------------------------------------------
static lval* builtin_col_fill_minv2_h1(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-fill-minv2-h1", a, 9);
  LASSERT_TYPE("col-fill-minv2-h1", a, 0, LVAL_TOBJ);
  for (int i = 1; i <= 6; i++)
    LASSERT(a, a->cell[i]->type == LVAL_COLUMN,
            "col-fill-minv2-h1: argument %d must be a Column", i);
  LASSERT(a, a->cell[7]->type == LVAL_NUM || a->cell[7]->type == LVAL_FLOAT,
          "col-fill-minv2-h1: m1 must be a number");
  LASSERT(a, a->cell[8]->type == LVAL_NUM || a->cell[8]->type == LVAL_FLOAT,
          "col-fill-minv2-h1: m2 must be a number");

  TH1* h = (TH1*)a->cell[0]->obj;
  for (int i = 1; i <= 6; i++) {
    LASSERT(a, col_of(a->cell[i])->dtype == COL_FLOAT32,
            "col-fill-minv2-h1: column %d must be float32", i);
    if (i > 1)
      LASSERT(a, col_of(a->cell[i])->n == col_of(a->cell[1])->n,
              "col-fill-minv2-h1: column length mismatch at argument %d", i);
  }

  float* px1 = (float*)col_of(a->cell[1])->data;
  float* py1 = (float*)col_of(a->cell[2])->data;
  float* pz1 = (float*)col_of(a->cell[3])->data;
  float* px2 = (float*)col_of(a->cell[4])->data;
  float* py2 = (float*)col_of(a->cell[5])->data;
  float* pz2 = (float*)col_of(a->cell[6])->data;
  size_t n   = col_of(a->cell[1])->n;
  double m1  = (a->cell[7]->type == LVAL_FLOAT) ? a->cell[7]->floating : (double)a->cell[7]->num;
  double m2  = (a->cell[8]->type == LVAL_FLOAT) ? a->cell[8]->floating : (double)a->cell[8]->num;

  for (size_t i = 0; i < n; i++) {
    double p1sq = (double)px1[i]*(double)px1[i] + (double)py1[i]*(double)py1[i] + (double)pz1[i]*(double)pz1[i];
    double p2sq = (double)px2[i]*(double)px2[i] + (double)py2[i]*(double)py2[i] + (double)pz2[i]*(double)pz2[i];
    double e1   = std::sqrt(p1sq + m1*m1);
    double e2   = std::sqrt(p2sq + m2*m2);
    double dpx  = (double)px1[i] + (double)px2[i];
    double dpy  = (double)py1[i] + (double)py2[i];
    double dpz  = (double)pz1[i] + (double)pz2[i];
    double e    = e1 + e2;
    double msq  = e*e - dpx*dpx - dpy*dpy - dpz*dpz;
    if (msq >= 0.0) h->Fill(std::sqrt(msq));
  }

  lval_del(a);
  return lval_qexpr();
}

// ---------------------------------------------------------------------------
// (col-fill-minv3-h1 hist px1 py1 pz1 px2 py2 pz2 px3 py3 pz3 m1 m2 m3) → nil
// Same as col-fill-minv2-h1 but for 3 daughters.
// ---------------------------------------------------------------------------
static lval* builtin_col_fill_minv3_h1(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-fill-minv3-h1", a, 13);
  LASSERT_TYPE("col-fill-minv3-h1", a, 0, LVAL_TOBJ);
  for (int i = 1; i <= 9; i++)
    LASSERT(a, a->cell[i]->type == LVAL_COLUMN,
            "col-fill-minv3-h1: argument %d must be a Column", i);
  for (int i = 10; i <= 12; i++)
    LASSERT(a, a->cell[i]->type == LVAL_NUM || a->cell[i]->type == LVAL_FLOAT,
            "col-fill-minv3-h1: mass hypothesis %d must be a number", i-9);

  TH1* h = (TH1*)a->cell[0]->obj;
  for (int i = 1; i <= 9; i++) {
    LASSERT(a, col_of(a->cell[i])->dtype == COL_FLOAT32,
            "col-fill-minv3-h1: column %d must be float32", i);
    if (i > 1)
      LASSERT(a, col_of(a->cell[i])->n == col_of(a->cell[1])->n,
              "col-fill-minv3-h1: column length mismatch at argument %d", i);
  }

  float* px1 = (float*)col_of(a->cell[1])->data;
  float* py1 = (float*)col_of(a->cell[2])->data;
  float* pz1 = (float*)col_of(a->cell[3])->data;
  float* px2 = (float*)col_of(a->cell[4])->data;
  float* py2 = (float*)col_of(a->cell[5])->data;
  float* pz2 = (float*)col_of(a->cell[6])->data;
  float* px3 = (float*)col_of(a->cell[7])->data;
  float* py3 = (float*)col_of(a->cell[8])->data;
  float* pz3 = (float*)col_of(a->cell[9])->data;
  size_t n   = col_of(a->cell[1])->n;

  auto get_mass = [&](int idx) -> double {
    return (a->cell[idx]->type == LVAL_FLOAT) ? a->cell[idx]->floating : (double)a->cell[idx]->num;
  };
  double m1 = get_mass(10), m2 = get_mass(11), m3 = get_mass(12);

  for (size_t i = 0; i < n; i++) {
    double p1sq = (double)px1[i]*(double)px1[i] + (double)py1[i]*(double)py1[i] + (double)pz1[i]*(double)pz1[i];
    double p2sq = (double)px2[i]*(double)px2[i] + (double)py2[i]*(double)py2[i] + (double)pz2[i]*(double)pz2[i];
    double p3sq = (double)px3[i]*(double)px3[i] + (double)py3[i]*(double)py3[i] + (double)pz3[i]*(double)pz3[i];
    double e1   = std::sqrt(p1sq + m1*m1);
    double e2   = std::sqrt(p2sq + m2*m2);
    double e3   = std::sqrt(p3sq + m3*m3);
    double dpx  = (double)px1[i] + (double)px2[i] + (double)px3[i];
    double dpy  = (double)py1[i] + (double)py2[i] + (double)py3[i];
    double dpz  = (double)pz1[i] + (double)pz2[i] + (double)pz3[i];
    double e    = e1 + e2 + e3;
    double msq  = e*e - dpx*dpx - dpy*dpy - dpz*dpz;
    if (msq >= 0.0) h->Fill(std::sqrt(msq));
  }

  lval_del(a);
  return lval_qexpr();
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------
void lenv_add_builtins_column(lenv* e) {
  lenv_add_builtin(e, "load-branch",    builtin_load_branch);
  lenv_add_builtin(e, "load-branches",  builtin_load_branches);
  lenv_add_builtin(e, "close-cached-files", [](lenv*, lval* a) -> lval* {
    lval_del(a);
    rut_dispatch_work([]{ rut_file_cache_clear(); });
    return lval_sexpr();
  });
  lenv_add_builtin(e, "col-length",     builtin_col_length);
  lenv_add_builtin(e, "col-nrows",      builtin_col_nrows);
  lenv_add_builtin(e, "col-slice",      builtin_col_slice);
  lenv_add_builtin(e, "col-dtype",      builtin_col_dtype);
  lenv_add_builtin(e, "col-ref",        builtin_col_ref);
  lenv_add_builtin(e, "col->list",      builtin_col_to_list);
  lenv_add_builtin(e, "col-map",        builtin_col_map);
  lenv_add_builtin(e, "col-map-ptr",    builtin_col_map_ptr);
  lenv_add_builtin(e, "col-test-bit",   builtin_col_test_bit);
  lenv_add_builtin(e, "col-unique-mask", builtin_col_unique_mask);
  lenv_add_builtin(e, "col-filter",     builtin_col_filter);
  lenv_add_builtin(e, "col-filter-ptr", builtin_col_filter_ptr);
  lenv_add_builtin(e, "col-reduce",     builtin_col_reduce);
  lenv_add_builtin(e, "col-reduce-ptr", builtin_col_reduce_ptr);
  lenv_add_builtin(e, "col-zip",        builtin_col_zip);
  lenv_add_builtin(e, "col-zip-ptr",    builtin_col_zip_ptr);
  lenv_add_builtin(e, "col-group-sum",    builtin_col_group_sum);
  lenv_add_builtin(e, "col-fill-h1",      builtin_col_fill_h1);
  lenv_add_builtin(e, "col-fill-mean-h1", builtin_col_fill_mean_h1);
  lenv_add_builtin(e, "col-fill-h2",    builtin_col_fill_h2);
  lenv_add_builtin(e, "col-cast-f32",   builtin_col_cast_f32);
  lenv_add_builtin(e, "col-mask",       builtin_col_mask);
  lenv_add_builtin(e, "col-cat",           builtin_col_cat);
  lenv_add_builtin(e, "col-dca-v0",        builtin_col_dca_v0);
  lenv_add_builtin(e, "col-cpa",           builtin_col_cpa);
  lenv_add_builtin(e, "col-armenteros",    builtin_col_armenteros);
  lenv_add_builtin(e, "col-gather",        builtin_col_gather);
  lenv_add_builtin(e, "col-fill-minv2-h1", builtin_col_fill_minv2_h1);
  lenv_add_builtin(e, "col-fill-minv3-h1", builtin_col_fill_minv3_h1);
  lenv_add_builtin(e, "jitfn-ptr",         builtin_jitfn_ptr);

  // Register the col-jit-fn call dispatcher (needs RutColumn access).
  g_coljitfn_dispatch = [](lval* fn, lval* args) -> lval* {
    int n_inputs  = (int)fn->num;
    int n_outputs = fn->count;
    typedef void(*DispFn)(size_t, float**);
    DispFn dispatch = (DispFn)fn->obj;

    // Validate argument count and types.
    if (args->count != n_inputs) {
      lval_del(args);
      return lval_err("col-jit-fn '%s': expected %d column args, got %d",
                      fn->sym, n_inputs, args->count);
    }
    for (int i = 0; i < n_inputs; i++) {
      if (args->cell[i]->type != LVAL_COLUMN) {
        lval_del(args);
        return lval_err("col-jit-fn '%s': argument %d must be a Column", fn->sym, i);
      }
      auto& c = col_of(args->cell[i]);
      if (c->dtype != COL_FLOAT32) {
        lval_del(args);
        return lval_err("col-jit-fn '%s': argument %d must be float32", fn->sym, i);
      }
    }

    size_t n = col_of(args->cell[0])->n;
    for (int i = 1; i < n_inputs; i++) {
      if (col_of(args->cell[i])->n != n) {
        lval_del(args);
        return lval_err("col-jit-fn '%s': column length mismatch at argument %d", fn->sym, i);
      }
    }

    // Allocate output buffers.
    std::vector<float*> out_ptrs(n_outputs);
    for (int j = 0; j < n_outputs; j++) {
      out_ptrs[j] = (float*)col_alloc((n ? n : 1) * sizeof(float));
      if (!out_ptrs[j]) {
        for (int k = 0; k < j; k++) free(out_ptrs[k]);
        lval_del(args);
        return lval_err("col-jit-fn '%s': out of memory", fn->sym);
      }
    }

    // Build args array: [out0, out1, ..., in0, in1, ...]
    std::vector<float*> arg_ptrs(n_outputs + n_inputs);
    for (int j = 0; j < n_outputs; j++) arg_ptrs[j] = out_ptrs[j];
    for (int i = 0; i < n_inputs; i++)
      arg_ptrs[n_outputs + i] = (float*)col_of(args->cell[i])->data;

    dispatch(n, arg_ptrs.data());
    lval_del(args);

    // Return: single column or Q-expr of columns.
    if (n_outputs == 1) {
      auto c = std::make_shared<RutColumn>();
      c->dtype = COL_FLOAT32; c->n = n; c->data = out_ptrs[0];
      return lval_column(std::move(c));
    }
    lval* result = lval_qexpr();
    for (int j = 0; j < n_outputs; j++) {
      auto c = std::make_shared<RutColumn>();
      c->dtype = COL_FLOAT32; c->n = n; c->data = out_ptrs[j];
      lval_add(result, lval_column(std::move(c)));
    }
    return result;
  };
}
