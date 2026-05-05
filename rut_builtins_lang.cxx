#include "rooture.h"
#include <cstring>
#include <cmath>

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
  // Normalize float32 to float for runtime arithmetic (f-suffix only matters in transpiler)
  if (x->type == LVAL_FLOAT32) x->type = LVAL_FLOAT;
  if (y->type == LVAL_FLOAT32) y->type = LVAL_FLOAT;
  if (x->type == LVAL_NUM && y->type == LVAL_FLOAT)
    x = promote_to_floating(x);
  if (x->type == LVAL_FLOAT && y->type == LVAL_NUM)
    y = promote_to_floating(y);
}

lval* builtin_op(lenv *e, lval* a, const char* op) {
  /* Ensure all arguments are numbers */
  for (int i = 0; i < a->count; i++) {
    LASSERT(a, a->cell[i]->type == LVAL_NUM
               || a->cell[i]->type == LVAL_FLOAT
               || a->cell[i]->type == LVAL_FLOAT32,
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
               || a->cell[i]->type == LVAL_FLOAT
               || a->cell[i]->type == LVAL_FLOAT32,
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

lval* builtin_band(lenv* e, lval* a) {
  LASSERT_NUM("band", a, 2);
  for (int i = 0; i < a->count; i++)
    LASSERT(a, a->cell[i]->type == LVAL_NUM, "band requires integer arguments");
  lval* x = lval_pop(a, 0);
  lval* y = lval_pop(a, 0);
  x->num &= y->num;
  lval_del(y); lval_del(a); return x;
}

lval* builtin_bor(lenv* e, lval* a) {
  LASSERT_NUM("bor", a, 2);
  for (int i = 0; i < a->count; i++)
    LASSERT(a, a->cell[i]->type == LVAL_NUM, "bor requires integer arguments");
  lval* x = lval_pop(a, 0);
  lval* y = lval_pop(a, 0);
  x->num |= y->num;
  lval_del(y); lval_del(a); return x;
}

lval* builtin_bxor(lenv* e, lval* a) {
  LASSERT_NUM("bxor", a, 2);
  for (int i = 0; i < a->count; i++)
    LASSERT(a, a->cell[i]->type == LVAL_NUM, "bxor requires integer arguments");
  lval* x = lval_pop(a, 0);
  lval* y = lval_pop(a, 0);
  x->num ^= y->num;
  lval_del(y); lval_del(a); return x;
}

lval* builtin_bnot(lenv* e, lval* a) {
  LASSERT_NUM("bnot", a, 1);
  LASSERT(a, a->cell[0]->type == LVAL_NUM, "bnot requires integer argument");
  lval* x = lval_pop(a, 0);
  x->num = ~x->num;
  lval_del(a); return x;
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
    case LVAL_NUM:     return (x->num == y->num);
    case LVAL_FLOAT:   return (x->floating == y->floating);
    case LVAL_FLOAT32: return ((float)x->floating == (float)y->floating);

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


/* (undefined? {sym}) -> 1 if sym is unbound in the environment, 0 otherwise.
 * Relies on the rooture rule that unbound symbols evaluate to a string of
 * their own name.  This is the canonical way to test before setting a
 * default — used by the `default` stdlib helper. */
lval* builtin_is_undefined(lenv* e, lval* a) {
  LASSERT_NUM("undefined?", a, 1);
  LASSERT_TYPE("undefined?", a, 0, LVAL_QEXPR);
  LASSERT(a, a->cell[0]->count == 1,
    "undefined?: expected single-symbol Q-expression, got %i elements",
    a->cell[0]->count);
  lval* inner = a->cell[0]->cell[0];
  LASSERT(a, inner->type == LVAL_SYM,
    "undefined?: Q-expression must contain a symbol, got %s",
    ltype_name(inner->type));
  /* Walk the env chain looking for the symbol */
  const char* name = inner->sym;
  lenv* env = e;
  while (env) {
    for (int i = 0; i < env->count; i++) {
      if (strcmp(env->syms[i], name) == 0) {
        lval_del(a);
        return lval_num(0);   /* found — defined */
      }
    }
    env = env->par;
  }
  lval_del(a);
  return lval_num(1);  /* not found — undefined */
}

// ---------------------------------------------------------------------------
// Math builtins — unary and binary floating-point functions
// ---------------------------------------------------------------------------
static double lval_to_double(lval* v) {
  if (v->type == LVAL_FLOAT || v->type == LVAL_FLOAT32) return v->floating;
  return (double)v->num;
}

static lval* builtin_math1(lenv* e, lval* a, double(*fn)(double), const char* name) {
  LASSERT_NUM(name, a, 1);
  LASSERT(a, a->cell[0]->type == LVAL_NUM ||
             a->cell[0]->type == LVAL_FLOAT ||
             a->cell[0]->type == LVAL_FLOAT32,
          "'%s' requires a number, got %s", name, ltype_name(a->cell[0]->type));
  double result = fn(lval_to_double(a->cell[0]));
  lval_del(a);
  return lval_floating(result);
}

static lval* builtin_sqrt(lenv* e, lval* a)  { return builtin_math1(e, a, std::sqrt,  "sqrt"); }
static lval* builtin_log(lenv* e, lval* a)   { return builtin_math1(e, a, std::log,   "log"); }
static lval* builtin_log2(lenv* e, lval* a)  { return builtin_math1(e, a, std::log2,  "log2"); }
static lval* builtin_log10(lenv* e, lval* a) { return builtin_math1(e, a, std::log10, "log10"); }
static lval* builtin_exp(lenv* e, lval* a)   { return builtin_math1(e, a, std::exp,   "exp"); }
static lval* builtin_sin(lenv* e, lval* a)   { return builtin_math1(e, a, std::sin,   "sin"); }
static lval* builtin_cos(lenv* e, lval* a)   { return builtin_math1(e, a, std::cos,   "cos"); }
static lval* builtin_tan(lenv* e, lval* a)   { return builtin_math1(e, a, std::tan,   "tan"); }
static lval* builtin_asin(lenv* e, lval* a)  { return builtin_math1(e, a, std::asin,  "asin"); }
static lval* builtin_acos(lenv* e, lval* a)  { return builtin_math1(e, a, std::acos,  "acos"); }
static lval* builtin_atan(lenv* e, lval* a)  { return builtin_math1(e, a, std::atan,  "atan"); }
static lval* builtin_fabs(lenv* e, lval* a)  { return builtin_math1(e, a, std::fabs,  "fabs"); }
static lval* builtin_floor(lenv* e, lval* a) { return builtin_math1(e, a, std::floor, "floor"); }
static lval* builtin_ceil(lenv* e, lval* a)  { return builtin_math1(e, a, std::ceil,  "ceil"); }

static lval* builtin_pow(lenv* e, lval* a) {
  LASSERT_NUM("pow", a, 2);
  for (int i = 0; i < 2; i++)
    LASSERT(a, a->cell[i]->type == LVAL_NUM ||
               a->cell[i]->type == LVAL_FLOAT ||
               a->cell[i]->type == LVAL_FLOAT32,
            "'pow' requires numbers, got %s at arg %d", ltype_name(a->cell[i]->type), i);
  double result = std::pow(lval_to_double(a->cell[0]), lval_to_double(a->cell[1]));
  lval_del(a);
  return lval_floating(result);
}

static lval* builtin_atan2(lenv* e, lval* a) {
  LASSERT_NUM("atan2", a, 2);
  for (int i = 0; i < 2; i++)
    LASSERT(a, a->cell[i]->type == LVAL_NUM ||
               a->cell[i]->type == LVAL_FLOAT ||
               a->cell[i]->type == LVAL_FLOAT32,
            "'atan2' requires numbers, got %s at arg %d", ltype_name(a->cell[i]->type), i);
  double result = std::atan2(lval_to_double(a->cell[0]), lval_to_double(a->cell[1]));
  lval_del(a);
  return lval_floating(result);
}

void lenv_add_builtins_lang(lenv* e) {
  /* List Functions */
  lenv_add_builtin(e, "list", builtin_list);
  lenv_add_builtin(e, "head",       builtin_head);
  lenv_add_builtin(e, "undefined?", builtin_is_undefined);
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
  lenv_add_builtin(e, "%",    builtin_mod);
  lenv_add_builtin(e, "band", builtin_band);
  lenv_add_builtin(e, "bor",  builtin_bor);
  lenv_add_builtin(e, "bxor", builtin_bxor);
  lenv_add_builtin(e, "bnot", builtin_bnot);
  /* Floating-point math */
  lenv_add_builtin(e, "sqrt",  builtin_sqrt);
  lenv_add_builtin(e, "log",   builtin_log);
  lenv_add_builtin(e, "log2",  builtin_log2);
  lenv_add_builtin(e, "log10", builtin_log10);
  lenv_add_builtin(e, "exp",   builtin_exp);
  lenv_add_builtin(e, "sin",   builtin_sin);
  lenv_add_builtin(e, "cos",   builtin_cos);
  lenv_add_builtin(e, "tan",   builtin_tan);
  lenv_add_builtin(e, "asin",  builtin_asin);
  lenv_add_builtin(e, "acos",  builtin_acos);
  lenv_add_builtin(e, "atan",  builtin_atan);
  lenv_add_builtin(e, "atan2", builtin_atan2);
  lenv_add_builtin(e, "fabs",  builtin_fabs);
  lenv_add_builtin(e, "floor", builtin_floor);
  lenv_add_builtin(e, "ceil",  builtin_ceil);
  lenv_add_builtin(e, "pow",   builtin_pow);
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
