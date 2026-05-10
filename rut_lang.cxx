#include "rooture.h"
#include <cmath>

/* Parsers */
mpc_parser_t* Number;
mpc_parser_t* Float32;
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

// Read-write lock protecting all lenv writes.
// MCP get_symbol acquires shared_lock; lenv_put acquires unique_lock.
std::shared_mutex g_env_rwlock;

// ---------------------------------------------------------------------------
// Atom — thread-safe mutable reference (Clojure-style)
// ---------------------------------------------------------------------------

lval* lval_atom(lval* init) {
  lval* v = (lval*)malloc(sizeof(lval));
  v->type = LVAL_ATOM;
  v->obj  = new RutAtomPtr(std::make_shared<RutAtom>(init));
  return v;
}


lval* lval_future_new(RutFuturePtr rf) {
  lval* v = (lval*)malloc(sizeof(lval));
  v->type = LVAL_FUTURE;
  v->obj  = new RutFuturePtr(std::move(rf));
  return v;
}

lval* lval_promise_new() {
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
  std::unique_lock<std::shared_mutex> lock(g_env_rwlock);

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
lenv* lenv_snapshot(lenv* e) {
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
    case LVAL_FLOAT:   return "Floating";
    case LVAL_FLOAT32: return "Float32";
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
    case LVAL_FLOAT32: break;

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

lval* lval_float32(float x) {
  lval* v = (lval *)malloc(sizeof(lval));
  v->type = LVAL_FLOAT32;
  v->floating = x;
  return v;
}

lval* lval_read_float32(mpc_ast_t* t) {
  errno = 0;
  float x = strtof(t->contents, NULL);
  return errno != ERANGE ?
    lval_float32(x) : lval_err("Invalid float32", t->contents);
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
    case LVAL_NUM:     rut_print("%li", v->num); break;
    case LVAL_FLOAT:   rut_print("%f", v->floating); break;
    case LVAL_FLOAT32: rut_print("%gf", (float)v->floating); break;
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

// ---------------------------------------------------------------------------
// lval_sprint — serialize lval to std::string (safe to call from MCP thread)
// Does NOT call ROOT Print() or rut_print; uses only C++ struct traversal.
// ---------------------------------------------------------------------------
static void lval_sprint_impl(lval* v, std::string& out);

static void lval_expr_sprint(lval* v, char open, char close, std::string& out) {
  out += open;
  for (int i = 0; i < v->count; i++) {
    lval_sprint_impl(v->cell[i], out);
    if (i != v->count-1) out += ' ';
  }
  out += close;
}

static void lval_sprint_impl(lval* v, std::string& out) {
  char buf[128];
  switch (v->type) {
    case LVAL_NUM:     snprintf(buf, sizeof(buf), "%li", v->num); out += buf; break;
    case LVAL_FLOAT:   snprintf(buf, sizeof(buf), "%f", v->floating); out += buf; break;
    case LVAL_FLOAT32: snprintf(buf, sizeof(buf), "%gf", (float)v->floating); out += buf; break;
    case LVAL_ERR:     out += "Error: "; out += v->err; break;
    case LVAL_SYM:     out += v->sym; break;
    case LVAL_STR: {
      char* escaped = strdup(v->str);
      escaped = (char*)mpcf_escape(escaped);
      out += '"'; out += escaped; out += '"';
      free(escaped);
      break;
    }
    case LVAL_FUN:
      if (v->builtin) {
        out += "<builtin>";
      } else {
        out += "(\\ ";
        lval_sprint_impl(v->formals, out);
        out += ' ';
        lval_sprint_impl(v->body, out);
        out += ')';
      }
      break;
    case LVAL_TOBJ:
      snprintf(buf, sizeof(buf), "<%s @%p>",
               v->cls ? v->cls->GetName() : "object", v->obj);
      out += buf;
      break;
    case LVAL_TMETHOD:
      out += "<tmethodcall "; out += v->method->GetMethodName();
      out += '('; out += v->methodArgs; out += ")>";
      break;
    case LVAL_SEXPR: lval_expr_sprint(v, '(', ')', out); break;
    case LVAL_QEXPR: lval_expr_sprint(v, '{', '}', out); break;
    case LVAL_JITFN:
      snprintf(buf, sizeof(buf), "<jit-fn %s/%ld>", v->sym, v->num);
      out += buf; break;
    case LVAL_COLJITFN:
      snprintf(buf, sizeof(buf), "<col-jit-fn %s/%ld\xe2\x86\x92%d>", v->sym, v->num, v->count);
      out += buf; break;
    case LVAL_COLUMN: {
      auto& cp = *(RutColumnPtr*)v->obj;
      snprintf(buf, sizeof(buf), "<column: %zu entries [%s]>", cp->n,
               cp->dtype == COL_FLOAT32 ? "f32" :
               cp->dtype == COL_FLOAT64 ? "f64" :
               cp->dtype == COL_INT32   ? "i32" :
               cp->dtype == COL_UINT8   ? "u8"  : "?");
      out += buf; break;
    }
    case LVAL_ATOM: {
      RutAtomPtr& ap = *(RutAtomPtr*)v->obj;
      std::lock_guard<std::mutex> alock(ap->mu);
      out += "<atom ";
      lval_sprint_impl(ap->val, out);
      out += '>';
      break;
    }
    case LVAL_FUTURE: {
      RutFuturePtr& fp = *(RutFuturePtr*)v->obj;
      std::lock_guard<std::mutex> flock(fp->mu);
      out += fp->realized ? "<future: realized>" : "<future: pending>";
      break;
    }
    case LVAL_PROMISE: {
      RutFuturePtr& fp = *(RutFuturePtr*)v->obj;
      std::lock_guard<std::mutex> flock(fp->mu);
      if (fp->realized) {
        out += "<promise: ";
        lval_sprint_impl(fp->result, out);
        out += '>';
      } else {
        out += "<promise: pending>";
      }
      break;
    }
  }
}

std::string lval_sprint(lval* v) {
  std::string out;
  lval_sprint_impl(v, out);
  return out;
}

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
  if (strstr(t->tag, "float32"))  { return lval_read_float32(t); }
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

    case LVAL_NUM:     x->num = v->num; break;
    case LVAL_FLOAT:   x->floating = v->floating; break;
    case LVAL_FLOAT32: x->floating = v->floating; break;
    
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

