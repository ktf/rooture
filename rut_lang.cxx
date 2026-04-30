#include "rooture.h"
#include <cmath>

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

// Thread identity: true on threads spawned by (future ...).
thread_local bool g_in_future = false;

// ---------------------------------------------------------------------------
// Atom — thread-safe mutable reference (Clojure-style)
// ---------------------------------------------------------------------------
struct RutAtom {
  std::mutex mu;
  lval*      val;
  explicit RutAtom(lval* v) : val(v) {}
  ~RutAtom() { lval_del(val); }
};
using RutAtomPtr = std::shared_ptr<RutAtom>;

lval* lval_atom(lval* init) {
  lval* v = (lval*)malloc(sizeof(lval));
  v->type = LVAL_ATOM;
  v->obj  = new RutAtomPtr(std::make_shared<RutAtom>(init));
  return v;
}

// ---------------------------------------------------------------------------
// Future — deferred value evaluated on a background thread
// ---------------------------------------------------------------------------
struct RutFuture {
  std::mutex              mu;
  std::condition_variable cv;
  bool                    realized   = false;
  bool                    is_promise = false; // true → manually fulfilled via deliver
  lval*                   result     = nullptr;

  ~RutFuture() {
    if (is_promise) {
      // Promises have no background thread; just free any delivered result.
      if (result) lval_del(result);
      return;
    }
    // Future: drain the Cling queue and wait for the worker thread to finish.
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

static lval* lval_future_new(RutFuturePtr rf) {
  lval* v = (lval*)malloc(sizeof(lval));
  v->type = LVAL_FUTURE;
  v->obj  = new RutFuturePtr(std::move(rf));
  return v;
}

static lval* lval_promise_new() {
  auto rf = std::make_shared<RutFuture>();
  rf->is_promise = true;
  lval* v = (lval*)malloc(sizeof(lval));
  v->type = LVAL_PROMISE;
  v->obj  = new RutFuturePtr(std::move(rf));
  return v;
}

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

/* Flatten the entire env chain into one self-contained env with no parent.
 * Innermost bindings win (they are inserted last and overwrite outer ones).
 * Used by builtin_future so the background thread holds no raw parent pointers
 * into lambda envs that will be freed before the thread finishes. */
static lenv* lenv_snapshot(lenv* e) {
  /* Collect chain from innermost to outermost. */
  std::vector<lenv*> chain;
  for (lenv* cur = e; cur; cur = cur->par) chain.push_back(cur);

  lenv* snap = lenv_new();  /* flat, no parent */
  lval key;
  key.type = LVAL_SYM;

  /* Process outermost first so inner bindings overwrite outer ones. */
  for (int i = (int)chain.size() - 1; i >= 0; i--) {
    lenv* level = chain[i];
    for (int j = 0; j < level->count; j++) {
      key.sym = level->syms[j];
      lenv_put(snap, &key, level->vals[j]);
    }
  }
  return snap;
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

lval* lval_jitfn(const char* name, long nparams) {
  lval* v = (lval*)malloc(sizeof(lval));
  v->type    = LVAL_JITFN;
  v->sym     = strdup(name);
  v->num     = nparams;
  v->builtin = nullptr;
  return v;
}

// sym=kernel_name  num=n_inputs  count=n_outputs  obj=dispatch_ptr
lval* lval_coljitfn(const char* name, int n_inputs, int n_outputs, void* dispatch_ptr) {
  lval* v    = (lval*)calloc(1, sizeof(lval));
  v->type    = LVAL_COLJITFN;
  v->sym     = strdup(name);
  v->num     = n_inputs;
  v->count   = n_outputs;
  v->obj     = dispatch_ptr;
  return v;
}

ColJitFnDispatch g_coljitfn_dispatch = nullptr;

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
    case LVAL_JITFN:    return "JitFn";
    case LVAL_COLJITFN: return "ColJitFn";
    case LVAL_ATOM:    return "Atom";
    case LVAL_FUTURE:  return "Future";
    case LVAL_PROMISE: return "Promise";
    case LVAL_COLUMN:  return "Column";
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
    case LVAL_JITFN:    free(v->sym); break;
    case LVAL_COLJITFN: free(v->sym); break;  // obj is a function ptr, not owned heap
    case LVAL_ATOM:    delete (RutAtomPtr*)v->obj;   break;
    case LVAL_FUTURE:
    case LVAL_PROMISE: delete (RutFuturePtr*)v->obj; break;
    case LVAL_COLUMN:
      delete (RutColumnPtr*)v->obj;
      break;
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
    case LVAL_JITFN:    rut_print("<jit-fn %s/%ld>", v->sym, v->num); break;
    case LVAL_COLJITFN: rut_print("<col-jit-fn %s/%ld→%d>", v->sym, v->num, v->count); break;
    case LVAL_COLUMN: {
      auto& cp = *(RutColumnPtr*)v->obj;
      rut_print("<column: %zu entries [%s]>", cp->n,
                cp->dtype == COL_FLOAT32 ? "f32" :
                cp->dtype == COL_FLOAT64 ? "f64" :
                cp->dtype == COL_INT32   ? "i32" :
                cp->dtype == COL_UINT8   ? "u8"  : "?");
    } break;
    case LVAL_ATOM: {
      RutAtomPtr& ap = *(RutAtomPtr*)v->obj;
      std::lock_guard<std::mutex> lock(ap->mu);
      rut_print("<atom ");
      lval_print(ap->val);
      rut_print(">");
    } break;
    case LVAL_FUTURE: {
      RutFuturePtr& fp = *(RutFuturePtr*)v->obj;
      std::lock_guard<std::mutex> lock(fp->mu);
      rut_print(fp->realized ? "<future: realized>" : "<future: pending>");
    } break;
    case LVAL_PROMISE: {
      RutFuturePtr& fp = *(RutFuturePtr*)v->obj;
      std::lock_guard<std::mutex> lock(fp->mu);
      if (fp->realized) {
        rut_print("<promise: ");
        lval_print(fp->result);
        rut_print(">");
      } else {
        rut_print("<promise: pending>");
      }
    } break;
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
    case LVAL_JITFN:
      x->sym = strdup(v->sym); x->num = v->num; break;
    case LVAL_COLJITFN:
      x->sym = strdup(v->sym); x->num = v->num; x->count = v->count; x->obj = v->obj; break;
    case LVAL_ATOM:
      /* Atoms are reference types: copy shares the same RutAtom. */
      x->obj = new RutAtomPtr(*(RutAtomPtr*)v->obj); break;
    case LVAL_FUTURE:
    case LVAL_PROMISE:
      /* Futures and promises are reference types: copy shares the same RutFuture. */
      x->obj = new RutFuturePtr(*(RutFuturePtr*)v->obj); break;
    case LVAL_COLUMN:
      /* Columns are reference types: copy shares the same RutColumn buffer. */
      x->obj = new RutColumnPtr(*(RutColumnPtr*)v->obj);
      break;

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

  /* LVAL_COLJITFN — dispatch to rut_column.cxx handler */
  if (f->type == LVAL_COLJITFN) {
    if (g_coljitfn_dispatch) return g_coljitfn_dispatch(f, a);
    lval_del(a);
    return lval_err("col-jit-fn: dispatch not registered");
  }

  /* LVAL_JITFN — generate a ProcessLine call with serialised arguments */
  if (f->type == LVAL_JITFN) {
    std::string call = std::string(f->sym) + "(";
    for (int i = 0; i < a->count; i++) {
      if (i) call += ", ";
      lval* arg = a->cell[i];
      switch (arg->type) {
        case LVAL_NUM:
          call += std::to_string(arg->num); break;
        case LVAL_FLOAT: {
          char b[32]; snprintf(b, sizeof(b), "%.17g", arg->floating);
          call += b; break;
        }
        case LVAL_STR:
          call += "\"" + escape_for_cling_str(arg->str) + "\""; break;
        case LVAL_TOBJ: {
          std::string cn = arg->cls ? std::string(arg->cls->GetName()) : "void";
          call += "((" + cn + "*)" + ptr_to_hex(arg->obj) + ")"; break;
        }
        default: call += "0"; break;
      }
    }
    call += ")";
    rut_process_line(call.c_str());
    lval_del(a);
    return lval_sexpr();
  }

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
    /* Typed formal {type name} — unwrap to the name symbol for binding */
    if (sym->type == LVAL_QEXPR && sym->count == 2 &&
        sym->cell[1]->type == LVAL_SYM) {
      lval* name = lval_copy(sym->cell[1]);
      lval_del(sym);
      sym = name;
    }
    /* Special Case to deal with '&' */
    if (sym->type == LVAL_SYM && strcmp(sym->sym, "&") == 0) {

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
    /* If the single element is a callable, invoke it with zero arguments. */
    if (x->type == LVAL_FUN || x->type == LVAL_JITFN || x->type == LVAL_COLJITFN) {
      lval* result = lval_call(e, x, lval_sexpr());
      lval_del(x);
      return result;
    }
    return x;
  }

  /* Ensure first element is a callable after evaluation */
  lval* f = lval_pop(v, 0);
  if (f->type != LVAL_FUN && f->type != LVAL_JITFN && f->type != LVAL_COLJITFN) {
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

lval* promote_to_floating(lval *a) {
  lval *f = lval_floating((double)a->num);
  lval_del(a);
  return f;
}

void best_numeric_type(lval *&x, lval *&y) {
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
        if (strcmp(op, "%") == 0) {
          if (y->num == 0) {
            lval_del(x); lval_del(y);
            x = lval_err("Modulo By Zero!"); break;
          }
          x->num %= y->num;
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
        if (strcmp(op, "%") == 0) {
          if (y->floating == 0) {
            lval_del(x); lval_del(y);
            x = lval_err("Modulo By Zero!"); break;
          }
          x->floating = fmod(x->floating, y->floating);
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
      lval_del(a);
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

lval* builtin_cond(lenv* e, lval* a) {
  LASSERT(a, a->count % 2 == 0,
          "'cond' requires an even number of arguments (condition/result pairs), got %i",
          a->count);
  for (int i = 0; i < a->count; i += 2) {
    LASSERT(a, a->cell[i]->type == LVAL_QEXPR,
            "'cond' condition %i must be a Q-expression { }, got %s",
            i/2, ltype_name(a->cell[i]->type));
    LASSERT(a, a->cell[i+1]->type == LVAL_QEXPR,
            "'cond' result %i must be a Q-expression { }, got %s",
            i/2, ltype_name(a->cell[i+1]->type));

    /* Evaluate the condition by flipping a copy to SEXPR. */
    lval* cond_expr = lval_copy(a->cell[i]);
    cond_expr->type = LVAL_SEXPR;
    lval* cond_result = lval_eval(e, cond_expr);

    if (cond_result->type == LVAL_ERR) { lval_del(a); return cond_result; }

    int truthy = (cond_result->type == LVAL_NUM && cond_result->num != 0);
    lval_del(cond_result);

    if (truthy) {
      lval* result = lval_copy(a->cell[i+1]);
      result->type = LVAL_SEXPR;
      lval_del(a);
      return lval_eval(e, result);
    }
  }
  /* No branch matched — return nil. */
  lval_del(a);
  return lval_qexpr();
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


lval* builtin_dotimes(lenv* e, lval* a) {
  LASSERT_NUM("dotimes", a, 3);
  LASSERT_TYPE("dotimes", a, 0, LVAL_QEXPR);
  LASSERT_TYPE("dotimes", a, 1, LVAL_NUM);
  LASSERT_TYPE("dotimes", a, 2, LVAL_QEXPR);

  LASSERT(a, a->cell[0]->count == 1,
    "'dotimes' first argument must be a single-symbol Q-expression, got %i elements.",
    a->cell[0]->count);
  LASSERT(a, a->cell[0]->cell[0]->type == LVAL_SYM,
    "'dotimes' loop variable must be a symbol, got %s.",
    ltype_name(a->cell[0]->cell[0]->type));

  lval* sym  = a->cell[0]->cell[0];
  long  n    = a->cell[1]->num;
  lval* body = a->cell[2];

  lval* result = lval_sexpr();

  for (long i = 0; i < n; i++) {
    lval* ival = lval_num(i);
    lenv_put(e, sym, ival);
    lval_del(ival);

    lval* body_copy  = lval_copy(body);
    body_copy->type  = LVAL_SEXPR;
    lval* res        = lval_eval(e, body_copy);

    if (res->type == LVAL_ERR) {
      lval_del(a);
      lval_del(result);
      return res;
    }
    lval_del(result);
    result = res;
  }

  lval_del(a);
  return result;
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

lval* builtin_mod(lenv* e, lval* a) {
  return builtin_op(e, a, "%");
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
    case LVAL_ATOM:
      return (*(RutAtomPtr*)x->obj).get() == (*(RutAtomPtr*)y->obj).get();
    case LVAL_FUTURE:
    case LVAL_PROMISE:
      return (*(RutFuturePtr*)x->obj).get() == (*(RutFuturePtr*)y->obj).get();
  }
  return 0;
}



lval* builtin_lambda(lenv* e, lval* a) {
  /* Check Two arguments, each of which are Q-Expressions */
  LASSERT_NUM("\\", a, 2);
  LASSERT_TYPE("\\", a, 0, LVAL_QEXPR);
  LASSERT_TYPE("\\", a, 1, LVAL_QEXPR);
  
  /* Check first Q-Expression contains only Symbols or {type name} typed formals */
  for (int i = 0; i < a->cell[0]->count; i++) {
    lval* f = a->cell[0]->cell[i];
    bool is_sym = (f->type == LVAL_SYM);
    bool is_typed = (f->type == LVAL_QEXPR && f->count == 2 &&
                     (f->cell[0]->type == LVAL_SYM || f->cell[0]->type == LVAL_STR) &&
                     f->cell[1]->type == LVAL_SYM);
    LASSERT(a, (is_sym || is_typed),
      "Cannot define non-symbol. Got %s, Expected %s.",
      ltype_name(f->type), ltype_name(LVAL_SYM));
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

std::vector<std::string> load_path;
static std::map<std::string, std::string> g_annotations;
std::string executable_dir() {
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
  } else if (v->type == LVAL_TOBJ) {
    snprintf(buf, sizeof(buf), "%p", v->obj);
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
  Long_t ret = rut_process_line(code, &err);
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

// ---------------------------------------------------------------------------
// jit-fn transpiler
// ---------------------------------------------------------------------------

// Returns true if v contains no side-effects (assignments, loops, method
// calls, new) — used to decide whether an `if` can become a ternary.

// ---------------------------------------------------------------------------
// Future builtins — (future {body}), (realized? f)
// ---------------------------------------------------------------------------
lval* builtin_parallelism(lenv* e, lval* a) {
  LASSERT_NUM("parallelism", a, 0);
  lval_del(a);
  return lval_num(rut_pool_size());
}

lval* builtin_set_parallelism(lenv* e, lval* a) {
  LASSERT_NUM("set-parallelism!", a, 1);
  LASSERT_TYPE("set-parallelism!", a, 0, LVAL_NUM);
  int n = (int)a->cell[0]->num;
  lval_del(a);
  rut_pool_set_size(n);
  return lval_num(rut_pool_size());
}

lval* builtin_future(lenv* e, lval* a) {
  LASSERT_NUM("future", a, 1);
  LASSERT_TYPE("future", a, 0, LVAL_QEXPR);

  lenv* env_copy  = lenv_snapshot(e);
  lval* body_copy = lval_copy(a->cell[0]);
  body_copy->type = LVAL_SEXPR;   // evaluate as S-expression on the thread
  lval_del(a);

  auto rf = std::make_shared<RutFuture>();

  rut_pool_submit([rf, env_copy, body_copy]() mutable {
    lval* result = lval_eval(env_copy, body_copy);
    lenv_del(env_copy);
    {
      std::lock_guard<std::mutex> lock(rf->mu);
      rf->result   = result;
      rf->realized = true;
    }
    rf->cv.notify_all();
  });

  return lval_future_new(std::move(rf));
}

lval* builtin_realized(lenv* e, lval* a) {
  LASSERT_NUM("realized?", a, 1);
  LASSERT(a, a->cell[0]->type == LVAL_FUTURE ||
             a->cell[0]->type == LVAL_PROMISE,
          "'realized?' expects a Future or Promise, got %s",
          ltype_name(a->cell[0]->type));
  RutFuturePtr& fp = *(RutFuturePtr*)a->cell[0]->obj;
  std::lock_guard<std::mutex> lock(fp->mu);
  int r = fp->realized ? 1 : 0;
  lval_del(a);
  return lval_num(r);
}

lval* builtin_promise(lenv* e, lval* a) {
  LASSERT_NUM("promise", a, 0);
  lval_del(a);
  return lval_promise_new();
}

lval* builtin_deliver(lenv* e, lval* a) {
  LASSERT_NUM("deliver", a, 2);
  LASSERT(a, a->cell[0]->type == LVAL_PROMISE,
          "'deliver' expects a Promise as first argument, got %s",
          ltype_name(a->cell[0]->type));
  RutFuturePtr& fp = *(RutFuturePtr*)a->cell[0]->obj;
  lval* val = lval_copy(a->cell[1]);
  bool delivered = false;
  {
    std::lock_guard<std::mutex> lock(fp->mu);
    if (!fp->realized) {
      fp->result   = val;
      fp->realized = true;
      delivered    = true;
    }
  }
  fp->cv.notify_all();
  lval_del(a);
  return lval_num(delivered ? 1 : 0);
}

// ---------------------------------------------------------------------------
// Atom builtins — (atom val), (deref a), (reset! a val), (swap! a f & args)
// ---------------------------------------------------------------------------
lval* builtin_atom(lenv* e, lval* a) {
  LASSERT_NUM("atom", a, 1);
  lval* init = lval_copy(a->cell[0]);
  lval_del(a);
  return lval_atom(init);
}

lval* builtin_deref(lenv* e, lval* a) {
  /* Accepted arities:
   *   (deref ref)                  — block forever (atoms and futures)
   *   (deref future ms)            — block up to ms milliseconds; return {} on timeout
   *   (deref future ms default)    — block up to ms milliseconds; return default on timeout */
  LASSERT(a, a->count >= 1 && a->count <= 3,
          "'deref' expects 1–3 arguments, got %i", a->count);
  LASSERT(a, a->cell[0]->type == LVAL_ATOM   ||
             a->cell[0]->type == LVAL_FUTURE  ||
             a->cell[0]->type == LVAL_PROMISE,
          "'deref' expects an Atom, Future, or Promise, got %s",
          ltype_name(a->cell[0]->type));
  LASSERT(a, a->count == 1 || a->cell[0]->type == LVAL_FUTURE ||
                               a->cell[0]->type == LVAL_PROMISE,
          "'deref' timeout is only meaningful for Futures and Promises, not Atoms");
  LASSERT(a, a->count < 2 || a->cell[1]->type == LVAL_NUM,
          "'deref' timeout-ms must be a number, got %s",
          a->count >= 2 ? ltype_name(a->cell[1]->type) : "");

  if (a->cell[0]->type == LVAL_ATOM) {
    RutAtomPtr& ap = *(RutAtomPtr*)a->cell[0]->obj;
    std::lock_guard<std::mutex> lock(ap->mu);
    lval* result = lval_copy(ap->val);
    lval_del(a);
    return result;
  }

  /* LVAL_FUTURE — optional timeout. */
  bool     has_timeout = (a->count >= 2);
  long     timeout_ms  = has_timeout ? a->cell[1]->num : 0;
  lval*    timeout_val = (a->count >= 3) ? lval_copy(a->cell[2]) : lval_qexpr(); /* {} = nil */

  using clock    = std::chrono::steady_clock;
  auto  deadline = clock::now() + std::chrono::milliseconds(timeout_ms);

  RutFuturePtr& fp = *(RutFuturePtr*)a->cell[0]->obj;
  while (true) {
    rut_drain_cling_queue();
    {
      std::unique_lock<std::mutex> lock(fp->mu);
      if (fp->realized) {
        lval* result = lval_copy(fp->result);
        lock.unlock();
        lval_del(timeout_val);
        lval_del(a);
        return result;
      }
    }
    if (has_timeout && clock::now() >= deadline) {
      lval_del(a);
      return timeout_val;   /* return the caller-supplied default (or {}) */
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

lval* builtin_reset(lenv* e, lval* a) {
  LASSERT_NUM("reset!", a, 2);
  LASSERT_TYPE("reset!", a, 0, LVAL_ATOM);
  lval* newval = lval_copy(a->cell[1]);
  RutAtomPtr& ap = *(RutAtomPtr*)a->cell[0]->obj;
  {
    std::lock_guard<std::mutex> lock(ap->mu);
    lval_del(ap->val);
    ap->val = lval_copy(newval);
  }
  lval_del(a);
  return newval;
}

lval* builtin_swap(lenv* e, lval* a) {
  LASSERT(a, a->count >= 2, "'swap!' requires at least 2 arguments.");
  LASSERT_TYPE("swap!", a, 0, LVAL_ATOM);
  RutAtomPtr& ap = *(RutAtomPtr*)a->cell[0]->obj;

  /* Read current value under lock, then release before calling f. */
  lval* cur;
  {
    std::lock_guard<std::mutex> lock(ap->mu);
    cur = lval_copy(ap->val);
  }

  /* Build (f cur extra-args...) and evaluate. */
  lval* call = lval_sexpr();
  lval_add(call, lval_copy(a->cell[1]));  /* f */
  lval_add(call, cur);
  for (int i = 2; i < a->count; i++)
    lval_add(call, lval_copy(a->cell[i]));

  lval* newval = lval_eval(e, call);
  if (newval->type == LVAL_ERR) { lval_del(a); return newval; }

  lval* result = lval_copy(newval);
  {
    std::lock_guard<std::mutex> lock(ap->mu);
    lval_del(ap->val);
    ap->val = newval;
  }
  lval_del(a);
  return result;
}

void lenv_add_builtins_lang(lenv* e) {
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
  lenv_add_builtin(e, "%", builtin_mod);
  /* Conditionals */
  lenv_add_builtin(e, "cond", builtin_cond);
  lenv_add_builtin(e, "if",   builtin_if);
  lenv_add_builtin(e, "==", builtin_eq);
  lenv_add_builtin(e, "!=", builtin_ne);
  lenv_add_builtin(e, ">",  builtin_gt);
  lenv_add_builtin(e, "<",  builtin_lt);
  lenv_add_builtin(e, ">=", builtin_ge);
  lenv_add_builtin(e, "<=", builtin_le);
  /* Helpers */
  lenv_add_builtin(e, "load",        builtin_load);
  lenv_add_builtin(e, "error",       builtin_error);
  lenv_add_builtin(e, "print",       builtin_print);
  lenv_add_builtin(e, "exit",        builtin_exit);
  lenv_add_builtin(e, "->",          builtin_arrow);
  lenv_add_builtin(e, "doto",        builtin_doto);
  lenv_add_builtin(e, "str",         builtin_str);
  lenv_add_builtin(e, "concat",      builtin_concat);
  lenv_add_builtin(e, "process-line",builtin_process_line);
  lenv_add_builtin(e, "annotate",    builtin_annotate);
  lenv_add_builtin(e, "annotations", builtin_annotations);
  lenv_add_builtin(e, "dotimes",     builtin_dotimes);
  /* Atom */
  lenv_add_builtin(e, "atom",   builtin_atom);
  lenv_add_builtin(e, "deref",  builtin_deref);
  lenv_add_builtin(e, "reset!", builtin_reset);
  lenv_add_builtin(e, "swap!",  builtin_swap);
  /* Future + thread pool */
  lenv_add_builtin(e, "future",           builtin_future);
  lenv_add_builtin(e, "realized?",        builtin_realized);
  lenv_add_builtin(e, "promise",          builtin_promise);
  lenv_add_builtin(e, "deliver",          builtin_deliver);
  lenv_add_builtin(e, "parallelism",      builtin_parallelism);
  lenv_add_builtin(e, "set-parallelism!", builtin_set_parallelism);
}
