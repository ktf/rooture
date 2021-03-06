#include <editline/readline.h>
#include <cstdlib>
#include <cstdio>
#include "ROOTureApp.h"
#include "Rtypes.h"
#include "TClass.h"
#include "TApplication.h"
#include "TMethodCall.h"
#include "TSysEvtHandler.h"
#include "TROOT.h"
#include "TSystem.h"
#include "TVirtualX.h"
#include "TVirtualPad.h"
#include "Getline.h"
#include "TStopwatch.h"
#include "TException.h"
#include "TInterpreter.h"
#include "TMethod.h"
#include "TFile.h"
#include "TRandom.h"
#include <iostream>

extern "C"
{
#include "mpc.h"
}

/* Create Enumeration of Possible lval Types */
enum { LVAL_ERR, LVAL_NUM,  LVAL_FLOAT, LVAL_SYM, LVAL_STR,
       LVAL_FUN, LVAL_TOBJ, LVAL_TMETHOD, LVAL_SEXPR, LVAL_QEXPR };

struct lval;
struct lenv;
void lval_del(lval* v);
int lval_eq(lval* x, lval* y);
lval* lval_copy(lval* v);
lval* lval_err(const char* fmt, ...);
lval* lval_eval(lenv* e, lval* v);
lval* lval_eval_sexpr(lenv* e, lval* v);
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
  /* TObject related */
  TObject *obj;
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

/* Create a new TObject lval */
lval* lval_tobj(TObject *obj) {
  lval* v = (lval*)malloc(sizeof(lval));
  v->type = LVAL_TOBJ;
  v->obj = obj;
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
  putchar(open);
  for (int i = 0; i < v->count; i++) {

    /* Print Value contained within */
    lval_print(v->cell[i]);

    /* Don't print trailing space if last element */
    if (i != (v->count-1)) {
      putchar(' ');
    }
  }
  putchar(close);
}

void lval_print_str(lval* v) {
  /* Make a Copy of the string */
  char* escaped = strdup(v->str);
  /* Pass it through the escape function */
  escaped = (char *)mpcf_escape(escaped);
  /* Print it between " characters */
  printf("\"%s\"", escaped);
  /* free the copied string */
  free(escaped);
}

TObjArray *lval_to_obj_array(lval *a, int offset) {
  TObjArray *args = new TObjArray();
  for (int i = offset; i < a->count; i++) {
    lval *v = a->cell[i];
    switch (v->type) {
      case LVAL_NUM: args->Add(new TObjString(strdup(std::to_string(v->num).c_str()))); break;
      case LVAL_FLOAT: args->Add(new TObjString(strdup(std::to_string(v->floating).c_str()))); break;
      case LVAL_STR: args->Add(new TObjString(strdup(("\"" + std::string(v->str) + "\"").c_str()))); break;
      default:
        printf("Cannot use as a C++ argument.");
        args->Add(new TObjString(""));
    }
  }
  return args;

}
std::string lval_to_cpp_arg(lval* a, int offset) {
  // Let's iterate on all the arguments and construct the
  // string which is required to 
  std::string args = "";
  bool first = true;
  for (int i = offset; i < a->count; i++) {
    if (!first)
      args += ", ";
    first = false;
    lval *v = a->cell[i];
    switch (v->type) {
      case LVAL_NUM: args += std::to_string(v->num); break;
      case LVAL_FLOAT: args += std::to_string(v->floating); break;
      case LVAL_STR: args += "\"" + std::string(v->str) + "\""; break;
      default:
        printf("Cannot use as a C++ argument.");
        args += "";
    }
  }
  return args;
}

/* Print an "lval" */
void lval_print(lval* v) {
  switch (v->type) {
    case LVAL_NUM:   printf("%li", v->num); break;
    case LVAL_FLOAT:   printf("%f", v->floating); break;
    case LVAL_ERR:   printf("Error: %s", v->err); break;
    case LVAL_FUN:
      if (v->builtin) {
        printf("<builtin>");
      } else {
        printf("(\\ "); lval_print(v->formals);
        putchar(' '); lval_print(v->body); putchar(')');
      }
    break;
    case LVAL_TOBJ:
      printf("<tobject @%llx>\n", (int64_t)v->obj);
      if (v->obj)
        v->obj->Print();
    break;
    case LVAL_TMETHOD:
      printf("<tmethodcall %s(%s)>", v->method->GetMethodName(), v->methodArgs);
    break;
    case LVAL_SYM:   printf("%s", v->sym); break;
    case LVAL_STR:   lval_print_str(v); break;
    case LVAL_SEXPR: lval_expr_print(v, '(', ')'); break;
    case LVAL_QEXPR: lval_expr_print(v, '{', '}'); break;
  }
}

void lval_println(lval* v) { lval_print(v); putchar('\n'); }

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

lval* lval_read(mpc_ast_t* t) {
  /* If Symbol, Number or method return conversion to that type */
  if (strstr(t->tag, "number")) { return lval_read_num(t); }
  if (strstr(t->tag, "floating")) { return lval_read_floating(t); }
  if (strstr(t->tag, "symbol")) { return lval_sym(t->contents);}
  /* If string read it */
  if (strstr(t->tag, "string")) { return lval_read_str(t); }


  /* If root (>) or sexpr then create empty list */
  lval* x = NULL;
  if (strcmp(t->tag, ">") == 0) { x = lval_sexpr(); } 
  if (strstr(t->tag, "sexpr"))  { x = lval_sexpr(); }
  if (strstr(t->tag, "qexpr"))  { x = lval_qexpr(); }

  /* Fill this list with any valid expression contained within */
  for (int i = 0; i < t->children_num; i++) {
    if (strcmp(t->children[i]->contents, "(") == 0) { continue; }
    if (strcmp(t->children[i]->contents, ")") == 0) { continue; }
    if (strcmp(t->children[i]->contents, "}") == 0) { continue; }
    if (strcmp(t->children[i]->contents, "{") == 0) { continue; }
    if (strcmp(t->children[i]->tag,  "regex") == 0) { continue; }
    if (strstr(t->children[i]->tag, "comment")) { continue; }
    x = lval_add(x, lval_read(t->children[i]));
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
    case LVAL_TOBJ: x->obj = v->obj; break;
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

  for (int i = 0; i < v->count; i++) {
    v->cell[i] = lval_eval(e, v->cell[i]);
  }
  
  for (int i = 0; i < v->count; i++) {
    if (v->cell[i]->type == LVAL_ERR) { return lval_take(v, i); }
  }

  if (v->count == 0) { return v; }  
  if (v->count == 1) { return lval_take(v, 0); }

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
  // FIXME: check that arguments are > 2.
  LASSERT(a, a->count >= 2,
    "Function '.' needs at least 2 argument: <method name> and <object>.");
  LASSERT_TYPE(".", a, 0, LVAL_STR);
  LASSERT_TYPE(".", a, 1, LVAL_TOBJ);
  /* Pop the first element */
  lval* name = lval_pop(a, 0);
  lval* obj = lval_pop(a, 0);
  lval_print(name);
  lval_print(obj);
  std::string args = lval_to_cpp_arg(a, 0);
  std::cout << "Executing " << name->str << "(" << args.c_str() 
                                     << ") in object " << std::hex << obj->obj
                                     << " of class " << obj->obj->ClassName() << std::endl;
  // FIXME: Slow and error prone, but good enough for now
  int error = 0;
  obj->obj->Execute(name->str, args.c_str(), &error);
  return lval_qexpr();
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

void lenv_add_global_object(lenv* e, const char* name, TObject *obj) {
  lval* k = lval_sym(name);
  lval* v = lval_tobj(obj);
  lenv_put(e, k, v);
  lval_del(k); lval_del(v);
}

lval* builtin_load(lenv* e, lval* a) {
  LASSERT_NUM("load", a, 1);
  LASSERT_TYPE("load", a, 0, LVAL_STR);
  
  /* Parse File given by string name */
  mpc_result_t r;
  if (mpc_parse_contents(a->cell[0]->str, Lispy, &r)) {
    
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
    lval_print(a->cell[i]); putchar(' ');
  }

  /* Print a newline and delete arguments */
  putchar('\n');
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

// Creates a new TObject
lval *builtin_new(lenv *e, lval* a) {
  LASSERT(a, a->count >= 1,
    "Function 'new' needs at least 1 argument: <class name>.");
  LASSERT_TYPE("new", a, 0, LVAL_STR);
  // Create an object of the given class
  const char *className = a->cell[0]->str;
  std::string args = lval_to_cpp_arg(a, 1);
  int error = 0;

  std::string ctorLine = std::string("new ") + className + "(" + args + ");";
  TObject *obj = (TObject *)gInterpreter->Calc(ctorLine.c_str());

  obj->IsA()->Print();
  lval_del(a);
  if (error)
    return lval_err("Constructor not found for %s",  className);
  return lval_tobj(obj);
}

// Invokes a method
lval *builtin_invoke(lenv *e, lval *a) {
  LASSERT_NUM("invoke", a, 2);
  LASSERT_TYPE("invoke", a, 0, LVAL_TMETHOD);
  LASSERT_TYPE("invoke", a, 1, LVAL_TOBJ);
  TMethodCall *m = a->cell[0]->method;
  const char *args = a->cell[0]->methodArgs;
  printf("Return type is %i\n", m->ReturnType());
  printf("Executing method with arguments %s(%s)\n", m->GetMethodName(), args);
  m->Execute(a->cell[1]->obj, args);
  return lval_qexpr();
}

void lenv_add_builtins(lenv* e) {  
  /* List Functions */
  lenv_add_builtin(e, "list", builtin_list);
  lenv_add_builtin(e, "head", builtin_head);
  lenv_add_builtin(e, "tail", builtin_tail);
  lenv_add_builtin(e, "eval", builtin_eval);
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
  lenv_add_builtin(e, "new", builtin_new);
  lenv_add_builtin(e, "member", builtin_member);
  lenv_add_builtin(e, ".", builtin_member);
  lenv_add_builtin(e, "invoke", builtin_invoke);
  
  /*A few TObjects */
  lenv_add_global_object(e, "gSystem", gSystem);
  lenv_add_global_object(e, "gInterpreter", gInterpreter);
  lenv_add_global_object(e, "gROOT", gROOT);
  lenv_add_global_object(e, "gFile", gFile);
  lenv_add_global_object(e, "gPad", gPad);
  lenv_add_global_object(e, "gDirectory", gDirectory);
  lenv_add_global_object(e, "gRandom", gRandom);
}

//----- Interrupt signal handler -----------------------------------------------
////////////////////////////////////////////////////////////////////////////////
//static Int_t Key_Pressed(Int_t key)
//{
//  gApplication->KeyPressed(key);
//  return 0;
//}


class TInterruptHandler : public TSignalHandler {
public:
   TInterruptHandler() : TSignalHandler(kSigInterrupt, kFALSE) { }
   Bool_t  Notify();
};

////////////////////////////////////////////////////////////////////////////////
/// TRint interrupt handler.

Bool_t TInterruptHandler::Notify()
{
   exit(0);
   if (fDelay) {
      fDelay++;
      return kTRUE;
   }
   return kTRUE;
}


class TTermInputHandler : public TFileHandler {
public:
   TTermInputHandler(Int_t fd) : TFileHandler(fd, 1) { }
   Bool_t Notify();
   Bool_t ReadNotify() { return Notify(); }
};

Bool_t TTermInputHandler::Notify()
{
   return gApplication->HandleTermInput();
}

ROOTureApp::ROOTureApp(int *argc, char **argv, lenv *e) 
: TApplication("ROOTure", argc, argv),
  fGlobalContext(e)
{
  // Install interrupt and terminal input handlers
  TInterruptHandler *ih = new TInterruptHandler();
  ih->Add();
  SetSignalHandler(ih);

  // Handle stdin events
  fInputHandler = new TTermInputHandler(0);
  fInputHandler->Add();

  // Add support for history
  // Goto into raw terminal input mode
  char defhist[kMAXPATHLEN];
  snprintf(defhist, sizeof(defhist), "%s/.rooture_hist", gSystem->HomeDirectory());
  // In the code we had HistorySize and HistorySave, in the rootrc and doc
  // we have HistSize and HistSave. Keep the doc as it is and check
  // now also for HistSize and HistSave in case the user did not use
  // the History versions
  int hist_size = 500;
  int hist_save = 400;
  Gl_histsize(hist_size, hist_save);
  Gl_windowchanged();

}

ROOTureApp::~ROOTureApp() {
  fInputHandler->Remove();
  delete fInputHandler;
}

void 
ROOTureApp::Run(Bool_t retrn) {
  /* Supplied with list of files, let's execute those. */
  if (this->Argc() >= 2) {

    /* loop over each supplied filename (starting from 1) */
    for (int i = 1; i < this->Argc(); i++) {

      /* Argument list with a single argument, the filename */
      lval* args = lval_add(lval_sexpr(), lval_str(this->Argv()[i]));

      /* Pass to builtin load and get the result */
      lval* x = builtin_load(fGlobalContext, args);

      /* If the result is an error be sure to print it */
      if (x->type == LVAL_ERR) { lval_println(x); }
      lval_del(x);
    }
  }
  TIter next(gROOT->GetListOfCanvases());
  TVirtualPad* canvas;
  while ((canvas = (TVirtualPad*)next())) {
    canvas->Update();
  }

  fInputHandler->Activate();
  Getlinem(kInit, "ROOTure> ");
  TApplication::Run(retrn);
  Getlinem(kCleanUp, 0);
}

Bool_t
ROOTureApp::HandleTermInput()
{
  static TStopwatch timer;

  /* Output our prompt */
  const char* line = Getlinem(kOneChar, 0);
  if (!line)
  {
    return kTRUE;
  }
  if (line[0] == 0 && Gl_eof())
    Terminate(0);
  gVirtualX->SetKeyAutoRepeat(kTRUE);

  const char *input = strdup(line);
  Gl_histadd(input);
  TString sline = line;
  
  // strip off '\n' and leading and trailing blanks
  sline = sline.Chop();
  sline = sline.Strip(TString::kBoth);
  ReturnPressed((char*)sline.Data());

  // prevent recursive calling of this input handler
  fInputHandler->DeActivate();
  if (gROOT->Timer()) timer.Start();
  TTHREAD_TLS(Bool_t) added;
  added = kFALSE; // reset on each call.

  /* Attempt to parse the user input */
  mpc_result_t r;
  if (mpc_parse("<stdin>", input, Lispy, &r)) {
    /* On success print and delete the AST */
    mpc_ast_print((mpc_ast_t*)r.output);
    lval* x = lval_eval(fGlobalContext, lval_read((mpc_ast_t *)r.output));
    lval_println(x);
    lval_del(x);
  } else {
    /* Otherwise print and delete the Error */
    mpc_err_print(r.error);
    mpc_err_delete(r.error);
  }
  free((void *)input);
  if (!sline.IsNull())
    LineProcessed(sline);
  TIter next(gROOT->GetListOfCanvases());
  TVirtualPad* canvas;
  while ((canvas = (TVirtualPad*)next())) {
    canvas->Update();
  }

  fInputHandler->Activate();

  TInterpreter::Instance()->EndOfLineAction();

  Getlinem(kInit, "ROOTure> ");
  return kTRUE;
}
  
void 
ROOTureApp::HandleException(Int_t sig)
{
   fCaughtException = kTRUE;
   if (TROOT::Initialized()) {
      if (gException) {
         Getlinem(kCleanUp, 0);
         Getlinem(kInit, "Root > ");
      }
   }
   TApplication::HandleException(sig);
}

ClassImp(ROOTureApp)

int main(int argc, char** argv) {
  /* Create Some Parsers */
  Floating  = mpc_new("floating");
  Number    = mpc_new("number");
  Symbol    = mpc_new("symbol");
  String    = mpc_new("string");
  Comment   = mpc_new("comment");
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
      symbol   : /[a-zA-Z0-9_+\\-*\\/\\\\=<>!&.]+/ ;           \
      string   : /\"(\\\\.|[^\"])*\"/ ;                       \
      comment  : /;[^\\r\\n]*/ ;                              \
      sexpr    : '(' <expr>* ')' ;                            \
      qexpr    : '{' <expr>* '}' ;                            \
      expr     : <floating> | <number> | <symbol> | <string>  \
               | <comment> | <sexpr> | <qexpr>;               \
      lispy    : /^/ <expr>* /$/ ;                            \
    ",
  Floating, Number, Symbol, String, Comment, Sexpr, Qexpr, Expr, Lispy);

  
  /* Print Version and Exit Information */
  puts("ROOTure 0.1.0");
  puts("Press Ctrl+c to Exit\n");

  /* The environment*/
  lenv* e = lenv_new();
  lenv_add_builtins(e);

  TApplication *app = new ROOTureApp(&argc, argv, e);
  app->Run();

  /* In a never ending loop */
  while (1) {

    /* Output our prompt */
    char* input = readline("ROOTure> ");

    /* Add input to history */
    add_history(input);

    /* Attempt to parse the user input */
    mpc_result_t r;
    if (mpc_parse("<stdin>", input, Lispy, &r)) {
      /* On success print and delete the AST */
      mpc_ast_print((mpc_ast_t*)r.output);
      lval* x = lval_eval(e, lval_read((mpc_ast_t *)r.output));
      lval_println(x);
      lval_del(x);
    } else {
      /* Otherwise print and delete the Error */
      mpc_err_print(r.error);
      mpc_err_delete(r.error);
    }
    free(input);
  }
  lenv_del(e);

  /* Undefine and delete our parsers */
  mpc_cleanup(8, 
    Number, Floating, Symbol, String, Comment, 
    Sexpr,  Qexpr,  Expr,   Lispy);

  return 0;
}
