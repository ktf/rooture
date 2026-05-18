#pragma once

#include <replxx.hxx>
#include <tree_sitter/api.h>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include "Rtypes.h"
#include "TClass.h"
#include "TApplication.h"
#include "TMethodCall.h"
#include "TSysEvtHandler.h"
#include "TROOT.h"
#include "TSystem.h"
#include "TVirtualX.h"
#include "TVirtualPad.h"
#include "TStopwatch.h"
#include "TException.h"
#include "TInterpreter.h"
#include "TMethod.h"
#include "TMethodArg.h"
#include "TFile.h"
#include "TStyle.h"
#include "TEnv.h"
#include "TRandom.h"
#include "TObjString.h"
#include "TCollection.h"
#include "TQObject.h"
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <unordered_map>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <future>
#include <queue>
#include <regex>
#include <climits>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern "C" {
#include "mpc.h"
const TSLanguage* tree_sitter_rooture();
}

// ---------------------------------------------------------------------------
// lval type tags
// ---------------------------------------------------------------------------
enum { LVAL_ERR, LVAL_NUM, LVAL_FLOAT, LVAL_FLOAT32, LVAL_SYM, LVAL_STR,
       LVAL_FUN, LVAL_TOBJ, LVAL_TMETHOD, LVAL_SEXPR, LVAL_QEXPR,
       LVAL_JITFN, LVAL_ATOM, LVAL_FUTURE, LVAL_PROMISE, LVAL_COLUMN,
       LVAL_COLJITFN };

// ---------------------------------------------------------------------------
// Core structs — forward declared so lbuiltin can reference lval/lenv
// ---------------------------------------------------------------------------
struct lval;
struct lenv;
typedef lval*(*lbuiltin)(lenv*, lval*);

struct lval {
  int type;
  long   num;
  double floating;
  char* err;
  char* sym;
  char* str;
  void    *obj;
  TClass  *cls;
  TMethodCall *method;
  char *methodArgs;
  lbuiltin builtin;
  lenv* env;
  lval* formals;
  lval* body;
  int count;
  lval** cell;
};

struct lenv {
  lenv* par;
  int count;
  char** syms;
  lval** vals;
};

// ---------------------------------------------------------------------------
// mpc parser globals (defined in rut_lang.cxx)
// ---------------------------------------------------------------------------
extern mpc_parser_t* Number;
extern mpc_parser_t* Float32;
extern mpc_parser_t* Floating;
extern mpc_parser_t* Symbol;
extern mpc_parser_t* String;
extern mpc_parser_t* Comment;
extern mpc_parser_t* Inlinestr;
extern mpc_parser_t* QexprItem;
extern mpc_parser_t* Sexpr;
extern mpc_parser_t* Qexpr;
extern mpc_parser_t* Expr;
extern mpc_parser_t* Lispy;

// ---------------------------------------------------------------------------
// Global variables (defined in their respective .cxx files)
// ---------------------------------------------------------------------------
extern bool g_debug;       // rut_repl.cxx
extern lenv* g_global_env; // rut_repl.cxx — root environment (for non-blocking get_symbol)
extern std::shared_mutex g_env_rwlock; // rut_lang.cxx — guards lenv writes
extern int  g_cb_pipe[2];    // rut_root.cxx — callback pipe (int ids)
extern int  g_cling_pipe[2]; // rut_root.cxx — wakes event loop when future posts Cling work

// Cling main-thread dispatch: future threads post work here and block until done.
struct ClingWork {
  std::function<void()>  fn;
  std::promise<void>     done;
};
extern std::mutex                              g_cling_mu;
extern std::queue<std::unique_ptr<ClingWork>> g_cling_queue;

// Set to true on threads spawned by (future ...); false on the main thread.
extern thread_local bool g_in_future;

// ---------------------------------------------------------------------------
// Inline utilities used across translation units
// ---------------------------------------------------------------------------
inline std::string ptr_to_hex(void* p) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%p", p);
  return buf;
}

// ---------------------------------------------------------------------------
// LASSERT macros — need lval_err and lval_del forward declarations below
// ---------------------------------------------------------------------------
void  lval_del(lval* v);
lval* lval_err(const char* fmt, ...);

#define LASSERT(args, cond, fmt, ...)         \
  if (!(cond)) {                              \
    lval* err = lval_err(fmt, ##__VA_ARGS__); \
    lval_del(args);                           \
    return err;                               \
  }

#define LASSERT_NUM(what, a, expected)                  \
    LASSERT(a, a->count == expected,                    \
            "Function '%s' passed too many arguments. " \
            "Got %i, expected %i.", what, a->count,     \
            expected);

#define LASSERT_TYPE(what, a, n, expected)                          \
    LASSERT(a, a->cell[n]->type == expected,                        \
            "Function '%s' passed incorrect type for argument %i. " \
            "Got %s, expected %s.", what, n,                        \
            ltype_name(a->cell[0]->type),                           \
            ltype_name(expected));

// Forward-declared here so RutFuture::~RutFuture() can call it.
void rut_drain_cling_queue();

// ---------------------------------------------------------------------------
// RutAtom — mutable reference cell (used by LVAL_ATOM)
// ---------------------------------------------------------------------------
struct RutAtom {
  std::mutex mu;
  lval*      val;
  explicit RutAtom(lval* v) : val(v) {}
  ~RutAtom() { lval_del(val); }
};
using RutAtomPtr = std::shared_ptr<RutAtom>;

// ---------------------------------------------------------------------------
// RutFuture — deferred value evaluated on a background thread
// ---------------------------------------------------------------------------
struct RutFuture {
  std::mutex              mu;
  std::condition_variable cv;
  bool                    realized   = false;
  bool                    is_promise = false;
  lval*                   result     = nullptr;

  ~RutFuture() {
    if (is_promise) {
      if (result) lval_del(result);
      return;
    }
    while (true) {
      rut_drain_cling_queue();
      std::unique_lock<std::mutex> lock(mu);
      if (realized) { if (result) lval_del(result); return; }
      lock.unlock();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
};
using RutFuturePtr = std::shared_ptr<RutFuture>;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void        rut_print(const char* fmt, ...);
const char* ltype_name(int t);

int   lval_eq(lval* x, lval* y);
lval* lval_copy(lval* v);
lval* lval_tobj(void* obj, TClass* cls);
lval* lval_eval(lenv* e, lval* v);
lval* lval_eval_sexpr(lenv* e, lval* v);
lval* lval_call(lenv* e, lval* f, lval* a);
lval* lval_str(const char* s);
lval* lval_sym(const char* s);
lval* lval_num(long x);
lval* lval_floating(double x);
lval* lval_sexpr(void);
lval* lval_qexpr(void);
lval* lval_add(lval* v, lval* x);
lval* lval_pop(lval* v, int i);
lval* lval_take(lval* v, int i);
lval* lval_read(mpc_ast_t* t);
lval* lval_jitfn(const char* name, long nparams);
lval* lval_fun(lbuiltin func);
lval* lval_lambda(lval* formals, lval* body);
// col-jit-fn: sym=kernel_name, num=n_inputs, count=n_outputs, obj=dispatch_ptr
lval* lval_coljitfn(const char* name, int n_inputs, int n_outputs, void* dispatch_ptr);

// Dispatch callback — set by rut_column.cxx so rut_lang.cxx stays column-free.
typedef lval* (*ColJitFnDispatch)(lval* fn, lval* args);
extern ColJitFnDispatch g_coljitfn_dispatch;
lval* lval_atom(lval* init);
lval* lval_future_new(RutFuturePtr rf);
lval* lval_promise_new();
void        lval_print(lval* v);
void        lval_println(lval* v);
std::string lval_sprint(lval* v); // serialize lval to string without printing (MCP-thread-safe)
lenv* lenv_snapshot(lenv* e);

std::string executable_dir();
extern std::vector<std::string> load_path;
TFileHandler* rut_make_callback_handler(int fd, lenv* env);
TFileHandler* rut_make_cling_handler(int fd);

// Dispatch work to the main thread if called from a future thread; run inline otherwise.
void   rut_dispatch_work(std::function<void()> fn);
// rut_drain_cling_queue — declared earlier (needed by RutFuture destructor).
// Cling primitives — always execute on the main thread (dispatch if needed).
Long_t rut_calc(const char* expr, TInterpreter::EErrorCode* ec = nullptr);
Long_t rut_process_line(const char* code, TInterpreter::EErrorCode* ec = nullptr);
bool   rut_declare(const char* code);
TFile* rut_open_file(const char* path);

// ---------------------------------------------------------------------------
// RutColumn — owned typed flat buffer (used by LVAL_COLUMN)
// ---------------------------------------------------------------------------
enum ColDtype {
  COL_FLOAT32, COL_FLOAT64,
  COL_INT64,   COL_UINT64,
  COL_INT32,   COL_UINT32,
  COL_INT16,   COL_UINT16,
  COL_INT8,    COL_UINT8,
  COL_BOOL,
};

struct RutColumn {
  int    dtype = 0;
  size_t n     = 0;
  void*  data  = nullptr;   // owned, malloc'd
  ~RutColumn() { free(data); }
};
using RutColumnPtr = std::shared_ptr<RutColumn>;

// Thread pool — fixed-size worker pool used by (future ...) instead of raw threads.
void rut_pool_create(int n_workers);    // call once from main() after TApplication init
void rut_pool_submit(std::function<void()> task);
void rut_pool_destroy();                // call before process exit
int  rut_pool_size();                   // returns number of worker threads
void rut_pool_set_size(int n_workers);  // resize pool (joins current workers first)

lenv* lenv_new(void);
void  lenv_del(lenv* e);
lenv* lenv_copy(lenv* e);
lval* lenv_get(lenv* e, lval* v);
lval* lenv_get_local(lenv* e, lval* k);
void  lenv_put(lenv* e, lval* k, lval* v);
void  lenv_def(lenv* e, lval* k, lval* v);

void lenv_add_builtin(lenv* e, const char* name, lbuiltin func);
void lenv_add_global_object(lenv* e, const char* name, void* obj, TClass* cls);

lval* builtin_eval(lenv* e, lval* a);
lval* builtin_list(lenv* e, lval* a);
lval* builtin_op(lenv* e, lval* a, const char* op);

std::string escape_for_cling_str(const char* s);
std::string lval_to_cpp_arg(lenv* e, lval* a, int offset,
                             const std::vector<bool>* deref_mask = nullptr);

// lenv_add_builtins_* — one per module, called from lenv_add_builtins
void lenv_add_builtins_lang(lenv* e);
void lenv_add_builtins_root(lenv* e);
void lenv_add_builtins_jitfn(lenv* e);
void rut_file_cache_clear();   // close all cached TFiles (main thread)
void lenv_add_builtins_column(lenv* e);
void lenv_add_builtins_net(lenv* e);
void lenv_add_builtins(lenv* e);
