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
#include "TFile.h"
#include "TStyle.h"
#include "TRandom.h"
#include "TObjString.h"
#include "TCollection.h"
#include "TQObject.h"
#include <iostream>
#include <string>
#include <vector>
#include <set>
#include <map>
#include <atomic>
#include <regex>
#include <climits>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern "C"
{
#include "mpc.h"
const TSLanguage* tree_sitter_rooture();
}

/* Create Enumeration of Possible lval Types */
enum { LVAL_ERR, LVAL_NUM,  LVAL_FLOAT, LVAL_SYM, LVAL_STR,
       LVAL_FUN, LVAL_TOBJ, LVAL_TMETHOD, LVAL_SEXPR, LVAL_QEXPR };

static bool g_debug = false;

static int             g_pipe_fds[2];
static replxx::Replxx* g_rx = nullptr;
static TSParser*       g_ts_parser = nullptr;

// MCP mode
static bool        g_mcp_mode = false;
static int         g_mcp_reply_fds[2];
static FILE*       g_mcp_out = stdout;  // real JSON-RPC output stream (saved before fd-1 redirect)
static bool        g_capturing = false;
static std::string g_capture_buf;

static void rut_print(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  if (g_capturing) {
    char buf[4096]; vsnprintf(buf, sizeof(buf), fmt, ap);
    g_capture_buf += buf;
  } else if (g_mcp_mode) {
    // discard: stdout is reserved for JSON-RPC in MCP mode
  } else if (g_rx) {
    char buf[4096]; vsnprintf(buf, sizeof(buf), fmt, ap);
    g_rx->print("%s", buf);
  } else {
    vprintf(fmt, ap);
  }
  va_end(ap);
}

struct lval;
struct lenv;
void lval_del(lval* v);
int lval_eq(lval* x, lval* y);
lval* lval_copy(lval* v);
lval* lval_err(const char* fmt, ...);
lval* lval_tobj(void* obj, TClass* cls);
lval* lval_eval(lenv* e, lval* v);
lval* lval_eval_sexpr(lenv* e, lval* v);
lval* lval_call(lenv* e, lval* f, lval* a);
lval* lval_str(const char *s);
lval* lenv_get(lenv *e, lval* v);
void lenv_put(lenv *e, lval* k, lval* v);
lval* builtin_eval(lenv *e, lval* a);
lval* builtin_list(lenv *e, lval* a);
lval* builtin_op(lenv* e, lval* a, const char* op);

/* Function pointer*/
typedef lval*(*lbuiltin)(lenv*, lval*);

/* Declare New lval Struct */
struct lval {
  int type;

  /* Basic */
  long   num;
  double floating;
  char* err;
  char* sym;
  char* str;
  /* Generic C++ object */
  void    *obj;
  TClass  *cls;
  TMethodCall *method;
  char *methodArgs;

  /* Function */
  lbuiltin builtin;
  lenv* env;
  lval* formals;
  lval* body;

  /* Expression */
  int count;
  lval** cell;
};

/* Parsers */
mpc_parser_t* Number;
mpc_parser_t* Floating;
mpc_parser_t* Symbol;
mpc_parser_t* String;
mpc_parser_t* Comment;
mpc_parser_t* Inlinestr;
mpc_parser_t* QexprItem;
mpc_parser_t* Sexpr;
mpc_parser_t* Qexpr;
mpc_parser_t* Expr;
mpc_parser_t* Lispy;

/* The environment (context) for functions */
struct lenv {
  lenv* par;
  int count;
  char** syms;
  lval** vals;
};

/* Create a new environment */
lenv* lenv_new(void) {
  lenv* e = (lenv *) malloc(sizeof(lenv));
  e->par = NULL;
  e->count = 0;
  e->syms = NULL;
  e->vals = NULL;
  return e;
}

void lenv_del(lenv* e) {
  for (int i = 0; i < e->count; i++) {
    free(e->syms[i]);
    lval_del(e->vals[i]);
  }
  free(e->syms);
  free(e->vals);
  free(e);
}

lval* lenv_get(lenv* e, lval* k) {
  /* Linear search of symbol in current context */
  for (int i = 0; i < e->count; i++) {
    if (strcmp(e->syms[i], k->sym) == 0) {
      return lval_copy(e->vals[i]);
    }
  }

  /* If no symbol check in parent. If we are at top level then we bind a
     symbol to a string which has the same value. */
  if (e->par) {
    return lenv_get(e->par, k);
  } else {
    lval *v = lval_str(k->sym);
    lenv_put(e, k, v);
    return v;
  }
}

void lenv_put(lenv* e, lval* k, lval* v) {

  /* Iterate over all items in environment */
  /* This is to see if variable already exists */
  for (int i = 0; i < e->count; i++) {

    /* If variable is found delete item at that position */
    /* And replace with variable supplied by user */
    if (strcmp(e->syms[i], k->sym) == 0) {
      lval_del(e->vals[i]);
      e->vals[i] = lval_copy(v);
      return;
    }
  }

  /* If no existing entry found allocate space for new entry */
  e->count++;
  e->vals = (lval **)realloc(e->vals, sizeof(lval*) * e->count);
  e->syms = (char **)realloc(e->syms, sizeof(char*) * e->count);

  /* Copy contents of lval and symbol string into new location */
  e->vals[e->count-1] = lval_copy(v);
  e->syms[e->count-1] = strdup(k->sym);
}

lenv* lenv_copy(lenv* e) {
  lenv* n = (lenv *)malloc(sizeof(lenv));
  n->par = e->par;
  n->count = e->count;
  n->syms = (char **)malloc(sizeof(char*) * n->count);
  n->vals = (lval **)malloc(sizeof(lval*) * n->count);
  for (int i = 0; i < e->count; i++) {
    n->syms[i] = strdup(e->syms[i]);
    n->vals[i] = lval_copy(e->vals[i]);
  }
  return n;
}

void lenv_def(lenv* e, lval* k, lval* v) {
  /* Iterate till e has no parent */
  while (e->par) { e = e->par; }
  /* Put value in e */
  lenv_put(e, k, v);
}

lval* lval_eval(lenv* e, lval* v) {
  if (v->type == LVAL_SYM) {
    /* @name — look up a TNamed ROOT object by name */
    if (v->sym[0] == '@') {
      const char* name = v->sym + 1;
      TObject* obj = gROOT->FindObject(name);
      if (!obj && gDirectory) obj = (TObject*)gDirectory->Get(name);
      lval_del(v);
      if (!obj) return lval_err("No ROOT object named '%s'", name);
      TClass* cls = obj->IsA();
      return lval_tobj((void*)obj, cls);
    }
    lval* x = lenv_get(e, v);
    lval_del(v);
    return x;
  }
  if (v->type == LVAL_SEXPR) { return lval_eval_sexpr(e, v); }
  return v;
}

void lval_del(lval* v);
/* Create Enumeration of Possible Error Types */
enum { LERR_DIV_ZERO, LERR_BAD_OP, LERR_BAD_NUM };

lval* lval_fun(lbuiltin func) {
  lval* v = (lval*)malloc(sizeof(lval));
  v->type = LVAL_FUN;
  v->builtin = func;
  return v;
}

/* Create a new generic C++ object lval */
lval* lval_tobj(void *obj, TClass *cls) {
  lval* v = (lval*)malloc(sizeof(lval));
  v->type = LVAL_TOBJ;
  v->obj  = obj;
  v->cls  = cls;
  return v;
}

/* Create a new TMethodCall lval */
lval* lval_tmethod(TMethodCall *method, const char *args) {
  lval* v = (lval*)malloc(sizeof(lval));
  v->type = LVAL_TMETHOD;
  v->method = method;
  v->methodArgs = strdup(args);
  return v;
}

/* Create a new number type lval */
lval* lval_num(long x) {
  lval* v = (lval *)malloc(sizeof(lval));
  v->type = LVAL_NUM;
  v->num = x;
  return v;
}

/* Create a floating point lval */
lval *lval_floating(double x) {
  lval* v = (lval *)malloc(sizeof(lval));
  v->type = LVAL_FLOAT;
  v->floating = x;
  return v;
}

lval* lval_str(const char* s) {
  lval* v = (lval *)malloc(sizeof(lval));
  v->type = LVAL_STR;
  v->str = strdup(s);
  return v;
}

lval* lval_lambda(lval* formals, lval* body) {
  lval* v = (lval*) malloc(sizeof(lval));
  v->type = LVAL_FUN;

  /* Set Builtin to Null */
  v->builtin = NULL;

  /* Build new environment */
  v->env = lenv_new();

  /* Set Formals and Body */
  v->formals = formals;
  v->body = body;
  return v;  
}

const char* ltype_name(int t) {
  switch(t) {
    case LVAL_FUN: return "Function";
    case LVAL_NUM: return "Number";
    case LVAL_FLOAT: return "Floating";
    case LVAL_ERR: return "Error";
    case LVAL_SYM: return "Symbol";
    case LVAL_STR: return "String";
    case LVAL_TOBJ: return "Object";
    case LVAL_TMETHOD: return "Method";
    case LVAL_SEXPR: return "S-Expression";
    case LVAL_QEXPR: return "Q-Expression";
    default: return "Unknown";
  }
}

lval* lval_err(const char* fmt, ...) {
  lval* v = (lval *)malloc(sizeof(lval));
  v->type = LVAL_ERR;

  /* Create a va list and initialize it */
  va_list va;
  va_start(va, fmt);

  /* Allocate 512 bytes of space */
  v->err = (char *)malloc(512);

  /* printf the error string with a maximum of 511 characters */
  vsnprintf(v->err, 511, fmt, va);

  /* Reallocate to number of bytes actually used */
  v->err = (char *)realloc(v->err, strlen(v->err)+1);

  /* Cleanup our va list */
  va_end(va);

  return v;
}

/* Construct a pointer to a new Symbol lval */ 
lval* lval_sym(const char* s) {
  lval* v = (lval *)malloc(sizeof(lval));
  v->type = LVAL_SYM;
  v->sym = strdup(s);
  return v;
}

/* A pointer to a new empty Sexpr lval */
lval* lval_sexpr(void) {
  lval* v = (lval *)malloc(sizeof(lval));
  v->type = LVAL_SEXPR;
  v->count = 0;
  v->cell = NULL;
  return v;
}

/* A pointer to a new empty Qexpr lval */
lval* lval_qexpr(void) {
  lval* v = (lval *)malloc(sizeof(lval));
  v->type = LVAL_QEXPR;
  v->count = 0;
  v->cell = NULL;
  return v;
}

void lval_del(lval* v) {
  switch (v->type) {
    /* No deletion for functions*/
    case LVAL_FUN:
      if (!v->builtin) {
        lenv_del(v->env);
        lval_del(v->formals);
        lval_del(v->body);
      }
    break;
    case LVAL_TOBJ:
    // FIXME: Reference counting TObjects?
    break;
    case LVAL_TMETHOD:
    // FIXME: reference counting TMethods?
    break;
    /* Do nothing special for number type */
    case LVAL_NUM: break;
    case LVAL_FLOAT: break;

    /* For Err or Sym free the string data */
    case LVAL_ERR: free(v->err); break;
    case LVAL_SYM: free(v->sym); break;
    case LVAL_STR: free(v->str); break;
    /* If Sexpr or Qexpr then delete all elements inside */
    case LVAL_QEXPR:
    case LVAL_SEXPR:
      for (int i = 0; i < v->count; i++) {
        lval_del(v->cell[i]);
      }
      /* Also free the memory allocated to contain the pointers */
      free(v->cell);
    break;
  }

  /* Free the memory allocated for the "lval" struct itself */
  free(v);
}

lval* lval_read_num(mpc_ast_t* t) {
  errno = 0;
  long x = strtol(t->contents, NULL, 10);
  return errno != ERANGE ?
    lval_num(x) : lval_err("invalid number", t->contents);
}

lval* lval_read_floating(mpc_ast_t* t) {
  errno = 0;
  double x = strtod(t->contents, NULL);
  return errno != ERANGE ?
    lval_floating(x) : lval_err("Invalid number", t->contents);
}

lval* lval_add(lval* v, lval* x) {
  v->count++;
  v->cell = (lval **)realloc(v->cell, sizeof(lval*) * v->count);
  v->cell[v->count-1] = x;
  return v;
}
void lval_print(lval* v);

void lval_expr_print(lval* v, char open, char close) {
  rut_print("%c", open);
  for (int i = 0; i < v->count; i++) {
    lval_print(v->cell[i]);
    if (i != (v->count-1)) rut_print(" ");
  }
  rut_print("%c", close);
}

void lval_print_str(lval* v) {
  char* escaped = strdup(v->str);
  escaped = (char *)mpcf_escape(escaped);
  rut_print("\"%s\"", escaped);
  free(escaped);
}

// Escape a raw C string for embedding inside a C++ double-quoted literal that
// will be fed to Cling's ProcessLine / Execute.  Backslashes and double-quotes
// must be escaped; other characters are passed through unchanged.
static std::string escape_for_cling_str(const char* s) {
  std::string out;
  out.reserve(strlen(s) + 8);
  for (; *s; ++s) {
    if (*s == '\\' || *s == '"') out += '\\';
    out += *s;
  }
  return out;
}

TObjArray *lval_to_obj_array(lval *a, int offset) {
  TObjArray *args = new TObjArray();
  for (int i = offset; i < a->count; i++) {
    lval *v = a->cell[i];
    switch (v->type) {
      case LVAL_NUM: args->Add(new TObjString(strdup(std::to_string(v->num).c_str()))); break;
      case LVAL_FLOAT: args->Add(new TObjString(strdup(std::to_string(v->floating).c_str()))); break;
      case LVAL_STR: args->Add(new TObjString(strdup(("\"" + escape_for_cling_str(v->str) + "\"").c_str()))); break;
      default:
        rut_print("Cannot use as a C++ argument.");
        args->Add(new TObjString(""));
    }
  }
  return args;

}
static std::string ptr_to_hex(void* p) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%p", p);
  return buf;
}

std::string lval_to_cpp_arg(lenv* e, lval* a, int offset);  // forward decl

// ---- Callable bridge -------------------------------------------------------

struct RutureClosure { lenv* env; lval* fn; };
static std::map<std::string, RutureClosure> g_callable_registry;
static std::atomic<int> g_callable_counter{0};

extern "C" double rooture_invoke_callable_c(const char* key,
                                             const double* args, int n) {
  auto it = g_callable_registry.find(key);
  if (it == g_callable_registry.end()) return 0.0;
  RutureClosure& c = it->second;
  lval* fn   = lval_copy(c.fn);
  lval* argv = lval_sexpr();
  for (int i = 0; i < n; i++)
    lval_add(argv, lval_floating(args[i]));
  lval* result = lval_call(c.env, fn, argv);
  lval_del(fn);
  double ret = 0.0;
  if (result->type == LVAL_FLOAT)     ret = result->floating;
  else if (result->type == LVAL_NUM)  ret = (double)result->num;
  else if (result->type == LVAL_TOBJ && !result->cls && result->obj)
    ret = *(double*)result->obj;  // heap-allocated primitive from cling_new_auto_typed
  lval_del(result);
  return ret;
}

static std::string register_rooture_callable(lenv* e, lval* fn) {
  std::string key = "__rut_fn_" + std::to_string(g_callable_counter++);
  g_callable_registry[key] = { e, lval_copy(fn) };
  return key;
}

static void emit_jit_wrapper(const std::string& key, lval* fn) {
  int nargs = 0;
  if (fn->formals)
    for (int i = 0; i < fn->formals->count; i++)
      if (strcmp(fn->formals->cell[i]->sym, "&") != 0) nargs++;

  std::string decl = "double " + key + "_wrapper(";
  std::string arr;
  for (int i = 0; i < nargs; i++) {
    if (i) { decl += ", "; arr += ", "; }
    decl += "double _a" + std::to_string(i);
    arr  += "_a" + std::to_string(i);
  }
  decl += ") { ";
  if (nargs > 0)
    decl += "double _args[] = {" + arr + "}; "
            "return __rooture_invoke_ptr(\"" + key + "\", _args, "
            + std::to_string(nargs) + "); }";
  else
    decl += "return __rooture_invoke_ptr(\"" + key + "\", nullptr, 0); }";
  gInterpreter->Declare(decl.c_str());
}

// ---------------------------------------------------------------------------

std::string lval_to_cpp_arg(lenv* e, lval* a, int offset) {
  std::string args;
  bool first = true;
  for (int i = offset; i < a->count; i++) {
    if (!first) args += ", ";
    first = false;
    lval *v = a->cell[i];
    switch (v->type) {
      case LVAL_NUM:   args += std::to_string(v->num); break;
      case LVAL_FLOAT: args += std::to_string(v->floating); break;
      case LVAL_STR:   args += "\"" + escape_for_cling_str(v->str) + "\""; break;
      case LVAL_TOBJ:
        if (v->cls)
          args += "((" + std::string(v->cls->GetName()) + "*)" + ptr_to_hex(v->obj) + ")";
        else
          args += ptr_to_hex(v->obj);
        break;
      case LVAL_FUN:
        if (v->builtin == nullptr) {
          std::string key = register_rooture_callable(e, v);
          emit_jit_wrapper(key, v);
          args += key + "_wrapper";
        } else {
          rut_print("Cannot pass builtin as C++ callable.\n");
        }
        break;
      default:
        rut_print("Cannot use lval type %d as a C++ argument.\n", v->type);
    }
  }
  return args;
}

/* Print an "lval" */
void lval_print(lval* v) {
  switch (v->type) {
    case LVAL_NUM:    rut_print("%li", v->num); break;
    case LVAL_FLOAT:  rut_print("%f", v->floating); break;
    case LVAL_ERR:    rut_print("Error: %s", v->err); break;
    case LVAL_FUN:
      if (v->builtin) {
        rut_print("<builtin>");
      } else {
        rut_print("(\\ "); lval_print(v->formals);
        rut_print(" "); lval_print(v->body); rut_print(")");
      }
    break;
    case LVAL_TOBJ:
      rut_print("<%s @%p>\n", v->cls ? v->cls->GetName() : "object", v->obj);
      if (v->obj && v->cls && !v->cls->InheritsFrom("TVirtualPad")) {
        TMethodCall mc(v->cls, "Print", "");
        if (mc.IsValid()) mc.Execute(v->obj);
      }
    break;
    case LVAL_TMETHOD:
      rut_print("<tmethodcall %s(%s)>", v->method->GetMethodName(), v->methodArgs);
    break;
    case LVAL_SYM:   rut_print("%s", v->sym); break;
    case LVAL_STR:   lval_print_str(v); break;
    case LVAL_SEXPR: lval_expr_print(v, '(', ')'); break;
    case LVAL_QEXPR: lval_expr_print(v, '{', '}'); break;
  }
}

void lval_println(lval* v) { lval_print(v); rut_print("\n"); }

lval* lval_read_str(mpc_ast_t* t) {
  /* Cut off the final quote character */
  t->contents[strlen(t->contents)-1] = '\0';
  /* Copy the string missing out the first quote character */
  char* unescaped = strdup(t->contents+1);
  /* Pass through the unescape function */
  unescaped = (char *)mpcf_unescape(unescaped);
  /* Construct a new lval using the string */
  lval* str = lval_str(unescaped);
  /* Free the string and return */
  free(unescaped);
  return str;
}

/* Trim leading and trailing whitespace in-place (returns pointer into buf). */
static char* trim_ws(char* buf) {
  char* s = buf;
  while (*s == ' ' || *s == '\t' || *s == '\r') s++;
  char* e = s + strlen(s);
  while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
  *e = '\0';
  return s;
}

lval* lval_read(mpc_ast_t* t) {
  /* Leaf nodes */
  if (strstr(t->tag, "number"))   { return lval_read_num(t); }
  if (strstr(t->tag, "floating")) { return lval_read_floating(t); }
  if (strstr(t->tag, "symbol"))   { return lval_sym(t->contents); }
  if (strstr(t->tag, "string"))   { return lval_read_str(t); }

  /* qexpr_item: either '|' <inlinestr> (→ string) or a plain <expr> */
  if (strstr(t->tag, "qexpr_item")) {
    for (int i = 0; i < t->children_num; i++) {
      if (strstr(t->children[i]->tag, "inlinestr")) {
        char* raw = strdup(t->children[i]->contents);
        lval* v = lval_str(trim_ws(raw));
        free(raw);
        return v;
      }
      if (strstr(t->children[i]->tag, "comment")) return NULL;
      if (strcmp(t->children[i]->tag, "regex") == 0) continue;
      if (strcmp(t->children[i]->contents, "|") == 0) continue;
      return lval_read(t->children[i]);
    }
    return NULL;
  }

  /* Container nodes */
  lval* x = NULL;
  if (strcmp(t->tag, ">") == 0) { x = lval_sexpr(); }
  if (strstr(t->tag, "sexpr"))  { x = lval_sexpr(); }
  if (strstr(t->tag, "qexpr"))  { x = lval_qexpr(); }

  for (int i = 0; i < t->children_num; i++) {
    if (strcmp(t->children[i]->contents, "(") == 0) { continue; }
    if (strcmp(t->children[i]->contents, ")") == 0) { continue; }
    if (strcmp(t->children[i]->contents, "}") == 0) { continue; }
    if (strcmp(t->children[i]->contents, "{") == 0) { continue; }
    if (strcmp(t->children[i]->tag,  "regex") == 0) { continue; }
    if (strstr(t->children[i]->tag, "comment")) { continue; }
    lval* child = lval_read(t->children[i]);
    if (child) x = lval_add(x, child);  /* NULL means skipped (e.g. comment in qexpr_item) */
  }
  return x;
}

lval* lval_pop(lval* v, int i) {
  /* Find the item at "i" */
  lval* x = v->cell[i];

  /* Shift memory after the item at "i" over the top */
  memmove(&v->cell[i], &v->cell[i+1],
    sizeof(lval*) * (v->count-i-1));

  /* Decrease the count of items in the list */
  v->count--;

  /* Reallocate the memory used */
  v->cell = (lval**)realloc(v->cell, sizeof(lval*) * v->count);
  return x;
}

lval* lval_take(lval* v, int i) {
  lval* x = lval_pop(v, i);
  lval_del(v);
  return x;
}

lval* lval_copy(lval* v) {
  
  lval* x = (lval*)malloc(sizeof(lval));
  x->type = v->type;
  
  switch (v->type) {
    
    /* Copy Functions and Numbers Directly */
    case LVAL_FUN:
      if (v->builtin) {
        x->builtin = v->builtin;
      } else {
        x->builtin = NULL;
        x->env = lenv_copy(v->env);
        x->formals = lval_copy(v->formals);
        x->body = lval_copy(v->body);
      }
    break;

    // FIXME: should we do reference counting?
    case LVAL_TOBJ: x->obj = v->obj; x->cls = v->cls; break;
    case LVAL_TMETHOD: 
      x->method = v->method; 
      x->methodArgs = strdup(v->methodArgs);
    break;

    case LVAL_NUM: x->num = v->num; break;
    case LVAL_FLOAT: x->floating = v->floating; break;
    
    /* Copy Strings using malloc and strcpy */
    case LVAL_ERR:
      x->err = strdup(v->err); break;
    case LVAL_SYM:
      x->sym = strdup(v->sym); break;
    case LVAL_STR: 
      x->str = strdup(v->str); break;

    /* Copy Lists by copying each sub-expression */
    case LVAL_SEXPR:
    case LVAL_QEXPR:
      x->count = v->count;
      x->cell = (lval **)malloc(sizeof(lval*) * x->count);
      for (int i = 0; i < x->count; i++) {
        x->cell[i] = lval_copy(v->cell[i]);
      }
    break;
  }
  
  return x;
}

lval* lval_call(lenv* e, lval* f, lval* a) {

  /* If Builtin then simply apply that */
  if (f->builtin) { return f->builtin(e, a); }

  /* Record Argument Counts */
  int given = a->count;
  int total = f->formals->count;

  /* While arguments still remain to be processed */
  while (a->count) {

    /* If we've ran out of formal arguments to bind */
    if (f->formals->count == 0) {
      lval_del(a); return lval_err(
        "Function passed too many arguments. "
        "Got %i, Expected %i.", given, total); 
    }

    /* Pop the first symbol from the formals */
    lval* sym = lval_pop(f->formals, 0);
    /* Special Case to deal with '&' */
    if (strcmp(sym->sym, "&") == 0) {

      /* Ensure '&' is followed by another symbol */
      if (f->formals->count != 1) {
        lval_del(a);
        return lval_err("Function format invalid. "
          "Symbol '&' not followed by single symbol.");
      }

      /* Next formal should be bound to remaining arguments */
      lval* nsym = lval_pop(f->formals, 0);
      lenv_put(f->env, nsym, builtin_list(e, a));
      lval_del(sym); lval_del(nsym);
      break;
    }

    /* Pop the next argument from the list */
    lval* val = lval_pop(a, 0);

    /* Bind a copy into the function's environment */
    lenv_put(f->env, sym, val);

    /* Delete symbol and value */
    lval_del(sym); lval_del(val);
  }

  /* Argument list is now bound so can be cleaned up */
  lval_del(a);

  /* If '&' remains in formal list bind to empty list */
  if (f->formals->count > 0 &&
    strcmp(f->formals->cell[0]->sym, "&") == 0) {
    
    /* Check to ensure that & is not passed invalidly. */
    if (f->formals->count != 2) {
      return lval_err("Function format invalid. "
        "Symbol '&' not followed by single symbol.");
    }
    
    /* Pop and delete '&' symbol */
    lval_del(lval_pop(f->formals, 0));
    
    /* Pop next symbol and create empty list */
    lval* sym = lval_pop(f->formals, 0);
    lval* val = lval_qexpr();
    
    /* Bind to environment and delete */
    lenv_put(f->env, sym, val);
    lval_del(sym); lval_del(val);
  }
  
  /* If all formals have been bound evaluate */
  if (f->formals->count == 0) {

    /* Set environment parent to evaluation environment */
    f->env->par = e;

    /* Evaluate and return */
    return builtin_eval(
      f->env, lval_add(lval_sexpr(), lval_copy(f->body)));
  } else {
    /* Otherwise return partially evaluated function */
    return lval_copy(f);
  }

}

// Evaluate an expression
lval* lval_eval_sexpr(lenv* e, lval* v) {

  /* Desugar (.Method args...) → (. Method args...) */
  if (v->count >= 1 && v->cell[0]->type == LVAL_SYM) {
    const char* sym = v->cell[0]->sym;
    if (sym[0] == '.' && sym[1] != '\0') {
      lval* dot    = lval_sym(".");
      lval* method = lval_sym(sym + 1);
      free(v->cell[0]->sym);
      v->cell[0]->sym = dot->sym; dot->sym = nullptr; lval_del(dot);
      /* Insert method name as new second cell */
      v->count++;
      v->cell = (lval**)realloc(v->cell, sizeof(lval*) * v->count);
      memmove(v->cell + 2, v->cell + 1, sizeof(lval*) * (v->count - 2));
      v->cell[1] = method;
    }
    /* Desugar (::Method ClassName args...) → (:: "Method" "ClassName" args...)
       Method and class name must become strings so they are not looked up in
       the rooture environment as variable names. */
    else if (sym[0] == ':' && sym[1] == ':' && sym[2] != '\0') {
      lval* colcol = lval_sym("::");
      lval* method = lval_str(sym + 2);   // string, not sym
      free(v->cell[0]->sym);
      v->cell[0]->sym = colcol->sym; colcol->sym = nullptr; lval_del(colcol);
      v->count++;
      v->cell = (lval**)realloc(v->cell, sizeof(lval*) * v->count);
      memmove(v->cell + 2, v->cell + 1, sizeof(lval*) * (v->count - 2));
      v->cell[1] = method;
      /* Also convert the class-name cell from sym to string so it is not
         looked up in the env.  It may be a sym (bare word) or already a str. */
      if (v->cell[2]->type == LVAL_SYM) {
        lval* cstr = lval_str(v->cell[2]->sym);
        lval_del(v->cell[2]);
        v->cell[2] = cstr;
      }
    }
  }

  for (int i = 0; i < v->count; i++) {
    v->cell[i] = lval_eval(e, v->cell[i]);
  }
  
  for (int i = 0; i < v->count; i++) {
    if (v->cell[i]->type == LVAL_ERR) { return lval_take(v, i); }
  }

  if (v->count == 0) { return v; }
  if (v->count == 1) {
    lval* x = lval_take(v, 0);
    /* If the single element is a function, call it with zero arguments.
       This makes (fn) correctly invoke fn rather than just returning it. */
    if (x->type == LVAL_FUN) {
      lval* result = lval_call(e, x, lval_sexpr());
      lval_del(x);
      return result;
    }
    return x;
  }

  /* Ensure first element is a function after evaluation */
  lval* f = lval_pop(v, 0);
  if (f->type != LVAL_FUN) {
    lval* err = lval_err(
      "S-Expression starts with incorrect type. "
      "Got %s, Expected %s.",
      ltype_name(f->type), ltype_name(LVAL_FUN));
    lval_del(f); lval_del(v);
    return err;
  }

  /* If so call function to get result */
  lval* result = lval_call(e, f, v);
  lval_del(f);
  return result;
}

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

lval* builtin_head(lenv *e, lval* a) {
  /* Check Error Conditions */
  LASSERT_NUM("head", a, 1);
  LASSERT_TYPE("head", a, 0, LVAL_QEXPR);
  LASSERT(a, a->cell[0]->count != 0,
    "Function 'head' passed {}!");

  /* Otherwise take first argument */
  lval* v = lval_take(a, 0);

  /* Delete all elements that are not head and return */
  while (v->count > 1) { lval_del(lval_pop(v, 1)); }
  return v;
}

lval* builtin_tail(lenv *e, lval* a) {
  /* Check Error Conditions */
  LASSERT_NUM("tail", a, 1);
  LASSERT_TYPE("tail", a, 0, LVAL_QEXPR);
  LASSERT(a, a->cell[0]->count != 0,
    "Function 'tail' passed {}!");

  /* Take first argument */
  lval* v = lval_take(a, 0);

  /* Delete first element and return */
  lval_del(lval_pop(v, 0));
  return v;
}

lval* builtin_list(lenv *e, lval* a) {
  a->type = LVAL_QEXPR;
  return a;
}

lval* builtin_eval(lenv *e, lval* a) {
  LASSERT_NUM("eval", a, 1);
  LASSERT_TYPE("eval", a, 0, LVAL_QEXPR);

  lval* x = lval_take(a, 0);
  x->type = LVAL_SEXPR;
  return lval_eval(e, x);
}

lval* builtin_do(lenv* e, lval* a) {
  /* Evaluate each argument in sequence, return the last result. */
  if (a->count == 0) { lval_del(a); return lval_sexpr(); }
  lval* result = lval_pop(a, a->count - 1);
  lval_del(a);
  return result;
}

lval* lval_join(lval* x, lval* y) {

  /* For each cell in 'y' add it to 'x' */
  while (y->count) {
    x = lval_add(x, lval_pop(y, 0));
  }

  /* Delete the empty 'y' and return 'x' */
  lval_del(y);  
  return x;
}

lval* builtin_join(lenv *e, lval* a) {

  for (int i = 0; i < a->count; i++) {
    LASSERT(a, a->cell[i]->type == LVAL_QEXPR,
      "Function 'join' passed incorrect type.");
  }

  lval* x = lval_pop(a, 0);

  while (a->count) {
    x = lval_join(x, lval_pop(a, 0));
  }

  lval_del(a);
  return x;
}

// Built-in method to get a member (either data or method) of a given object.
// - The first argument must be a string.
// - The second argument must be an object.
// - Rest of the arguments should be passed to the method call, if 
//   we are referring to one.
lval* builtin_member(lenv *e, lval *a) {
  LASSERT(a, a->count >= 2,
    "Function '.' needs at least 2 argument: <method name> and <object>.");
  LASSERT_TYPE(".", a, 0, LVAL_STR);
  LASSERT_TYPE(".", a, 1, LVAL_TOBJ);
  lval* name = lval_pop(a, 0);
  lval* obj  = lval_pop(a, 0);

  bool has_callable = false;
  for (int i = 0; i < a->count; i++)
    if (a->cell[i]->type == LVAL_FUN && a->cell[i]->builtin == nullptr)
      has_callable = true;

  std::string method_name = name->str;
  std::string class_name  = obj->cls ? obj->cls->GetName() : "void";
  void*       obj_ptr     = obj->obj;
  TClass*     obj_cls     = obj->cls;
  std::string args = lval_to_cpp_arg(e, a, 0);
  lval_del(name); lval_del(obj); lval_del(a);

  // Snapshot gPad before probe calls, which may trigger ROOT event processing
  // (via TCanvas::Update in the REPL loop) that resets gPad to the main canvas.
  // Restored just before actual method execution so that Draw/DrawClone go to
  // the pad that was active when the method call was issued.
  // Exception: cd() is intentionally changing gPad, so we don't restore for it.
  TVirtualPad* const pad_snapshot = (method_name != "cd") ? gPad : nullptr;

  if (g_debug)
    std::cout << "Executing " << method_name << "(" << args
              << ") on " << class_name << " @" << obj_ptr << std::endl;

  // Helper: evaluate base_expr, use typeid to get TClass (works for template
  // instantiations), then heap-copy the result.
  // on_void() is called (and lval_qexpr returned) if the return type is void.
  // If the expression fails to compile (e.g. pointer arg where reference is
  // expected), automatically retries with TOBJ pointer args dereferenced:
  //   ((Type*)0xADDR)  →  (*((Type*)0xADDR))
  auto cling_new_auto_typed = [&](const std::string& orig_expr,
                                   std::function<void()> on_void) -> lval* {
    static std::atomic<int> rut_tmp_n{0};

    // Try to find a compilable form of expr. Tries up to four combinations:
    //   1. direct(orig_args)   — e.g. ((Cls*)ptr)->Method(((ArgCls*)p))
    //   2. direct(deref_args)  — e.g. ((Cls*)ptr)->Method(*((ArgCls*)p))   [ref mismatch]
    //   3. arrow(orig_args)    — e.g. (*((Cls*)ptr))->Method(((ArgCls*)p)) [smart-ptr]
    //   4. arrow(deref_args)   — e.g. (*((Cls*)ptr))->Method(*((ArgCls*)p))
    // Returns {best_expr, alias} — alias may be undefined if all forms fail.
    // tobj_arg_re: matches ((Type*)0xHEX) in argument position (not followed by ->)
    static const std::regex tobj_arg_re(R"(\(\([^)*]+\*\)(0x[0-9a-fA-F]+)\)(?!->))");
    // tobj_obj_re: matches (capture)-> at object position; $1 = ((Type*)0xHEX) without ->
    static const std::regex tobj_obj_re(R"((\(\([^)*]+\*\)(0x[0-9a-fA-F]+)\))->)");
    auto probe = [&](const std::string& e) -> std::pair<std::string,std::string> {
      std::string n  = std::to_string(rut_tmp_n++);
      std::string al = "__rut_t" + n;
      gInterpreter->ProcessLine(("using " + al + " = decltype(" + e + ");").c_str());
      // sizeof(void) is invalid — use conditional to treat void as char for the size check
      bool ok = (bool)(Long_t)gInterpreter->Calc(
          ("(Long_t)sizeof(std::conditional_t<std::is_void<" + al + ">::value,char," + al + ">)").c_str());
      if (g_debug) std::cout << "Cling probe (" << (ok?"ok":"fail") << "): " << e << std::endl;
      return {ok ? e : "", al};
    };
    auto deref_args = [&](const std::string& e){ return std::regex_replace(e, tobj_arg_re, "(*$&)"); };
    // arrow_form: dereference object pointer so operator-> is called: ((Cls*)ptr)-> → (*((Cls*)ptr))->
    auto arrow_form = [&](const std::string& e){ return std::regex_replace(e, tobj_obj_re, "(*$1)->"); };

    // If the object class exposes operator->() (smart-pointer pattern), the method
    // is likely on the pointee rather than the class itself — skip direct Cling forms
    // and go straight to the arrow forms.  This avoids slow Cling error-recovery on
    // deep template types (e.g. RResultPtr<TH1D>::DrawClone).
    // For regular ROOT objects (TCanvas, TH1, RInterface, …) there is no operator->,
    // so try_direct stays true and direct dispatch is used as usual.
    bool try_direct = !obj_cls || obj_cls->GetMethodAny("operator->") == nullptr;

    auto pick_expr = [&](const std::string& orig) -> std::pair<std::string,std::string> {
      if (try_direct) {
        // 1. direct, orig args
        auto [e1, al1] = probe(orig);
        if (!e1.empty()) return {e1, al1};
        // 2. direct, deref args (handles pointer-where-reference-expected)
        std::string da = deref_args(orig);
        if (da != orig) {
          auto [e2, al2] = probe(da);
          if (!e2.empty()) return {e2, al2};
        }
      }
      // 3. arrow (operator->), orig args (handles smart-pointer dispatch)
      std::string ar = arrow_form(orig);
      if (ar != orig) {
        auto [e3, al3] = probe(ar);
        if (!e3.empty()) return {e3, al3};
        // 4. arrow + deref args
        std::string ar_da = deref_args(ar);
        if (ar_da != ar) {
          auto [e4, al4] = probe(ar_da);
          if (!e4.empty()) return {e4, al4};
        }
      }
      // All forms failed — return orig with whatever alias (likely undefined)
      std::string n = std::to_string(rut_tmp_n++);
      std::string al = "__rut_t" + n;
      gInterpreter->ProcessLine(("using " + al + " = decltype(" + orig + ");").c_str());
      return {orig, al};
    };

    auto [base_expr, alias] = pick_expr(orig_expr);
    std::string n_str = std::to_string(rut_tmp_n++);
    std::string var   = "__rut_r" + n_str;

    // Restore the pad that was active when builtin_member was entered.
    // Probe calls and ROOT's post-eval Update() can reset gPad; restoring here
    // ensures Draw/DrawClone etc. target the pad the user cd'd to.
    // TObject::DrawClone uses gROOT->GetSelectedPad() (not gPad) to decide
    // where to draw, and TPad::cd(0) does NOT call gROOT->SetSelectedPad().
    // So we must set both explicitly.
    if (pad_snapshot) {
      pad_snapshot->cd();
      gROOT->SetSelectedPad(pad_snapshot);
    }

    bool is_void = (bool)(Long_t)gInterpreter->Calc(
        ("(Long_t)std::is_void<" + alias + ">::value").c_str());
    if (is_void) {
      // Use base_expr (best compilable form) rather than on_void() which may
      // use the original form before pick_expr selected the arrow/deref variant.
      gInterpreter->ProcessLine((base_expr + ";").c_str());
      return lval_qexpr();
    }

    // If the return type is a pointer, get it directly and use typeid on the
    // dereferenced value; don't heap-copy a pointer with new auto().
    bool is_ptr = (bool)(Long_t)gInterpreter->Calc(
        ("(Long_t)std::is_pointer<" + alias + ">::value").c_str());
    if (is_ptr) {
      std::string decl = alias + " " + var + " = " + base_expr + ";";
      if (g_debug) std::cout << "Cling ptr decl: " << decl << std::endl;
      gInterpreter->ProcessLine(decl.c_str());
      void* result = (void*)gInterpreter->Calc(("(Long_t)" + var).c_str());
      if (!result)
        return lval_err("Method '%s' returned null", method_name.c_str());
      TClass* ret_cls = (TClass*)gInterpreter->Calc(
          ("(Long_t)TClass::GetClass(typeid(*" + var + "))").c_str());
      return lval_tobj(result, ret_cls);
    }

    // Store result in an auto variable (strips reference qualifiers from return type).
    std::string decl = "auto " + var + " = " + base_expr + ";";
    if (g_debug) std::cout << "Cling decl: " << decl << std::endl;
    gInterpreter->ProcessLine(decl.c_str());

    // Check scalar types on the auto-deduced (ref-stripped) variable type.
    bool is_integral = (bool)(Long_t)gInterpreter->Calc(
        ("(Long_t)std::is_integral<decltype(" + var + ")>::value").c_str());
    if (is_integral) {
      Long_t ival = gInterpreter->Calc(("(Long_t)" + var).c_str());
      return lval_num((long)ival);
    }

    bool is_fp = (bool)(Long_t)gInterpreter->Calc(
        ("(Long_t)std::is_floating_point<decltype(" + var + ")>::value").c_str());
    if (is_fp) {
      std::string fvar = "__rut_f" + n_str;
      gInterpreter->ProcessLine(("double " + fvar + " = (double)" + var + ";").c_str());
      Long_t raw = gInterpreter->Calc(("*reinterpret_cast<Long_t*>(&" + fvar + ")").c_str());
      double dval; memcpy(&dval, &raw, sizeof(dval));
      return lval_floating(dval);
    }
    TClass* ret_cls = (TClass*)gInterpreter->Calc(
        ("(Long_t)TClass::GetClass(typeid(" + var + "))").c_str());
    void* result = (void*)gInterpreter->Calc(
        ("(Long_t)(new auto(" + var + "))").c_str());
    if (!result)
      return lval_err("Method '%s' returned null", method_name.c_str());
    return lval_tobj(result, ret_cls);
  };

  if (has_callable) {
    // TMethodCall can't handle callable args — build a full Cling expression.
    std::string base = "((" + class_name + "*)" + ptr_to_hex(obj_ptr) + ")->"
                     + method_name + "(" + args + ")";
    if (g_debug) std::cout << "Cling base: " << base << std::endl;
    return cling_new_auto_typed(base,
      [&]{ gInterpreter->ProcessLine((base + ";").c_str()); });
  }

  TMethodCall mc(obj_cls, method_name.c_str(), args.c_str());
  if (!mc.IsValid()) {
    // Method not found directly — try smart-pointer dereference via operator->().
    // First try TMethodCall (works for non-template classes).
    TMethodCall mc_arrow(obj_cls, "operator->", "");
    if (mc_arrow.IsValid() && mc_arrow.ReturnType() == TMethodCall::kLong) {
      Long_t inner_ptr = 0;
      mc_arrow.Execute(obj_ptr, inner_ptr);
      TMethod* arrow_m = obj_cls->GetMethodAny("operator->");
      std::string inner_type = arrow_m ? arrow_m->GetReturnTypeName() : "";
      while (!inner_type.empty() && (inner_type.back() == '*' || inner_type.back() == ' '))
        inner_type.pop_back();
      TClass* inner_cls = TClass::GetClass(inner_type.c_str());
      if (inner_cls && inner_ptr) {
        obj_ptr    = (void*)inner_ptr;
        obj_cls    = inner_cls;
        class_name = inner_cls->GetName();
        mc         = TMethodCall(inner_cls, method_name.c_str(), args.c_str());
      }
    }
    if (!mc.IsValid()) {
      // TMethodCall can't resolve methods on template classes — use Cling.
      // pick_expr inside cling_new_auto_typed tries direct, arg-deref, arrow,
      // and arrow+arg-deref forms automatically.
      std::string fallback_base = "((" + class_name + "*)" + ptr_to_hex(obj_ptr) + ")->"
                                + method_name + "(" + args + ")";
      if (g_debug) std::cout << "Cling fallback: " << fallback_base << std::endl;
      return cling_new_auto_typed(fallback_base,
        [&]{ gInterpreter->ProcessLine((fallback_base + ";").c_str()); });
    }
  }

  // Restore gPad and the canvas selected pad before the actual method execute.
  // The smart-pointer dereference via mc_arrow.Execute above (and any probe
  // calls) may have left gPad in the wrong state; restoring here ensures
  // Draw/DrawClone (which uses gROOT->GetSelectedPad()) target the pad that
  // was active when this member call was issued.
  if (pad_snapshot) {
    pad_snapshot->cd();
    gROOT->SetSelectedPad(pad_snapshot);
  }

  switch (mc.ReturnType()) {
    case TMethodCall::kLong: {
      // Could be an integral or a raw pointer
      Long_t ret = 0;
      mc.Execute(obj_ptr, ret);
      TMethod* m = obj_cls ? obj_cls->GetMethodAny(method_name.c_str()) : nullptr;
      std::string retname = m ? m->GetReturnTypeName() : "";
      if (!retname.empty() && retname.back() == '*') {
        // Raw pointer return — strip '*' and look up class
        std::string bare = retname.substr(0, retname.size() - 1);
        // trim trailing space
        while (!bare.empty() && bare.back() == ' ') bare.pop_back();
        TClass* ret_cls = TClass::GetClass(bare.c_str());
        if (!ret_cls && ret != 0) {
          // Name lookup failed — recover dynamic type via Cling typeid
          static std::atomic<int> rut_ptr_n{0};
          std::string pvar = "__rut_p" + std::to_string(rut_ptr_n++);
          gInterpreter->ProcessLine(
            ("auto* " + pvar + " = (" + bare + "*)(Long_t)" + std::to_string((Long_t)ret) + ";").c_str());
          ret_cls = (TClass*)gInterpreter->Calc(
            ("(Long_t)TClass::GetClass(typeid(*" + pvar + "))").c_str());
        }
        return lval_tobj((void*)ret, ret_cls);
      }
      return lval_num((long)ret);
    }
    case TMethodCall::kDouble: {
      Double_t ret = 0;
      mc.Execute(obj_ptr, ret);
      return lval_floating(ret);
    }
    case TMethodCall::kOther: {
      // kOther can fire for void returns in some ROOT versions — check first.
      TMethod* m = obj_cls ? obj_cls->GetMethodAny(method_name.c_str()) : nullptr;
      std::string retname = m ? m->GetReturnTypeName() : "";
      if (retname == "void") {
        mc.Execute(obj_ptr);
        return lval_qexpr();
      }
      // Non-void value-type return — use typeid path for correct TClass
      // even for template methods where GetMethodAny may not resolve.
      std::string base = "((" + class_name + "*)" + ptr_to_hex(obj_ptr) + ")->"
                       + method_name + "(" + args + ")";
      return cling_new_auto_typed(base,
        [&]{ mc.Execute(obj_ptr); });
    }
    default:  // kNone (void) or kString
      mc.Execute(obj_ptr);
      return lval_qexpr();
  }
}

// (:: Method ClassName args...)  — static method call
// Sugar: (::Method ClassName args...) desugars to the above in lval_eval_sexpr.
// Return type dispatch:
//   void    → empty Q-expression
//   pointer → TOBJ with dynamic TClass (via typeid)
//   float   → LVAL_FLOAT (bits smuggled through Long_t)
//   other   → LVAL_NUM (integral / enum)
lval* builtin_static(lenv* e, lval* a) {
  LASSERT(a, a->count >= 2,
    "Function '::' needs at least 2 arguments: <method> and <class>.");
  LASSERT_TYPE("::", a, 0, LVAL_STR);
  LASSERT_TYPE("::", a, 1, LVAL_STR);

  std::string method_name = a->cell[0]->str;
  std::string class_name  = a->cell[1]->str;
  lval_del(lval_pop(a, 0));
  lval_del(lval_pop(a, 0));
  std::string args = lval_to_cpp_arg(e, a, 0);
  lval_del(a);

  std::string base  = class_name + "::" + method_name + "(" + args + ")";
  static std::atomic<int> rut_static_n{0};
  std::string ns    = std::to_string(rut_static_n++);
  std::string alias = "__rut_sT" + ns;

  std::string type_decl = "using " + alias + " = decltype(" + base + ");";
  if (g_debug) std::cout << "static type-check: " << type_decl << std::endl;
  gInterpreter->ProcessLine(type_decl.c_str());

  auto calc_bool = [&](const std::string& expr) -> bool {
    return (bool)(Long_t)gInterpreter->Calc(("(Long_t)" + expr).c_str());
  };

  if (calc_bool("std::is_void<"           + alias + ">::value")) {
    gInterpreter->ProcessLine((base + ";").c_str());
    return lval_qexpr();
  }

  if (calc_bool("std::is_pointer<" + alias + ">::value")) {
    std::string var = "__rut_sP" + ns;
    gInterpreter->ProcessLine(("auto* " + var + " = " + base + ";").c_str());
    void* result = (void*)gInterpreter->Calc(("(Long_t)" + var).c_str());
    if (!result)
      return lval_err("Static '%s::%s' returned null",
                      class_name.c_str(), method_name.c_str());
    TClass* ret_cls = (TClass*)gInterpreter->Calc(
        ("(Long_t)TClass::GetClass(typeid(*" + var + "))").c_str());
    return lval_tobj(result, ret_cls);
  }

  if (calc_bool("std::is_floating_point<" + alias + ">::value")) {
    std::string var = "__rut_sD" + ns;
    gInterpreter->ProcessLine(("double " + var + " = (double)(" + base + ");").c_str());
    Long_t raw = gInterpreter->Calc(
        ("*reinterpret_cast<Long_t*>(&" + var + ")").c_str());
    double dval; memcpy(&dval, &raw, sizeof(dval));
    return lval_floating(dval);
  }

  // Value-type class/struct return (e.g. RDataFrame, RNode) — heap-allocate.
  // Construct directly from the expression so move-only types (like RDataFrame)
  // are move-constructed rather than copy-constructed.
  if (calc_bool("std::is_class<" + alias + ">::value")) {
    std::string var = "__rut_sV" + ns;
    gInterpreter->ProcessLine(
        ("auto* " + var + " = new " + alias + "(" + base + ");").c_str());
    void* result = (void*)gInterpreter->Calc(("(Long_t)" + var).c_str());
    if (!result)
      return lval_err("Static '%s::%s' returned null",
                      class_name.c_str(), method_name.c_str());
    TClass* ret_cls = (TClass*)gInterpreter->Calc(
        ("(Long_t)TClass::GetClass(typeid(*" + var + "))").c_str());
    return lval_tobj(result, ret_cls);
  }

  // Integral / enum / other scalar
  Long_t ival = gInterpreter->Calc(("(Long_t)(" + base + ")").c_str());
  return lval_num((long)ival);
}

lval* promote_to_floating(lval *a) {
  lval *f = lval_floating((double)a->num);
  lval_del(a);
  return f;
}

void best_numeric_type(lval *&x, lval *&y) {
  // If x is a integer and y is a double, promote x to be double.
  // If x is a double and y is an integer, promote y.
  // We never demote while doing math.
  if (x->type == LVAL_NUM && y->type == LVAL_FLOAT)
    x = promote_to_floating(x);
  if (x->type == LVAL_FLOAT && y->type == LVAL_NUM)
    y = promote_to_floating(y);
}

lval* builtin_op(lenv *e, lval* a, const char* op) {
  /* Ensure all arguments are numbers */
  for (int i = 0; i < a->count; i++) {
    LASSERT(a, a->cell[i]->type == LVAL_NUM 
               || a->cell[i]->type == LVAL_FLOAT,
      "Cannot operate on non-number!");
  }
  
  /* Pop the first element */
  lval* x = lval_pop(a, 0);

  /* If no arguments and sub then perform unary negation */
  if ((strcmp(op, "-") == 0) && a->count == 0) {
    switch (x->type) {
      case LVAL_NUM: x->num = -x->num; break;
      case LVAL_FLOAT: x->floating = -x->floating; break;
    }
  }

  /* While there are still elements remaining */
  while (a->count > 0) {

    /* Pop the next element */
    lval* y = lval_pop(a, 0);

    best_numeric_type(x, y);

    switch(x->type) {
      case LVAL_NUM:
        if (strcmp(op, "+") == 0) { x->num += y->num; }
        if (strcmp(op, "-") == 0) { x->num -= y->num; }
        if (strcmp(op, "*") == 0) { x->num *= y->num; }
        if (strcmp(op, "/") == 0) {
          if (y->num == 0) {
            lval_del(x); lval_del(y);
            x = lval_err("Division By Zero!"); break;
          }
          x->num /= y->num;
        }
      break;
      case LVAL_FLOAT:
        if (strcmp(op, "+") == 0) { x->floating += y->floating; }
        if (strcmp(op, "-") == 0) { x->floating -= y->floating; }
        if (strcmp(op, "*") == 0) { x->floating *= y->floating; }
        if (strcmp(op, "/") == 0) {
          if (y->floating == 0) {
            lval_del(x); lval_del(y);
            x = lval_err("Division By Zero!"); break;
          }
          x->floating /= y->floating;
        }
      break;
    }
    lval_del(y);
  }

  lval_del(a); return x;
}

lval* builtin_ord(lenv* e, lval* a, const char* op) {
  LASSERT_NUM(op, a, 2);
  for (int i = 0; i < a->count; i++) {
    LASSERT(a, a->cell[i]->type == LVAL_NUM 
               || a->cell[i]->type == LVAL_FLOAT,
      "Cannot operate on non-number!");
  }
  
  best_numeric_type(a->cell[0], a->cell[1]);
  
  int r;
  // Since we already promoted types, we can
  // only check one of the arguments.
  switch(a->cell[0]->type) {
    case LVAL_NUM:
      if (strcmp(op, ">")  == 0) {
        r = (a->cell[0]->num >  a->cell[1]->num);
      }
      if (strcmp(op, "<")  == 0) {
        r = (a->cell[0]->num <  a->cell[1]->num);
      }
      if (strcmp(op, ">=") == 0) {
        r = (a->cell[0]->num >= a->cell[1]->num);
      }
      if (strcmp(op, "<=") == 0) {
        r = (a->cell[0]->num <= a->cell[1]->num);
      }
      lval_del(a);
      return lval_num(r);
    case LVAL_FLOAT:
      lval_del(a);
      if (strcmp(op, ">")  == 0) {
        r = (a->cell[0]->floating >  a->cell[1]->floating);
      }
      if (strcmp(op, "<")  == 0) {
        r = (a->cell[0]->floating <  a->cell[1]->floating);
      }
      if (strcmp(op, ">=") == 0) {
        r = (a->cell[0]->floating >= a->cell[1]->floating);
      }
      if (strcmp(op, "<=") == 0) {
        r = (a->cell[0]->floating <= a->cell[1]->floating);
      }
      return lval_num(r);
    default:
      return lval_err("Guru Meditation");
  }
}

lval* builtin_gt(lenv* e, lval* a) {
  return builtin_ord(e, a, ">");
}

lval* builtin_lt(lenv* e, lval* a) {
  return builtin_ord(e, a, "<");
}

lval* builtin_ge(lenv* e, lval* a) {
  return builtin_ord(e, a, ">=");
}

lval* builtin_le(lenv* e, lval* a) {
  return builtin_ord(e, a, "<=");
}

lval* builtin_cmp(lenv* e, lval* a, const char* op) {
  LASSERT_NUM(op, a, 2);

  best_numeric_type(a->cell[0], a->cell[1]);
  int r;
  if (strcmp(op, "==") == 0) {
    r =  lval_eq(a->cell[0], a->cell[1]);
  }
  if (strcmp(op, "!=") == 0) {
    r = !lval_eq(a->cell[0], a->cell[1]);
  }
  lval_del(a);
  return lval_num(r);
}

lval* builtin_eq(lenv* e, lval* a) {
  return builtin_cmp(e, a, "==");
}

lval* builtin_ne(lenv* e, lval* a) {
  return builtin_cmp(e, a, "!=");
}

lval* builtin_if(lenv* e, lval* a) {
  LASSERT_NUM("if", a, 3);
  LASSERT_TYPE("if", a, 0, LVAL_NUM);
  LASSERT_TYPE("if", a, 1, LVAL_QEXPR);
  LASSERT_TYPE("if", a, 2, LVAL_QEXPR);
  
  /* Mark Both Expressions as evaluable */
  lval* x;
  a->cell[1]->type = LVAL_SEXPR;
  a->cell[2]->type = LVAL_SEXPR;
  
  if (a->cell[0]->num) {
    /* If condition is true evaluate first expression */
    x = lval_eval(e, lval_pop(a, 1));
  } else {
    /* Otherwise evaluate second expression */
    x = lval_eval(e, lval_pop(a, 2));
  }
  
  /* Delete argument list and return */
  lval_del(a);
  return x;
}


lval* builtin_add(lenv* e, lval* a) {
  return builtin_op(e, a, "+");
}

lval* builtin_sub(lenv* e, lval* a) {
  return builtin_op(e, a, "-");
}

lval* builtin_mul(lenv* e, lval* a) {
  return builtin_op(e, a, "*");
}

lval* builtin_div(lenv* e, lval* a) {
  return builtin_op(e, a, "/");
}

lval* builtin_var(lenv* e, lval* a, const char* func) {
  LASSERT_TYPE(func, a, 0, LVAL_QEXPR);
  
  lval* syms = a->cell[0];
  for (int i = 0; i < syms->count; i++) {
    LASSERT(a, (syms->cell[i]->type == LVAL_SYM),
      "Function '%s' cannot define non-symbol. "
      "Got %s, Expected %s.", func, 
      ltype_name(syms->cell[i]->type),
      ltype_name(LVAL_SYM));
  }
  
  LASSERT(a, (syms->count == a->count-1),
    "Function '%s' passed too many arguments for symbols. "
    "Got %i, Expected %i.", func, syms->count, a->count-1);
    
  for (int i = 0; i < syms->count; i++) {
    /* If 'def' define in globally. If 'put' define in locally */
    if (strcmp(func, "def") == 0) {
      lenv_def(e, syms->cell[i], a->cell[i+1]);
    }
    
    if (strcmp(func, "=")   == 0) {
      lenv_put(e, syms->cell[i], a->cell[i+1]);
    } 
  }
  
  lval_del(a);
  return lval_sexpr();
}

lval* builtin_def(lenv* e, lval* a) {
  return builtin_var(e, a, "def");
}

lval* builtin_put(lenv* e, lval* a) {
  return builtin_var(e, a, "=");
}

int lval_eq(lval* x, lval* y) {

  /* Different Types are always unequal */
  if (x->type != y->type) { return 0; }

  /* Compare Based upon type */
  switch (x->type) {
    /* Compare Number Value */
    case LVAL_NUM: return (x->num == y->num);
    case LVAL_FLOAT: return (x->floating == y->floating);

    /* Compare String Values */
    case LVAL_ERR: return (strcmp(x->err, y->err) == 0);
    case LVAL_SYM: return (strcmp(x->sym, y->sym) == 0);

    /* If builtin compare, otherwise compare formals and body */
    case LVAL_FUN:
      if (x->builtin || y->builtin) {
        return x->builtin == y->builtin;
      } else {
        return lval_eq(x->formals, y->formals) 
          && lval_eq(x->body, y->body);
      }
    case LVAL_STR: return (strcmp(x->str, y->str) == 0);

    /* If list compare every individual element */
    case LVAL_QEXPR:
    case LVAL_SEXPR:
      if (x->count != y->count) { return 0; }
      for (int i = 0; i < x->count; i++) {
        /* If any element not equal then whole list not equal */
        if (!lval_eq(x->cell[i], y->cell[i])) { return 0; }
      }
      /* Otherwise lists must be equal */
      return 1;
    break;
  }
  return 0;
}



lval* builtin_lambda(lenv* e, lval* a) {
  /* Check Two arguments, each of which are Q-Expressions */
  LASSERT_NUM("\\", a, 2);
  LASSERT_TYPE("\\", a, 0, LVAL_QEXPR);
  LASSERT_TYPE("\\", a, 1, LVAL_QEXPR);
  
  /* Check first Q-Expression contains only Symbols */
  for (int i = 0; i < a->cell[0]->count; i++) {
    LASSERT(a, (a->cell[0]->cell[i]->type == LVAL_SYM),
      "Cannot define non-symbol. Got %s, Expected %s.",
      ltype_name(a->cell[0]->cell[i]->type),ltype_name(LVAL_SYM));
  }
  
  /* Pop first two arguments and pass them to lval_lambda */
  lval* formals = lval_pop(a, 0);
  lval* body = lval_pop(a, 0);
  lval_del(a);
  
  return lval_lambda(formals, body);
}

void lenv_add_builtin(lenv* e, const char* name, lbuiltin func) {
  lval* k = lval_sym(name);
  lval* v = lval_fun(func);
  lenv_put(e, k, v);
  lval_del(k); lval_del(v);
}

void lenv_add_global_object(lenv* e, const char* name, void *obj, TClass *cls) {
  lval* k = lval_sym(name);
  lval* v = lval_tobj(obj, cls);
  lenv_put(e, k, v);
  lval_del(k); lval_del(v);
}

static std::vector<std::string> load_path;
static std::map<std::string, std::string> g_annotations;

// Callback registry for (connect widget signal lambda)
struct RutCallback { lval* fn; lenv* env; };
static std::map<int, RutCallback> g_callbacks;
static int  g_next_callback_id       = 0;

// Pipe used to defer callback execution out of Cling's slot-dispatch context.
// Calling Cling (via new/method-call builtins) from within a Cling-dispatched
// slot causes re-entrancy that silently corrupts results.  Instead we write the
// callback id to the pipe; a TFileHandler drains it in the next event-loop
// iteration, when Cling is idle.
static int g_cb_pipe[2] = {-1, -1};

// Executed by the Cling shim inside TQObject::Connect slot dispatch.
// Must not call Cling itself — only a pipe write.
extern "C" void rooture_fire_callback(int id) {
  if (g_cb_pipe[1] != -1)
    ::write(g_cb_pipe[1], &id, sizeof(id));
}

// TFileHandler that drains the callback pipe and runs rooture lambdas.
class RutCallbackHandler : public TFileHandler {
public:
  lenv* e;
  explicit RutCallbackHandler(int fd, lenv* env)
    : TFileHandler(fd, /*mask=*/1), e(env) {}
  Bool_t ReadNotify() override {
    // Drain the pipe and execute only the *last* queued invocation per
    // callback id.  This debounces rapid signals (e.g. slider drags) so
    // slow callbacks don't pile up while the user is still interacting.
    int id;
    std::vector<int> pending;
    while (::read(GetFd(), &id, sizeof(id)) == sizeof(id))
      pending.push_back(id);
    // Walk backwards, fire each id only once (the most recent occurrence).
    std::set<int> fired;
    for (int i = (int)pending.size() - 1; i >= 0; i--) {
      int cb_id = pending[i];
      if (!fired.insert(cb_id).second) continue;
      auto it = g_callbacks.find(cb_id);
      if (it == g_callbacks.end()) continue;
      lval* fn   = it->second.fn;
      lval* args = lval_sexpr();
      lval* result = lval_call(e, lval_copy(fn), args);
      if (result->type == LVAL_ERR)
        fprintf(stderr, "callback error: %s\n", result->err);
      lval_del(result);
    }
    // Flush canvas redraws triggered by the callbacks.
    TIter next(gROOT->GetListOfCanvases());
    TVirtualPad* c;
    while ((c = (TVirtualPad*)next())) c->Update();
    // Flush GUI widget redraws (e.g. TGLabel::SetText → NeedRedraw).
    // Update(2) calls TGClient::DoRedraw() via TGCocoa's friend access;
    // Update(1) flushes the resulting Cocoa command buffer to the screen.
    // (Mirrors what TMacOSXSystem::DispatchOneEvent does after Cocoa events.)
    if (gVirtualX) { gVirtualX->Update(2); gVirtualX->Update(1); }
    return kTRUE;
  }
  Bool_t Notify() override { return ReadNotify(); }
};

static std::string executable_dir() {
#ifdef __APPLE__
  char buf[PATH_MAX];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0)
    return gSystem->DirName(buf);
#else
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) { buf[n] = '\0'; return gSystem->DirName(buf); }
#endif
  return ".";
}

/* Count net open parens/braces, ignoring strings and ; comments */
static int paren_depth(const std::string& s) {
  int depth = 0;
  bool in_string = false;
  bool in_comment = false;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (in_comment) {
      if (c == '\n') in_comment = false;
    } else if (in_string) {
      if (c == '\\') i++;           // skip escaped char
      else if (c == '"') in_string = false;
    } else {
      if      (c == '"')            in_string = true;
      else if (c == ';')            in_comment = true;
      else if (c == '(' || c == '{') depth++;
      else if (c == ')' || c == '}') depth--;
    }
  }
  return depth;
}

lval* builtin_load(lenv* e, lval* a) {
  LASSERT_NUM("load", a, 1);
  LASSERT_TYPE("load", a, 0, LVAL_STR);
  
  /* Resolve filename against load_path if not directly accessible */
  std::string filename = a->cell[0]->str;
  char* expanded = gSystem->ExpandPathName(filename.c_str());
  filename = expanded;
  free(expanded);
  if (gSystem->AccessPathName(filename.c_str())) {
    for (const auto& dir : load_path) {
      std::string candidate = dir + "/" + filename;
      if (!gSystem->AccessPathName(candidate.c_str())) {
        filename = candidate;
        break;
      }
    }
  }

  /* Parse File given by string name */
  mpc_result_t r;
  if (mpc_parse_contents(filename.c_str(), Lispy, &r)) {
    
    /* Read contents */
    lval* expr = lval_read((mpc_ast_t *)r.output);
    mpc_ast_delete((mpc_ast_t *)r.output);

    /* Evaluate each Expression */
    while (expr->count) {
      lval* x = lval_eval(e, lval_pop(expr, 0));
      /* If Evaluation leads to error print it */
      if (x->type == LVAL_ERR) { lval_println(x); }
      lval_del(x);
    }
    
    /* Delete expressions and arguments */
    lval_del(expr);    
    lval_del(a);
    
    /* Return empty list */
    return lval_sexpr();
    
  } else {
    /* Get Parse Error as String */
    char* err_msg = mpc_err_string(r.error);
    mpc_err_delete(r.error);
    
    /* Create new error message using it */
    lval* err = lval_err("Could not load Library %s", err_msg);
    free(err_msg);
    lval_del(a);
    
    /* Cleanup and return error */
    return err;
  }
}

lval* builtin_print(lenv* e, lval* a) {

  /* Print each argument followed by a space */
  for (int i = 0; i < a->count; i++) {
    lval_print(a->cell[i]); rut_print(" ");
  }

  /* Print a newline and delete arguments */
  rut_print("\n");
  lval_del(a);

  return lval_sexpr();
}

lval* builtin_error(lenv* e, lval* a) {
  LASSERT_NUM("error", a, 1);
  LASSERT_TYPE("error", a, 0, LVAL_STR);

  /* Construct Error from first argument */
  lval* err = lval_err(a->cell[0]->str);

  /* Delete arguments and return */
  lval_del(a);
  return err;
}

lval* builtin_exit(lenv* e, lval* a) {
  LASSERT_NUM("exit", a, 1);
  LASSERT_TYPE("exit", a, 0, LVAL_NUM);

  exit(a->cell[0]->num);

  /* Delete arguments and return */
  lval_del(a);
  return 0;
}

// Thread-first operator  (-> val step1 step2 ...)
// Each step is a Q-expression in one of two forms:
//   {. Method extra-args...}     — classic form
//   {.Method extra-args...}      — shorthand; desugared by lval_eval_sexpr
// An optional alias as the first symbol before the dot binds the result globally:
//   {alias . Method extra-args...}
//   {alias .Method extra-args...}
lval* builtin_arrow(lenv* e, lval* a) {
  LASSERT(a, a->count >= 1, "'->' requires at least one argument.");
  lval* acc = lval_copy(a->cell[0]);

  for (int s = 1; s < a->count; s++) {
    lval* step = a->cell[s];
    LASSERT(a, step->type == LVAL_QEXPR && step->count >= 1,
            "'->' steps must be non-empty Q-expressions.");

    // Detect alias: first element is a symbol that is NOT "." and does NOT start with "."
    int fn_idx = 0;
    const char* alias = nullptr;
    if (step->cell[0]->type == LVAL_SYM) {
      const char* s0 = step->cell[0]->sym;
      if (s0[0] != '.') { alias = s0; fn_idx = 1; }
    }

    // Build the call sexpr: (fn_sym [method] acc extra-args...)
    // fn_sym is either "." (classic) or ".Method" (shorthand, desugared later)
    lval* call = lval_sexpr();
    lval_add(call, lval_copy(step->cell[fn_idx]));          // "." or ".Method"

    // For classic {. Method ...}: also prepend the explicit method name
    if (fn_idx + 1 < step->count) {
      const char* fn_sym = step->cell[fn_idx]->sym;
      if (fn_sym[0] == '.' && fn_sym[1] == '\0') {
        // classic dot — next element is the method name
        lval_add(call, lval_copy(step->cell[fn_idx + 1]));  // method name
        lval_add(call, lval_copy(acc));                      // object
        for (int i = fn_idx + 2; i < step->count; i++)
          lval_add(call, lval_copy(step->cell[i]));
      } else {
        // shorthand .Method — object goes right after fn_sym
        lval_add(call, lval_copy(acc));
        for (int i = fn_idx + 1; i < step->count; i++)
          lval_add(call, lval_copy(step->cell[i]));
      }
    } else {
      // Only the function symbol, no extra args (e.g. {.DrawClone} or {. DrawClone})
      lval_add(call, lval_copy(acc));
    }

    lval_del(acc);
    acc = lval_eval(e, call);
    if (acc->type == LVAL_ERR) return acc;

    if (alias) {
      lval* key = lval_sym(alias);
      lenv_def(e, key, acc);
      lval_del(key);
    }
  }

  return acc;
}

// (doto obj {Method args...} {Method2 args...} ...)
// Calls each method on obj in sequence; returns obj.
// Steps with first element ">>" thread the result: {>> {GetAxis} {SetTitle "x"}}.
lval* builtin_doto(lenv* e, lval* a) {
  LASSERT(a, a->count >= 1, "'doto' requires at least one argument.");
  lval* obj = lval_copy(a->cell[0]);

  for (int s = 1; s < a->count; s++) {
    lval* step = a->cell[s];
    LASSERT(a, step->type == LVAL_QEXPR && step->count >= 1,
            "'doto' steps must be non-empty Q-expressions.");

    if (step->cell[0]->type == LVAL_SYM &&
        strcmp(step->cell[0]->sym, ">>") == 0) {
      // Threading pipeline: thread obj through sub-steps, discard result
      lval* cur = lval_copy(obj);
      for (int i = 1; i < step->count; i++) {
        lval* sub = step->cell[i];
        LASSERT(a, sub->type == LVAL_QEXPR && sub->count >= 1,
                "'doto' '>>' sub-steps must be non-empty Q-expressions.");
        lval* call = lval_sexpr();
        lval_add(call, lval_sym("."));
        lval_add(call, lval_copy(sub->cell[0]));  // method name
        lval_add(call, cur);                        // current object
        for (int j = 1; j < sub->count; j++)
          lval_add(call, lval_copy(sub->cell[j]));
        cur = lval_eval(e, call);
        if (cur->type == LVAL_ERR) { lval_del(obj); return cur; }
      }
      lval_del(cur);
    } else {
      // Normal step: call method on obj, discard result
      lval* call = lval_sexpr();
      lval_add(call, lval_sym("."));
      lval_add(call, lval_copy(step->cell[0]));  // method name
      lval_add(call, lval_copy(obj));              // object
      for (int i = 1; i < step->count; i++)
        lval_add(call, lval_copy(step->cell[i]));
      lval* result = lval_eval(e, call);
      if (result->type == LVAL_ERR) { lval_del(obj); return result; }
      lval_del(result);
    }
  }

  return obj;
}

// Creates a new C++ object
lval *builtin_new(lenv *e, lval* a) {
  LASSERT(a, a->count >= 1,
    "Function 'new' needs at least 1 argument: <class name>.");
  LASSERT_TYPE("new", a, 0, LVAL_STR);
  std::string className = a->cell[0]->str;  // copy before lval_del
  TClass *cls = TClass::GetClass(className.c_str());
  if (!cls) {
    lval_del(a);
    return lval_err("Unknown class '%s'", className.c_str());
  }
  std::string args = lval_to_cpp_arg(e, a, 1);
  lval_del(a);

  // Try direct args first; if that fails (e.g. pointer where reference
  // expected), retry with TOBJ pointer args dereferenced: ((T*)p) → (*((T*)p))
  static const std::regex ctor_arg_re(R"(\(\([^)*]+\*\)(0x[0-9a-fA-F]+)\)(?!->))");
  auto try_ctor = [&](const std::string& ctorArgs) -> void* {
    std::string expr = "new " + className + "(" + ctorArgs + ");";
    return (void*)gInterpreter->Calc(expr.c_str());
  };
  void *obj = try_ctor(args);
  if (!obj) {
    std::string derefed = std::regex_replace(args, ctor_arg_re, "(*$&)");
    if (derefed != args)
      obj = try_ctor(derefed);
  }
  if (!obj)
    return lval_err("Constructor failed for '%s'", className.c_str());
  // Use typeid-based TClass lookup so template instantiations resolve correctly
  TClass* real_cls = (TClass*)gInterpreter->Calc(
      ("(Long_t)TClass::GetClass(typeid(*((" + className + "*)" +
       ptr_to_hex(obj) + ")))").c_str());
  return lval_tobj(obj, real_cls ? real_cls : cls);
}

// Invokes a method
lval *builtin_invoke(lenv *e, lval *a) {
  LASSERT_NUM("invoke", a, 2);
  LASSERT_TYPE("invoke", a, 0, LVAL_TMETHOD);
  LASSERT_TYPE("invoke", a, 1, LVAL_TOBJ);
  TMethodCall *m = a->cell[0]->method;
  const char *args = a->cell[0]->methodArgs;
  if (g_debug) {
    rut_print("Return type is %i\n", m->ReturnType());
    rut_print("Executing method %s(%s)\n", m->GetMethodName(), args);
  }
  m->Execute(a->cell[1]->obj, args);
  return lval_qexpr();
}

// Returns all user-defined symbol names from the global env as a Q-expression of strings.
lval* builtin_symbols(lenv* e, lval* a) {
  lval_del(a);
  lenv* top = e;
  while (top->par) top = top->par;
  lval* result = lval_qexpr();
  for (int i = 0; i < top->count; i++) {
    if (top->vals[i]->type == LVAL_FUN) continue;
    lval_add(result, lval_str(top->syms[i]));
  }
  return result;
}

// Returns names of all open TCanvas objects as a Q-expression of strings.
lval* builtin_canvases(lenv* e, lval* a) {
  lval_del(a);
  lval* result = lval_qexpr();
  TIter next(gROOT->GetListOfCanvases());
  TObject* obj;
  while ((obj = next()))
    lval_add(result, lval_str(obj->GetName()));
  return result;
}

// (save-window <frame-obj> "/path/to/out.png") — captures a TGFrame window to PNG
// using TASImage::FromWindow (libASImage loaded on demand).
lval* builtin_save_window(lenv* e, lval* a) {
  LASSERT(a, a->count == 2, "'save-window' requires 2 arguments: <frame> <path>.");
  LASSERT(a, a->cell[0]->type == LVAL_TOBJ,
          "'save-window' argument 1 must be a ROOT TGFrame object.");
  LASSERT_TYPE("save-window", a, 1, LVAL_STR);

  void* ptr  = a->cell[0]->obj;
  const char* path = a->cell[1]->str;

  gSystem->Load("libASImage");

  // Execute via Cling to avoid compile-time dependency on TGFrame/TASImage headers.
  // FromWindow is an instance method, not static — create a TASImage first.
  std::string code = Form(
    "{ TGFrame* __wf = (TGFrame*)((void*)%lldLL);"
    "  TASImage* __img = new TASImage();"
    "  __img->FromWindow(__wf->GetId(), 0, 0, 0, 0);"
    "  __img->WriteImage(\"%s\"); delete __img; }",
    (long long)ptr, path);
  gInterpreter->ProcessLine(code.c_str());

  lval* res = lval_str(path);
  lval_del(a);
  return res;
}

// (save-png "CanvasName" "/path/to/out.png") — saves a canvas to a PNG file.
lval* builtin_save_png(lenv* e, lval* a) {
  LASSERT(a, a->count == 2, "'save-png' requires 2 arguments: <canvas-name> <path>.");
  LASSERT_TYPE("save-png", a, 0, LVAL_STR);
  LASSERT_TYPE("save-png", a, 1, LVAL_STR);
  const char* name = a->cell[0]->str;
  const char* path = a->cell[1]->str;
  TObject* obj = gROOT->GetListOfCanvases()->FindObject(name);
  if (!obj) { lval_del(a); return lval_err("Canvas '%s' not found", name); }
  TVirtualPad* pad = dynamic_cast<TVirtualPad*>(obj);
  if (!pad) { lval_del(a); return lval_err("'%s' is not a pad", name); }
  pad->SaveAs(path);
  lval* res = lval_str(path);
  lval_del(a);
  return res;
}

// (global "gClient") — resolve a C++ global by name via Cling, wrap it as a
// rooture object and bind it in the current environment.
// The global must be TObject-derived so its dynamic type can be found via IsA().
lval* builtin_global(lenv* e, lval* a) {
  LASSERT_NUM("global", a, 1);
  LASSERT_TYPE("global", a, 0, LVAL_STR);

  const char* name = a->cell[0]->str;

  TInterpreter::EErrorCode err = TInterpreter::kNoError;
  Long_t addr = gInterpreter->Calc(Form("(Long_t)(TObject*)%s", name), &err);
  if (err != TInterpreter::kNoError || !addr) {
    lval_del(a);
    return lval_err("'global': '%s' is null or not found", name);
  }

  TObject* obj = (TObject*)addr;
  TClass*  cls = obj->IsA();
  lenv_add_global_object(e, name, obj, cls);

  lval* result = lval_tobj(obj, cls);
  lval_del(a);
  return result;
}

// (connect widget "Signal()" lambda) — wire a ROOT signal to a rooture lambda.
// Each call mints a unique Cling shim __rooture_cb_N() that fires the lambda.
lval* builtin_connect(lenv* e, lval* a) {
  LASSERT_NUM("connect", a, 3);
  LASSERT_TYPE("connect", a, 0, LVAL_TOBJ);
  LASSERT_TYPE("connect", a, 1, LVAL_STR);
  LASSERT(a, a->cell[2]->type == LVAL_FUN,
          "'connect' third argument must be a function");

  TObject*    widget = (TObject*)a->cell[0]->obj;
  const char* signal = a->cell[1]->str;
  int         id     = g_next_callback_id++;

  g_callbacks[id] = { lval_copy(a->cell[2]), e };

  // Declare a per-callback struct with a static method so that
  // TQSlot's ClassInfo_Init succeeds (global functions fail because
  // ClassInfo_Factory() without Init is invalid for SetFuncProto).
  // Embed the function pointer directly to avoid any global-variable
  // initialisation ordering issues.
  typedef void (*FireFn)(int);
  FireFn fn_ptr = rooture_fire_callback;
  const char* cls_name = Form("__RutSlot_%d", id);
  bool decl_ok = gInterpreter->Declare(Form(
    "struct %s { static void fire() { ((void(*)(int))%lldLL)(%d); } };",
    cls_name, (long long)(void*)fn_ptr, id));
  if (!decl_ok) {
    lval_del(a);
    return lval_err("'connect': failed to declare callback class %s", cls_name);
  }

  // DynamicCast TObject* → TQObject* via ROOT reflection.
  TClass* tqobj_cls = TClass::GetClass("TQObject");
  void*   tqobj_ptr = a->cell[0]->cls->DynamicCast(tqobj_cls, widget, kTRUE);
  if (!tqobj_ptr) {
    lval_del(a);
    return lval_err("'connect': widget does not inherit from TQObject");
  }
  TQObject* sender = static_cast<TQObject*>(tqobj_ptr);
  // Connect with the struct as receiver_class and static method as slot.
  bool ok = TQObject::Connect(sender, signal, cls_name, nullptr, "fire()");

  lval_del(a);
  if (!ok) return lval_err("'connect': TQObject::Connect failed for signal '%s'", signal);
  return lval_num(id);
}

// (str val) — convert a number (or string) to its string representation.
lval* builtin_str(lenv* e, lval* a) {
  LASSERT_NUM("str", a, 1);
  char buf[64];
  lval* v = a->cell[0];
  if (v->type == LVAL_STR) {
    lval* s = lval_str(v->str);
    lval_del(a); return s;
  } else if (v->type == LVAL_NUM) {
    snprintf(buf, sizeof(buf), "%ld", v->num);
  } else if (v->type == LVAL_FLOAT) {
    snprintf(buf, sizeof(buf), "%g", v->floating);
  } else {
    lval_del(a);
    return lval_err("'str': cannot convert %s to string", ltype_name(v->type));
  }
  lval* s = lval_str(buf);
  lval_del(a);
  return s;
}

// (concat str ...) — concatenate any number of strings.
lval* builtin_concat(lenv* e, lval* a) {
  std::string result;
  for (int i = 0; i < a->count; i++) {
    if (a->cell[i]->type == LVAL_STR)
      result += a->cell[i]->str;
    else {
      lval_del(a);
      return lval_err("'concat' requires string arguments, got %s at index %d",
                      ltype_name(a->cell[i]->type), i);
    }
  }
  lval* s = lval_str(result.c_str());
  lval_del(a);
  return s;
}

// (process-line "code") — execute a C++ statement via gInterpreter->ProcessLine
// directly from native C++, bypassing rooture's Cling method-dispatch path.
// This avoids nested ProcessLine calls (Cling re-entrancy) that would freeze
// the GUI when called from the event loop or MCP handler.
lval* builtin_process_line(lenv* e, lval* a) {
  LASSERT_NUM("process-line", a, 1);
  LASSERT_TYPE("process-line", a, 0, LVAL_STR);
  const char* code = a->cell[0]->str;
  TInterpreter::EErrorCode err = TInterpreter::kNoError;
  Long_t ret = gInterpreter->ProcessLine(code, &err);
  lval_del(a);
  if (err != TInterpreter::kNoError)
    return lval_err("process-line: Cling error %d executing: %s", (int)err, code);
  return lval_num(ret);
}

// (annotate sym "text") — attach a documentation string to a symbol name.
// The symbol auto-converts to a string at the call site, so both
//   (annotate myplot "description")  and  (annotate "myplot" "description")
// are accepted.
lval* builtin_annotate(lenv* e, lval* a) {
  LASSERT_NUM("annotate", a, 2);
  LASSERT(a, a->cell[0]->type == LVAL_STR || a->cell[0]->type == LVAL_SYM,
          "'annotate' first argument must be a symbol or string");
  LASSERT_TYPE("annotate", a, 1, LVAL_STR);

  const char* name = (a->cell[0]->type == LVAL_STR) ? a->cell[0]->str : a->cell[0]->sym;
  g_annotations[name] = a->cell[1]->str;

  lval* res = lval_str(a->cell[1]->str);
  lval_del(a);
  return res;
}

// (annotations) — return all annotations as a Q-expression of {name text} pairs.
lval* builtin_annotations(lenv* e, lval* a) {
  lval_del(a);
  lval* result = lval_qexpr();
  for (const auto& kv : g_annotations) {
    lval* pair = lval_qexpr();
    lval_add(pair, lval_str(kv.first.c_str()));
    lval_add(pair, lval_str(kv.second.c_str()));
    lval_add(result, pair);
  }
  return result;
}

void lenv_add_builtins(lenv* e) {
  /* List Functions */
  lenv_add_builtin(e, "list", builtin_list);
  lenv_add_builtin(e, "head", builtin_head);
  lenv_add_builtin(e, "tail", builtin_tail);
  lenv_add_builtin(e, "eval", builtin_eval);
  lenv_add_builtin(e, "do",   builtin_do);
  lenv_add_builtin(e, "join", builtin_join);
  /* Variable Functions */
  lenv_add_builtin(e, "\\",  builtin_lambda);
  lenv_add_builtin(e, "def", builtin_def);
  lenv_add_builtin(e, "=",   builtin_put);

  /* Mathematical Functions */
  lenv_add_builtin(e, "+", builtin_add);
  lenv_add_builtin(e, "-", builtin_sub);
  lenv_add_builtin(e, "*", builtin_mul);
  lenv_add_builtin(e, "/", builtin_div);

  /* Conditionals */
  lenv_add_builtin(e, "if", builtin_if);
  lenv_add_builtin(e, "==", builtin_eq);
  lenv_add_builtin(e, "!=", builtin_ne);
  lenv_add_builtin(e, ">",  builtin_gt);
  lenv_add_builtin(e, "<",  builtin_lt);
  lenv_add_builtin(e, ">=", builtin_ge);
  lenv_add_builtin(e, "<=", builtin_le);

  /*Helpers*/
  lenv_add_builtin(e, "load", builtin_load);
  lenv_add_builtin(e, "error", builtin_error);
  lenv_add_builtin(e, "print", builtin_print);
  lenv_add_builtin(e, "exit", builtin_exit);
  
  /*TObject interaction*/
  lenv_add_builtin(e, "->",   builtin_arrow);
  lenv_add_builtin(e, "doto", builtin_doto);
  lenv_add_builtin(e, "new", builtin_new);
  lenv_add_builtin(e, "member", builtin_member);
  lenv_add_builtin(e, ".", builtin_member);
  lenv_add_builtin(e, "::", builtin_static);
  lenv_add_builtin(e, "invoke", builtin_invoke);
  lenv_add_builtin(e, "global",  builtin_global);
  lenv_add_builtin(e, "connect",      builtin_connect);
  lenv_add_builtin(e, "str",          builtin_str);
  lenv_add_builtin(e, "concat",       builtin_concat);
  lenv_add_builtin(e, "process-line", builtin_process_line);
  lenv_add_builtin(e, "symbols", builtin_symbols);
  lenv_add_builtin(e, "canvases", builtin_canvases);
  lenv_add_builtin(e, "annotate", builtin_annotate);
  lenv_add_builtin(e, "annotations", builtin_annotations);
  lenv_add_builtin(e, "save-png", builtin_save_png);
  lenv_add_builtin(e, "save-window", builtin_save_window);

  /*A few TObjects */
  lenv_add_global_object(e, "gSystem",      gSystem,      TClass::GetClass("TSystem"));
  lenv_add_global_object(e, "gInterpreter", gInterpreter, TClass::GetClass("TInterpreter"));
  lenv_add_global_object(e, "gROOT",        gROOT,        TClass::GetClass("TROOT"));
  lenv_add_global_object(e, "gFile",        gFile,        TClass::GetClass("TFile"));
  lenv_add_global_object(e, "gPad",         gPad,         TClass::GetClass("TVirtualPad"));
  lenv_add_global_object(e, "gDirectory",   gDirectory,   TClass::GetClass("TDirectory"));
  lenv_add_global_object(e, "gRandom",      gRandom,      TClass::GetClass("TRandom"));
  lenv_add_global_object(e, "gStyle",       gStyle,       TClass::GetClass("TStyle"));

  // Register the callable bridge by address so Cling's JIT can call it
  // without relying on symbol export (-rdynamic/-export_dynamic).
  gInterpreter->Declare(
    "typedef double (*__rooture_invoke_t)(const char*, const double*, int);\n"
    "__rooture_invoke_t __rooture_invoke_ptr;");
  gInterpreter->ProcessLine(
    ("__rooture_invoke_ptr = (__rooture_invoke_t)" +
     ptr_to_hex((void*)rooture_invoke_callable_c) + ";").c_str());
}

//----- Pipe-based input handler -----------------------------------------------

class PipeHandler : public TFileHandler {
  lenv* fEnv;
public:
  PipeHandler(lenv* e) : TFileHandler(g_pipe_fds[0], 1), fEnv(e) {}

  Bool_t Notify() override {
    uint32_t len = 0;
    if (read(g_pipe_fds[0], &len, sizeof(len)) != sizeof(len)) return kTRUE;
    if (len == 0) { gApplication->Terminate(0); return kTRUE; }

    std::string expr(len, '\0');
    size_t got = 0;
    while (got < len) {
      ssize_t n = read(g_pipe_fds[0], expr.data() + got, len - got);
      if (n <= 0) break;
      got += n;
    }

    mpc_result_t r;
    const char* src = g_mcp_mode ? "<mcp>" : "<stdin>";

    int saved_stderr_fd = -1;
    int stderr_pipe[2] = {-1, -1};
    if (g_mcp_mode) {
      g_capturing = true;
      g_capture_buf.clear();
      // Redirect stderr to capture Cling diagnostics
      saved_stderr_fd = dup(STDERR_FILENO);
      pipe(stderr_pipe);
      dup2(stderr_pipe[1], STDERR_FILENO);
      close(stderr_pipe[1]);
      stderr_pipe[1] = -1;
    }

    if (mpc_parse(src, expr.c_str(), Lispy, &r)) {
      if (g_debug) mpc_ast_print((mpc_ast_t*)r.output);
      lval* prog = lval_read((mpc_ast_t*)r.output);
      mpc_ast_delete((mpc_ast_t*)r.output);
      /* Evaluate top-level expressions in sequence, print last result. */
      lval* x = lval_sexpr();
      while (prog->count) {
        lval_del(x);
        x = lval_eval(fEnv, lval_pop(prog, 0));
        if (x->type == LVAL_ERR) break;
      }
      lval_del(prog);
      lval_println(x);
      lval_del(x);
    } else {
      char* err_str = mpc_err_string(r.error);
      rut_print("%s\n", err_str);
      free(err_str);
      mpc_err_delete(r.error);
    }

    if (g_mcp_mode) {
      g_capturing = false;

      // Restore stderr and collect any Cling diagnostics
      if (saved_stderr_fd >= 0) {
        fflush(stderr);
        dup2(saved_stderr_fd, STDERR_FILENO);
        close(saved_stderr_fd);
        // Drain the pipe (non-blocking)
        fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);
        char sbuf[4096];
        ssize_t sn;
        std::string diag;
        while ((sn = read(stderr_pipe[0], sbuf, sizeof(sbuf))) > 0)
          diag.append(sbuf, sn);
        close(stderr_pipe[0]);
        if (!diag.empty()) {
          if (!g_capture_buf.empty()) g_capture_buf += "\n";
          g_capture_buf += diag;
        }
      }

      uint32_t rlen = (uint32_t)g_capture_buf.size();
      write(g_mcp_reply_fds[1], &rlen, sizeof(rlen));
      if (rlen > 0) write(g_mcp_reply_fds[1], g_capture_buf.data(), rlen);
    }

    TIter next(gROOT->GetListOfCanvases());
    TVirtualPad* c;
    while ((c = (TVirtualPad*)next())) c->Update();
    // Flush GUI widget redraws (e.g. TGLabel::SetText → NeedRedraw).
    // Update(2) calls TGClient::DoRedraw() via TGCocoa's friend access;
    // Update(1) flushes the resulting Cocoa command buffer to the screen.
    // (Mirrors what TMacOSXSystem::DispatchOneEvent does after Cocoa events.)
    if (gVirtualX) { gVirtualX->Update(2); gVirtualX->Update(1); }
    TInterpreter::Instance()->EndOfLineAction();
    return kTRUE;
  }

  Bool_t ReadNotify() override { return Notify(); }
};

//----- Syntax highlighter -----------------------------------------------------

static const std::vector<std::string> kKeywords = {
  "def", "=", "\\", "if", "fun", "do", "let",
  "->", ".", "::", "new", "load", "error", "print", "println"
};

static void highlight_rooture(std::string const& input,
                               replxx::Replxx::colors_t& colors) {
  using Color = replxx::Replxx::Color;
  if (!g_ts_parser) return;

  TSTree* tree = ts_parser_parse_string(g_ts_parser, nullptr,
                                        input.c_str(), (uint32_t)input.size());
  if (!tree) return;

  TSNode root = ts_tree_root_node(tree);
  TSTreeCursor cursor = ts_tree_cursor_new(root);

  auto color_range = [&](uint32_t start, uint32_t end, Color c) {
    for (uint32_t i = start; i < end && i < colors.size(); ++i)
      colors[i] = c;
  };

  std::function<void()> walk = [&]() {
    TSNode node = ts_tree_cursor_current_node(&cursor);
    if (ts_node_child_count(node) == 0) {
      uint32_t start = ts_node_start_byte(node);
      uint32_t end   = ts_node_end_byte(node);
      std::string_view text(input.data() + start, end - start);
      const char* type = ts_node_type(node);

      if (strcmp(type, "dot_method") == 0) {
        color_range(start, end, Color::BRIGHTGREEN);
      } else if (strcmp(type, "named_ref") == 0) {
        color_range(start, end, Color::MAGENTA);
      } else if (strcmp(type, "number") == 0 || strcmp(type, "float") == 0) {
        color_range(start, end, Color::CYAN);
      } else if (strcmp(type, "string") == 0) {
        color_range(start, end, Color::YELLOW);
      } else if (strcmp(type, "comment") == 0) {
        color_range(start, end, Color::BROWN);
      } else if (strcmp(type, "(") == 0 || strcmp(type, ")") == 0 ||
                 strcmp(type, "{") == 0 || strcmp(type, "}") == 0) {
        color_range(start, end, Color::WHITE);
      } else if (strcmp(type, "symbol") == 0) {
        if (text.find("::") != std::string_view::npos) {
          color_range(start, end, Color::BRIGHTBLUE);
        } else {
          bool is_kw = std::find(kKeywords.begin(), kKeywords.end(), text)
                       != kKeywords.end();
          color_range(start, end, is_kw ? Color::BRIGHTGREEN : Color::LIGHTGRAY);
        }
      }
    }

    if (ts_tree_cursor_goto_first_child(&cursor)) {
      walk();
      while (ts_tree_cursor_goto_next_sibling(&cursor)) walk();
      ts_tree_cursor_goto_parent(&cursor);
    }
  };

  walk();
  ts_tree_cursor_delete(&cursor);
  ts_tree_delete(tree);
}

//----- Completion callback ----------------------------------------------------

static replxx::Replxx::completions_t complete_rooture(
    std::string const& input, int& context_len)
{
  replxx::Replxx::completions_t completions;

  // Find the start of the current token
  int pos = (int)input.size() - 1;
  while (pos >= 0 && input[pos] != ' ' && input[pos] != '(' && input[pos] != ')' &&
         input[pos] != '{' && input[pos] != '}')
    --pos;
  std::string word = input.substr(pos + 1);

  if (word.empty() || word[0] != '@') return completions;

  std::string prefix = word.substr(1); // strip '@'
  context_len = (int)word.size();

  // Collect matching names as strings first, then deduplicate
  std::vector<std::string> names;
  auto add_from_list = [&](TCollection* lst) {
    if (!lst) return;
    TIter next(lst);
    TObject* obj;
    while ((obj = next())) {
      const char* raw = obj->GetName();
      if (!raw || raw[0] == '\0' || strcmp(raw, obj->ClassName()) == 0) continue;
      std::string name(raw);
      if (prefix.empty() || name.substr(0, prefix.size()) == prefix)
        names.push_back("@" + name);
    }
  };

  add_from_list(gROOT->GetListOfCanvases());
  add_from_list(gROOT->GetListOfFiles());
  add_from_list(gROOT->GetListOfSpecials());
  if (gDirectory) add_from_list(gDirectory->GetList());

  std::sort(names.begin(), names.end());
  names.erase(std::unique(names.begin(), names.end()), names.end());
  for (auto const& n : names) completions.push_back(n);

  return completions;
}

//----- Input thread -----------------------------------------------------------

static void input_thread_fn() {
  replxx::Replxx rx;
  rx.set_max_history_size(1000);
  g_rx = &rx;

  g_ts_parser = ts_parser_new();
  ts_parser_set_language(g_ts_parser, tree_sitter_rooture());
  rx.set_highlighter_callback(highlight_rooture);
  rx.set_completion_callback(complete_rooture);
  rx.set_word_break_characters(" \t\n\r(){}\"");

  rx.print("ROOTure 0.1.0\nPress Ctrl+c to interrupt, Ctrl+d to exit\n\n");

  // Load history
  const char* home = getenv("HOME");
  std::string hist_file;
  if (home) { hist_file = std::string(home) + "/.rooture_history"; rx.history_load(hist_file); }

  auto send_expr = [](const std::string& expr) {
    uint32_t len = (uint32_t)expr.size();
    write(g_pipe_fds[1], &len, sizeof(len));
    write(g_pipe_fds[1], expr.data(), len);
  };

  std::string accumulated;
  int open_depth = 0;

  while (true) {
    const char* prompt = accumulated.empty() ? "ROOTure> " : "      .. ";
    const char* input = rx.input(prompt);
    if (!input) {
      // EOF (Ctrl+D)
      uint32_t eof = 0;
      write(g_pipe_fds[1], &eof, sizeof(eof));
      break;
    }

    std::string line(input);

    // Count parens/braces to support multi-line input
    for (char ch : line) {
      if (ch == '(' || ch == '{') open_depth++;
      else if (ch == ')' || ch == '}') open_depth--;
    }

    if (accumulated.empty()) accumulated = line;
    else accumulated += "\n" + line;

    if (open_depth <= 0) {
      open_depth = 0;
      if (!accumulated.empty() && accumulated.find_first_not_of(" \t\n\r") != std::string::npos) {
        rx.history_add(accumulated);
        send_expr(accumulated);
      }
      accumulated.clear();
    }
  }

  if (!hist_file.empty()) rx.history_save(hist_file);

  ts_parser_delete(g_ts_parser);
  g_ts_parser = nullptr;
  g_rx = nullptr;
}

//----- MCP stdio server -------------------------------------------------------

static std::string base64_encode(const std::vector<uint8_t>& data) {
  static const char* b64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < data.size(); i += 3) {
    uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i+1] << 8) | data[i+2];
    out += b64[(v >> 18) & 63]; out += b64[(v >> 12) & 63];
    out += b64[(v >>  6) & 63]; out += b64[v & 63];
  }
  if (i < data.size()) {
    uint32_t v = (uint32_t)data[i] << 16;
    if (i + 1 < data.size()) v |= (uint32_t)data[i+1] << 8;
    out += b64[(v >> 18) & 63];
    out += b64[(v >> 12) & 63];
    out += (i + 1 < data.size()) ? b64[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

static void mcp_thread_fn() {
  using json = nlohmann::json;

  // Helper: send eval expression to main thread, block until result arrives.
  auto eval_expr = [](const std::string& expr) -> std::string {
    uint32_t len = (uint32_t)expr.size();
    write(g_pipe_fds[1], &len, sizeof(len));
    write(g_pipe_fds[1], expr.data(), len);
    uint32_t rlen = 0;
    if (read(g_mcp_reply_fds[0], &rlen, sizeof(rlen)) != sizeof(rlen)) return "";
    std::string result(rlen, '\0');
    size_t got = 0;
    while (got < rlen) {
      ssize_t n = read(g_mcp_reply_fds[0], result.data() + got, rlen - got);
      if (n <= 0) break;
      got += n;
    }
    return result;
  };

  // Helper: write one JSON-RPC response line to the saved MCP output stream.
  // Use error_handler_t::replace so invalid UTF-8 in error messages never
  // throws and kills the process.
  auto send_resp = [](const json& resp) {
    std::string s = resp.dump(-1, ' ', false,
                              json::error_handler_t::replace) + "\n";
    fwrite(s.c_str(), 1, s.size(), g_mcp_out);
    fflush(g_mcp_out);
  };

  json tools = json::array({
    {{"name", "eval"},
     {"description", "Evaluate a rooture expression in ROOT (Lisp-like syntax)."},
     {"inputSchema", {{"type","object"},
       {"properties", {{"expr", {{"type","string"},{"description","Expression to evaluate"}}}}},
       {"required", {"expr"}}}}},
    {{"name", "list_symbols"},
     {"description", "List all user-defined symbols in the rooture environment."},
     {"inputSchema", {{"type","object"},{"properties",json::object()},{"required",json::array()}}}},
    {{"name", "list_canvases"},
     {"description", "List names of all open ROOT TCanvas objects."},
     {"inputSchema", {{"type","object"},{"properties",json::object()},{"required",json::array()}}}},
    {{"name", "get_canvas"},
     {"description", "Return a ROOT TCanvas as a PNG image."},
     {"inputSchema", {{"type","object"},
       {"properties", {{"name", {{"type","string"},{"description","Canvas name"}}}}},
       {"required", {"name"}}}}},
    {{"name", "get_window"},
     {"description", "Capture a ROOT GUI window (TGFrame / TGMainFrame) as a PNG image. Pass the rooture symbol name that holds the window object (e.g. \"win\")."},
     {"inputSchema", {{"type","object"},
       {"properties", {{"symbol", {{"type","string"},{"description","Rooture symbol name of the TGFrame variable"}}}}},
       {"required", {"symbol"}}}}},
    {{"name", "list_annotations"},
     {"description", "List all symbol annotations set via (annotate sym \"text\"). Returns {name annotation} pairs describing user-defined customisation points."},
     {"inputSchema", {{"type","object"},{"properties",json::object()},{"required",json::array()}}}},
    {{"name", "reload"},
     {"description", "Quit the rooture MCP server so the host can restart it with a freshly built binary. Call this after rebuilding rooture, then reconnect with /mcp."},
     {"inputSchema", {{"type","object"},{"properties",json::object()},{"required",json::array()}}}},
  });

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) continue;
    json req;
    try { req = json::parse(line); } catch (...) { continue; }

    json id   = req.contains("id") ? req["id"] : json(nullptr);
    std::string method = req.value("method", "");

    if (method == "initialize") {
      send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
        {"protocolVersion","2024-11-05"},
        {"capabilities",{{"tools",json::object()}}},
        {"serverInfo",{{"name","rooture"},{"version","0.1.0"}}}
      }}});
    } else if (method == "initialized") {
      /* notification — no response */
    } else if (method == "tools/list") {
      send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{{"tools",tools}}}});
    } else if (method == "tools/call") {
      std::string tool = req["params"].value("name","");
      json args = req["params"].value("arguments", json::object());

      if (tool == "eval") {
        std::string expr = args.value("expr","");
        std::string result = eval_expr(expr);
        send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
          {"content", json::array({{{"type","text"},{"text",result}}})}
        }}});

      } else if (tool == "list_symbols") {
        std::string result = eval_expr("(symbols)");
        send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
          {"content", json::array({{{"type","text"},{"text",result}}})}
        }}});

      } else if (tool == "list_annotations") {
        std::string result = eval_expr("(annotations)");
        send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
          {"content", json::array({{{"type","text"},{"text",result}}})}
        }}});

      } else if (tool == "list_canvases") {
        std::string result = eval_expr("(canvases)");
        send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
          {"content", json::array({{{"type","text"},{"text",result}}})}
        }}});

      } else if (tool == "get_canvas") {
        std::string name = args.value("name","");
        std::string tmp = "/tmp/rooture_canvas_" + name + ".png";
        // Escape name/path for the rooture string literal
        eval_expr("(save-png \"" + name + "\" \"" + tmp + "\")");
        // Read PNG file and base64-encode
        std::ifstream f(tmp, std::ios::binary);
        std::vector<uint8_t> png((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
        if (png.empty()) {
          send_resp({{"jsonrpc","2.0"},{"id",id},{"error",{
            {"code",-32000},{"message","Failed to save canvas '"+name+"' as PNG"}
          }}});
        } else {
          std::string b64 = base64_encode(png);
          send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
            {"content", json::array({{{"type","image"},{"data",b64},{"mimeType","image/png"}}})}
          }}});
        }
        std::remove(tmp.c_str());

      } else if (tool == "get_window") {
        std::string symbol = args.value("symbol","");
        std::string tmp = "/tmp/rooture_window_" + symbol + ".png";
        // Flush pending window-manager events so MapRaised takes effect
        // before we capture the screen contents.
        eval_expr("(process-line \"for(int _i=0;_i<5;_i++) gSystem->ProcessEvents();\")");
        eval_expr("(save-window " + symbol + " \"" + tmp + "\")");
        std::ifstream wf(tmp, std::ios::binary);
        std::vector<uint8_t> png((std::istreambuf_iterator<char>(wf)),
                                  std::istreambuf_iterator<char>());
        if (png.empty()) {
          send_resp({{"jsonrpc","2.0"},{"id",id},{"error",{
            {"code",-32000},{"message","Failed to capture window '"+symbol+"' as PNG"}
          }}});
        } else {
          std::string b64 = base64_encode(png);
          send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
            {"content", json::array({{{"type","image"},{"data",b64},{"mimeType","image/png"}}})}
          }}});
        }
        std::remove(tmp.c_str());

      } else if (tool == "reload") {
        send_resp({{"jsonrpc","2.0"},{"id",id},{"result",{
          {"content", json::array({{{"type","text"},{"text","rooture MCP server exiting for reload."}}})}
        }}});
        fflush(g_mcp_out);
        std::exit(0);

      } else {
        send_resp({{"jsonrpc","2.0"},{"id",id},{"error",{
          {"code",-32601},{"message","Unknown tool: "+tool}
        }}});
      }
    }
    // Other methods (e.g. ping, notifications) are silently ignored.
  }

  // EOF on stdin — signal main thread to exit.
  uint32_t eof = 0;
  write(g_pipe_fds[1], &eof, sizeof(eof));
}

int main(int argc, char** argv) {
  const char* g_script_file = nullptr;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--debug") == 0)
      g_debug = true;
    else if (strcmp(argv[i], "--mcp") == 0)
      g_mcp_mode = true;
    else if (argv[i][0] != '-' && !g_script_file)
      g_script_file = argv[i];
  }

  /* In MCP mode save the real stdout fd for JSON-RPC, then redirect fd 1 to
     stderr so that ROOT internals (TEveManager, TApplication, etc.) cannot
     corrupt the JSON-RPC stream with their own print statements.  All ROOT
     output will appear on the terminal's stderr where it is still useful. */
  if (g_mcp_mode) {
    int saved = dup(STDOUT_FILENO);
    g_mcp_out = fdopen(saved, "w");
    dup2(STDERR_FILENO, STDOUT_FILENO);
  }

  /* Create Some Parsers */
  Floating  = mpc_new("floating");
  Number    = mpc_new("number");
  Symbol    = mpc_new("symbol");
  String    = mpc_new("string");
  Comment   = mpc_new("comment");
  Inlinestr = mpc_new("inlinestr");
  QexprItem = mpc_new("qexpr_item");
  Qexpr     = mpc_new("qexpr");
  Sexpr     = mpc_new("sexpr");
  Expr      = mpc_new("expr");
  Lispy     = mpc_new("lispy");

  /* Define them with the following Language */
  mpca_lang(MPCA_LANG_DEFAULT,
    "                                                         \
      floating : /-?[0-9]+[.][0-9]*/                          \
               | /-?[.][0-9]+/ ;                              \
      number   : /-?[0-9]+/ ;                                 \
      symbol   : /[a-zA-Z0-9_+\\-*\\/\\\\=<>!&.:@~]+/ ;          \
      string   : /\"(\\\\.|[^\"])*\"/ ;                       \
      comment    : /;[^\\r\\n]*/ ;                            \
      inlinestr  : /[^}\\n]*/ ;                               \
      qexpr_item : '|' <inlinestr> | <expr> ;                 \
      sexpr      : '(' <expr>* ')' ;                          \
      qexpr      : '{' <qexpr_item>* '}' ;                    \
      expr       : <floating> | <number> | <symbol>           \
                 | <string> | <comment> | <sexpr> | <qexpr>;  \
      lispy    : /^/ <expr>* /$/ ;                            \
    ",
  Floating, Number, Symbol, String, Comment, Inlinestr, QexprItem, Sexpr, Qexpr, Expr, Lispy);

  /* Build the file search path */
  std::string exe_dir = executable_dir();
  load_path.push_back(exe_dir + "/../share/rooture");

  /* The environment */
  lenv* e = lenv_new();
  lenv_add_builtins(e);

  /* Set up the communication pipe between input thread and main thread */
  if (pipe(g_pipe_fds) != 0) { perror("pipe"); return 1; }

  /* In MCP mode create the reply pipe (main→MCP thread) */
  if (g_mcp_mode) {
    if (pipe(g_mcp_reply_fds) != 0) { perror("pipe(mcp_reply)"); return 1; }
  }

  /* Bootstrap TApplication so ROOT graphics/gSystem work */
  TApplication app("rooture", &argc, argv);

  /* Callback pipe: rooture lambdas connected to ROOT signals are deferred here
     so they run in the event loop, not inside Cling's slot-dispatch context. */
  if (pipe(g_cb_pipe) == 0) {
    fcntl(g_cb_pipe[0], F_SETFL, O_NONBLOCK);
    gSystem->AddFileHandler(new RutCallbackHandler(g_cb_pipe[0], e));
  }

  /* Script mode: load a file and exit without starting the event loop */
  if (g_script_file) {
    mpc_result_t r;
    if (mpc_parse_contents(g_script_file, Lispy, &r)) {
      lval* expr = lval_read((mpc_ast_t*)r.output);
      mpc_ast_delete((mpc_ast_t*)r.output);
      while (expr->count) {
        lval* x = lval_eval(e, lval_pop(expr, 0));
        if (x->type == LVAL_ERR) { lval_println(x); lval_del(x); lval_del(expr); exit(1); }
        lval_del(x);
      }
      lval_del(expr);
    } else {
      char* err_msg = mpc_err_string(r.error);
      mpc_err_delete(r.error);
      fprintf(stderr, "%s\n", err_msg);
      free(err_msg);
      exit(1);
    }
    exit(0);
  }

  /* Register the pipe read-end with gSystem's event loop */
  PipeHandler* ph = new PipeHandler(e);
  ph->Add();

  /* Start the appropriate I/O thread */
  std::thread io_thread(g_mcp_mode ? mcp_thread_fn : input_thread_fn);

  /* Run ROOT's event loop on the main thread */
  gSystem->Run();

  io_thread.join();

  ph->Remove();
  delete ph;

  lenv_del(e);

  /* Undefine and delete our parsers */
  mpc_cleanup(11,
    Number, Floating, Symbol, String, Comment, Inlinestr, QexprItem,
    Sexpr,  Qexpr,  Expr,   Lispy);

  return 0;
}
