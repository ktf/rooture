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

// ---------------------------------------------------------------------------
// Helpers
// ColDtype letter mapping: F=float32 D=float64 I=int32 i=uint32
//                          S=int16   s=uint16  B=int8  b=uint8  O=bool
// ---------------------------------------------------------------------------
static size_t col_dtype_size(int d) {
  switch (d) {
    case COL_FLOAT64:              return 8;
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

static int branch_dtype(TBranch* br) {
  TString t = br->GetTitle();
  if (t.EndsWith("/F")) return COL_FLOAT32;
  if (t.EndsWith("/D")) return COL_FLOAT64;
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
// Core loader — safe to call from a future thread.
// All ROOT API calls that touch global state (StreamerInfo registry,
// gDirectory, class schema tables) are dispatched to the main thread.
// Only the raw GetBulkEntries read loop runs on the calling thread.
// ---------------------------------------------------------------------------
static RutColumnPtr load_branch_impl(const char* path,
                                      const char* tree_path,
                                      const char* branch_name) {
  TFile*   f     = nullptr;
  TBranch* br    = nullptr;
  int      dtype = -1;
  Long64_t total = 0;

  std::string p(path), tp(tree_path), bn(branch_name);
  rut_dispatch_work([&]{
    f = TFile::Open(p.c_str(), "READ");
    if (!f || f->IsZombie()) return;
    TTree* tree = f->Get<TTree>(tp.c_str());
    if (!tree) return;
    br    = tree->GetBranch(bn.c_str());
    if (!br) return;
    dtype = branch_dtype(br);
    total = br->GetEntries();
  });

  if (!br || dtype < 0) {
    if (f) { rut_dispatch_work([f]{ f->Close(); delete f; }); }
    return nullptr;
  }

  size_t esz = col_dtype_size(dtype);

  Long64_t total_local = total;
  void* buf = malloc((size_t)total_local * esz);
  if (!buf) { rut_dispatch_work([f]{ f->Close(); delete f; }); return nullptr; }

  auto& bulk = br->GetBulkRead();
  TBufferFile tbuf(TBuffer::kRead, 512 * 1024);
  Long64_t entry  = 0;
  size_t   cursor = 0;

  while (entry < total_local) {
    Int_t n = bulk.GetBulkEntries(entry, tbuf);
    if (n <= 0) break;
    memcpy((char*)buf + cursor * esz, tbuf.GetCurrent(), (size_t)n * esz);
    cursor += (size_t)n;
    entry  += n;
  }

  // Close on the main thread — TFile::~TFile calls TProcessUUID::RemoveUUID
  // which mutates a global THashList; concurrent calls from future threads
  // cause a data race and SIGSEGV.
  rut_dispatch_work([f]{ f->Close(); delete f; });

  auto col = std::make_shared<RutColumn>();
  col->dtype = dtype;
  col->n     = cursor;   // actual entries read (== total if no error)
  col->data  = buf;
  return col;
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

  RutColumnPtr col = load_branch_impl(path.c_str(), tree.c_str(), branch.c_str());
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
        return load_branch_impl(path.c_str(), tree.c_str(), bn.c_str());
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
static lval* col_map_impl(void* fp, RutColumnPtr& in_col) {
  size_t n     = in_col->n;
  int    dtype = in_col->dtype;
  size_t esz   = col_dtype_size(dtype);
  void*  out   = malloc(n * esz);
  if (!out) return lval_err("col-map: out of memory");

#define TYPED_MAP(Tin, Tout) do { \
    auto f = (Tout(*)(Tin))fp; \
    Tin*  in  = (Tin*)in_col->data; \
    Tout* out2 = (Tout*)out; \
    for (size_t i = 0; i < n; i++) out2[i] = f(in[i]); \
  } while (0)
  switch (dtype) {
    case COL_FLOAT32: TYPED_MAP(float,    float);    break;
    case COL_FLOAT64: TYPED_MAP(double,   double);   break;
    case COL_INT32:   TYPED_MAP(int32_t,  int32_t);  break;
    case COL_UINT32:  TYPED_MAP(uint32_t, uint32_t); break;
    case COL_INT16:   TYPED_MAP(int16_t,  int16_t);  break;
    case COL_UINT16:  TYPED_MAP(uint16_t, uint16_t); break;
    case COL_INT8:    TYPED_MAP(int8_t,   int8_t);   break;
    case COL_UINT8:   TYPED_MAP(uint8_t,  uint8_t);  break;
    case COL_BOOL:    TYPED_MAP(char,     char);      break;
    default: free(out); return lval_err("col-map: unsupported dtype");
  }
#undef TYPED_MAP
  auto c = std::make_shared<RutColumn>();
  c->dtype = dtype; c->n = n; c->data = out;
  return lval_column(std::move(c));
}

static lval* col_filter_impl(void* fp, RutColumnPtr& in_col) {
  size_t n     = in_col->n;
  int    dtype = in_col->dtype;
  size_t esz   = col_dtype_size(dtype);
  void*  out   = malloc(n * esz);
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
  void* out  = malloc(n * esz);
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
      case COL_FLOAT32: elem = lval_floating(((float*)col->data)[i]);  break;
      case COL_FLOAT64: elem = lval_floating(((double*)col->data)[i]); break;
      case COL_INT32:   elem = lval_num(((int32_t*)col->data)[i]);     break;
      case COL_UINT32:  elem = lval_num((long)((uint32_t*)col->data)[i]); break;
      case COL_INT16:   elem = lval_num(((int16_t*)col->data)[i]);     break;
      case COL_UINT16:  elem = lval_num((long)((uint16_t*)col->data)[i]); break;
      case COL_INT8:    elem = lval_num(((int8_t*)col->data)[i]);      break;
      case COL_UINT8:   elem = lval_num((long)((uint8_t*)col->data)[i]); break;
      case COL_BOOL:    elem = lval_num(((char*)col->data)[i] ? 1 : 0); break;
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
static lval* builtin_col_map_ptr(lenv* /*e*/, lval* a) {
  LASSERT_NUM("col-map-ptr", a, 2);
  LASSERT_TYPE("col-map-ptr", a, 0, LVAL_NUM);
  LASSERT_TYPE("col-map-ptr", a, 1, LVAL_COLUMN);
  void* fp = (void*)a->cell[0]->num;
  auto& col = col_of(a->cell[1]);
  lval* r = col_map_impl(fp, col);
  lval_del(a);
  return r;
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
  float* out = (float*)malloc(n ? n * sizeof(float) : 1);
  if (!out) { lval_del(a); return lval_err("col-cast-f32: out of memory"); }
#define CAST32(T) do { T* p=(T*)col->data; for(size_t i=0;i<n;i++) out[i]=(float)p[i]; } while(0)
  switch (col->dtype) {
    case COL_FLOAT32: CAST32(float);    break;
    case COL_FLOAT64: CAST32(double);   break;
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
  float* out = (float*)malloc(n ? n * sizeof(float) : 1);
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
  void* out = malloc(total ? total * esz : 1);
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
// Registration
// ---------------------------------------------------------------------------
void lenv_add_builtins_column(lenv* e) {
  lenv_add_builtin(e, "load-branch",    builtin_load_branch);
  lenv_add_builtin(e, "load-branches",  builtin_load_branches);
  lenv_add_builtin(e, "col-length",     builtin_col_length);
  lenv_add_builtin(e, "col-dtype",      builtin_col_dtype);
  lenv_add_builtin(e, "col-ref",        builtin_col_ref);
  lenv_add_builtin(e, "col->list",      builtin_col_to_list);
  lenv_add_builtin(e, "col-map",        builtin_col_map);
  lenv_add_builtin(e, "col-map-ptr",    builtin_col_map_ptr);
  lenv_add_builtin(e, "col-filter",     builtin_col_filter);
  lenv_add_builtin(e, "col-filter-ptr", builtin_col_filter_ptr);
  lenv_add_builtin(e, "col-reduce",     builtin_col_reduce);
  lenv_add_builtin(e, "col-reduce-ptr", builtin_col_reduce_ptr);
  lenv_add_builtin(e, "col-zip",        builtin_col_zip);
  lenv_add_builtin(e, "col-zip-ptr",    builtin_col_zip_ptr);
  lenv_add_builtin(e, "col-fill-h1",    builtin_col_fill_h1);
  lenv_add_builtin(e, "col-fill-h2",    builtin_col_fill_h2);
  lenv_add_builtin(e, "col-cast-f32",   builtin_col_cast_f32);
  lenv_add_builtin(e, "col-mask",       builtin_col_mask);
  lenv_add_builtin(e, "col-cat",        builtin_col_cat);
  lenv_add_builtin(e, "jitfn-ptr",      builtin_jitfn_ptr);
}
