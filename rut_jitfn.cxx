#include "rooture.h"


// ---------------------------------------------------------------------------
// jit-fn transpiler
// ---------------------------------------------------------------------------

// Sanitise a rooture identifier to a valid C++ identifier:
// rooture allows '-' in symbol names; C++ uses '_'.
static std::string to_cpp_id(const char* s) {
  std::string r(s);
  for (char& c : r) if (c == '-') c = '_';
  return r;
}

// Transpilation context: formal parameter names (emitted as-is) and
// the closure environment for constant-folding free variables.
struct KernelOutInfo {
  std::string cpp_type;  // "int", "float", …
  std::string name;      // C++ identifier
  size_t      size_val;  // constant-folded output size
  std::string init;      // C++ init literal
};

struct JitCtx {
  std::set<std::string> params;      // scalar formal params — emitted as-is
  std::set<std::string> col_params;  // column formal params — emitted as name[i]
  lenv* env;                          // closure env — LVAL_NUM/FLOAT literals folded
  int n_outputs = 0;                  // 0 = scalar jit-fn; N = col-jit-fn loop outputs
  bool kernel_mode = false;           // true inside col-kernel bodies
  std::map<std::string, KernelOutInfo> kernel_outs;      // output name → metadata (for lookup)
  std::vector<std::string>             kernel_out_order;  // insertion order (for code-gen)
};

// Returns true if v contains no side-effects (assignments, loops, method
// calls, new) — used to decide whether an `if` can become a ternary.
static bool rut_is_pure(lval* v) {
  if (!v) return true;
  if (v->type == LVAL_NUM || v->type == LVAL_FLOAT || v->type == LVAL_FLOAT32 ||
      v->type == LVAL_STR || v->type == LVAL_SYM)
    return true;
  if (v->type == LVAL_QEXPR) {
    if (v->count == 1) return rut_is_pure(v->cell[0]);
    return false;
  }
  if (v->type != LVAL_SEXPR || v->count == 0) return false;
  lval* head = v->cell[0];
  if (head->type != LVAL_SYM) return false;
  const char* s = head->sym;
  // Side-effecting forms
  if (strcmp(s, "=") == 0 || strcmp(s, "def") == 0) return false;
  if (strcmp(s, "dotimes") == 0 || strcmp(s, "do") == 0) return false;
  if (strcmp(s, "new") == 0) return false;
  if (s[0] == '.') return false;  // method call
  // Arithmetic / comparison / not / :: — check children
  for (int i = 1; i < v->count; i++)
    if (!rut_is_pure(v->cell[i])) return false;
  return true;
}

// Forward declaration
static std::string rut_to_cpp_stmt(lval* v, const std::string& ind,
                                   bool tail, const JitCtx& ctx);

// Pre-scan an AST node and collect every name bound by (= {name} ...) into
// ctx.params, so the expression transpiler treats them as C++ identifiers
// rather than trying to constant-fold them from the closure environment.
static void collect_locals(lval* v, JitCtx& ctx) {
  if (!v) return;
  if (v->type == LVAL_QEXPR || v->type == LVAL_SEXPR) {
    // (= {name} rhs) — the lhs is a QEXPR with one SYM child
    if (v->type == LVAL_SEXPR && v->count == 3 &&
        v->cell[0]->type == LVAL_SYM &&
        strcmp(v->cell[0]->sym, "=") == 0) {
      lval* lhs = v->cell[1];
      if (lhs->type == LVAL_QEXPR && lhs->count == 1 &&
          lhs->cell[0]->type == LVAL_SYM)
        ctx.params.insert(to_cpp_id(lhs->cell[0]->sym));
    }
    for (int i = 0; i < v->count; i++) collect_locals(v->cell[i], ctx);
  }
}

// Transpile a single expression to a C++ expression string (no semicolon).
static std::string rut_to_cpp_expr(lval* v, const JitCtx& ctx) {
  if (!v) return "/*null*/";
  if (v->type == LVAL_NUM) return std::to_string(v->num);
  if (v->type == LVAL_FLOAT) {
    char buf[32]; snprintf(buf, sizeof(buf), "%.17g", v->floating);
    return buf;
  }
  if (v->type == LVAL_FLOAT32) {
    char buf[32]; snprintf(buf, sizeof(buf), "%.9g", (float)v->floating);
    // C++ requires a decimal point or exponent before the 'f' suffix.
    // "%.9g" may omit it for integer-valued floats (e.g. 1.0 → "1").
    std::string s = buf;
    if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
        s.find('E') == std::string::npos && s.find('n') == std::string::npos)
      s += ".0";
    return s + "f";
  }
  if (v->type == LVAL_STR)
    return "\"" + escape_for_cling_str(v->str) + "\"";
  // Kernel mode: (name expr) → name[expr] for col inputs and kernel outputs
  if (ctx.kernel_mode && (v->type == LVAL_SEXPR || v->type == LVAL_QEXPR)
      && v->count == 2 && v->cell[0]->type == LVAL_SYM) {
    std::string cid = to_cpp_id(v->cell[0]->sym);
    if (ctx.col_params.count(cid) || ctx.kernel_outs.count(cid))
      return cid + "[" + rut_to_cpp_expr(v->cell[1], ctx) + "]";
  }

  if (v->type == LVAL_SYM) {
    std::string cid = to_cpp_id(v->sym);
    // Kernel mode: bare column name = pointer (not auto-indexed)
    if (ctx.kernel_mode && ctx.col_params.count(cid)) return cid;
    // Column parameter — emit as name[i] inside the vectorised loop.
    if (ctx.col_params.count(cid)) return cid + "[i]";
    // If it's a scalar formal parameter, emit as a C++ identifier.
    if (ctx.params.count(cid)) return cid;
    // Try constant-folding from the closure environment.
    if (ctx.env) {
      lval tmp; tmp.type = LVAL_SYM; tmp.sym = v->sym;
      lval* found = lenv_get(ctx.env, &tmp);
      if (found) {
        std::string r;
        if (found->type == LVAL_NUM)
          r = std::to_string(found->num);
        else if (found->type == LVAL_FLOAT) {
          char buf[32]; snprintf(buf, sizeof(buf), "%.17g", found->floating);
          r = buf;
        } else if (found->type == LVAL_FLOAT32) {
          char buf[32]; snprintf(buf, sizeof(buf), "%.9g", (float)found->floating);
          r = buf;
          if (r.find('.') == std::string::npos && r.find('e') == std::string::npos &&
              r.find('E') == std::string::npos && r.find('n') == std::string::npos)
            r += ".0";
          r += "f";
        } else if (found->type == LVAL_TOBJ && found->obj) {
          // Fold a heap object to a stable pointer cast: ((ClassName*)0xADDR)
          // The address is baked in at compile time; the value is read at runtime.
          std::string cls_name = found->cls ? std::string(found->cls->GetName()) : "void";
          char addr_buf[32]; snprintf(addr_buf, sizeof(addr_buf), "%p", found->obj);
          r = "((" + cls_name + "*)" + std::string(addr_buf) + ")";
        }
        lval_del(found);
        if (!r.empty()) return r;
      }
    }
    return cid;  // fallback: emit as C++ identifier (hyphens → underscores)
  }

  // In rooture {f a b} = (f a b): Q-expressions and S-expressions have the
  // same semantics in call position. Unwrap single-element, otherwise fall
  // through to the SEXPR handler below.
  if (v->type == LVAL_QEXPR) {
    if (v->count == 1) return rut_to_cpp_expr(v->cell[0], ctx);
    // Multi-element QEXPR: treat as function call (same as SEXPR).
  }

  if ((v->type != LVAL_SEXPR && v->type != LVAL_QEXPR) || v->count == 0)
    return "/*unsupported*/";

  lval* head = v->cell[0];
  if (head->type != LVAL_SYM) return "/*unsupported-head*/";
  const char* s = head->sym;

  // Binary arithmetic / comparison
  const char* binop = nullptr;
  if (strcmp(s, "+")  == 0) binop = "+";
  if (strcmp(s, "-")  == 0) binop = "-";
  if (strcmp(s, "*")  == 0) binop = "*";
  if (strcmp(s, "/")  == 0) binop = "/";
  if (strcmp(s, "<")  == 0) binop = "<";
  if (strcmp(s, ">")  == 0) binop = ">";
  if (strcmp(s, "<=") == 0) binop = "<=";
  if (strcmp(s, ">=") == 0) binop = ">=";
  if (strcmp(s, "==")   == 0) binop = "==";
  if (strcmp(s, "!=")   == 0) binop = "!=";
  if (strcmp(s, "band") == 0) binop = "&";
  if (strcmp(s, "bor")  == 0) binop = "|";
  if (strcmp(s, "bxor") == 0) binop = "^";
  if (strcmp(s, "rem")  == 0) binop = "%";
  if (binop && v->count >= 3) {
    std::string result = rut_to_cpp_expr(v->cell[1], ctx);
    for (int i = 2; i < v->count; i++)
      result = "(" + result + " " + binop + " " + rut_to_cpp_expr(v->cell[i], ctx) + ")";
    return result;
  }

  // (not expr)
  if (strcmp(s, "not") == 0 && v->count == 2)
    return "(!(" + rut_to_cpp_expr(v->cell[1], ctx) + "))";

  // (to-int expr) → (int)(expr)
  if (strcmp(s, "to-int") == 0 && v->count == 2)
    return "(int)(" + rut_to_cpp_expr(v->cell[1], ctx) + ")";

  // (bnot expr)
  if (strcmp(s, "bnot") == 0 && v->count == 2)
    return "(~(" + rut_to_cpp_expr(v->cell[1], ctx) + "))";

  // (:: Method ClassName args...) — static call
  if (strcmp(s, "::") == 0 && v->count >= 3) {
    lval* meth = v->cell[1];
    lval* cls  = v->cell[2];
    std::string call = std::string(cls->type == LVAL_SYM ? cls->sym : cls->str)
                       + "::" + std::string(meth->sym) + "(";
    for (int i = 3; i < v->count; i++) {
      if (i > 3) call += ", ";
      call += rut_to_cpp_expr(v->cell[i], ctx);
    }
    call += ")";
    return call;
  }
  // (::Method ClassName args...) — sugared static call (method name starts with ::)
  if (s[0] == ':' && s[1] == ':' && v->count >= 2) {
    lval* cls = v->cell[1];
    std::string call = std::string(cls->type == LVAL_SYM ? cls->sym : cls->str)
                       + "::" + std::string(s + 2) + "(";
    for (int i = 2; i < v->count; i++) {
      if (i > 2) call += ", ";
      call += rut_to_cpp_expr(v->cell[i], ctx);
    }
    call += ")";
    return call;
  }

  // (.Method obj args...) — instance call, raw AST form
  if (s[0] == '.' && s[1] != '\0' && v->count >= 2) {
    std::string call = rut_to_cpp_expr(v->cell[1], ctx) + "->" + std::string(s + 1) + "(";
    for (int i = 2; i < v->count; i++) {
      if (i > 2) call += ", ";
      call += rut_to_cpp_expr(v->cell[i], ctx);
    }
    call += ")";
    return call;
  }

  // (new ClassName args...)
  if (strcmp(s, "new") == 0 && v->count >= 2) {
    lval* cls = v->cell[1];
    std::string cname = (cls->type == LVAL_SYM) ? cls->sym : cls->str;
    std::string call = "new " + cname + "(";
    for (int i = 2; i < v->count; i++) {
      if (i > 2) call += ", ";
      call += rut_to_cpp_expr(v->cell[i], ctx);
    }
    call += ")";
    return call;
  }

  // (if cond {true} {false}) — ternary when both branches are pure
  if (strcmp(s, "if") == 0 && v->count == 3) {
    // (if cond {}) — no false branch; not valid as expression
    return "/*unsupported-if-expr*/";
  }
  if (strcmp(s, "if") == 0 && v->count == 4) {
    lval* tb = v->cell[2];
    lval* fb = v->cell[3];
    bool tb_pure = rut_is_pure(tb);
    bool fb_pure = rut_is_pure(fb);
    if (tb_pure && fb_pure) {
      std::string te = (tb->type == LVAL_QEXPR && tb->count == 0) ? "0" : rut_to_cpp_expr(tb, ctx);
      std::string fe = (fb->type == LVAL_QEXPR && fb->count == 0) ? "0" : rut_to_cpp_expr(fb, ctx);
      return "(" + rut_to_cpp_expr(v->cell[1], ctx) + " ? " + te + " : " + fe + ")";
    }
  }

  // General direct C/C++ function call: (fn arg1 arg2 ...) → fn(arg1, arg2, ...)
  // This lets col-jit-fn bodies call any C function visible in the Cling session,
  // e.g. (cosf x), (atan2f y x), (sqrtf x), (fmaxf a b).
  {
    std::string call = to_cpp_id(s) + "(";
    for (int i = 1; i < v->count; i++) {
      if (i > 1) call += ", ";
      call += rut_to_cpp_expr(v->cell[i], ctx);
    }
    call += ")";
    return call;
  }
}

// Transpile a statement (or block) to C++ code ending with '\n'.
// When tail=true the last expression in tail position is emitted as `return expr;`.
static std::string rut_to_cpp_stmt(lval* v, const std::string& ind,
                                   bool tail, const JitCtx& ctx) {
  if (!v) return "";

  // Unwrap QEXPR used as a statement (raw AST body cells).
  // {do e1 e2 ...} in raw AST is a QEXPR [SYM do, e1, e2, ...] — treat as do-block.
  if (v->type == LVAL_QEXPR) {
    if (v->count == 0) return "";
    if (v->count == 1) return rut_to_cpp_stmt(v->cell[0], ind, tail, ctx);

    // Kernel mode: multi-statement body treated as sequential block.
    if (ctx.kernel_mode && v->count > 1) {
      bool is_do = v->cell[0]->type == LVAL_SYM && strcmp(v->cell[0]->sym, "do") == 0;
      int start = is_do ? 1 : 0;
      std::string out;
      for (int i = start; i < v->count; i++)
        out += rut_to_cpp_stmt(v->cell[i], ind, tail && (i == v->count - 1), ctx);
      return out;
    }

    // Multi-output tail for col-jit-fn: {e0 e1 ...} with exactly n_outputs children.
    if (tail && ctx.n_outputs > 1 && v->count == ctx.n_outputs) {
      // First child must not be the symbol "do" (to distinguish from a do-block).
      bool is_do = v->cell[0]->type == LVAL_SYM && strcmp(v->cell[0]->sym, "do") == 0;
      if (!is_do) {
        std::string out;
        for (int j = 0; j < v->count; j++)
          out += ind + "__out" + std::to_string(j) + "[i] = "
               + rut_to_cpp_expr(v->cell[j], ctx) + ";\n";
        return out;
      }
    }

    // {do e1 e2 ...} → sequential block
    if (v->cell[0]->type == LVAL_SYM && strcmp(v->cell[0]->sym, "do") == 0) {
      std::string out;
      for (int i = 1; i < v->count; i++)
        out += rut_to_cpp_stmt(v->cell[i], ind, tail && (i == v->count - 1), ctx);
      return out;
    }

    // {f a b ...} → function call (same as (f a b ...) in rooture semantics)
    // Delegate to the expression transpiler, which handles (::Fn), (.method),
    // arithmetic, (if ...), and now general C calls like (cosf x).
    if (tail) {
      // rut_to_cpp_stmt's emit_tail is not in scope here; reconstruct equivalent.
      std::string expr = rut_to_cpp_expr(v, ctx);
      if (ctx.n_outputs > 0)
        return ind + "__out0[i] = " + expr + ";\n";
      return ind + "return " + expr + ";\n";
    }
    return ind + rut_to_cpp_expr(v, ctx) + ";\n";
  }

  // Helper: emit the tail (return or output-assignment depending on context).
  auto emit_tail = [&](const std::string& expr_str) -> std::string {
    if (!tail) return ind + expr_str + ";\n";
    if (ctx.n_outputs > 0) return ind + "__out0[i] = " + expr_str + ";\n";
    return ind + "return " + expr_str + ";\n";
  };

  if (v->type != LVAL_SEXPR || v->count == 0)
    return emit_tail(rut_to_cpp_expr(v, ctx));

  lval* head = v->cell[0];
  if (head->type != LVAL_SYM)
    return emit_tail(rut_to_cpp_expr(v, ctx));
  const char* s = head->sym;

  // (do e1 e2 ...) — last child inherits tail
  if (strcmp(s, "do") == 0) {
    std::string out;
    for (int i = 1; i < v->count; i++)
      out += rut_to_cpp_stmt(v->cell[i], ind, tail && (i == v->count - 1), ctx);
    return out;
  }

  // (= {varname} rhs)
  if (strcmp(s, "=") == 0 && v->count == 3) {
    lval* sym_q = v->cell[1];
    std::string varname;
    if (sym_q->type == LVAL_QEXPR && sym_q->count == 1 && sym_q->cell[0]->type == LVAL_SYM)
      varname = to_cpp_id(sym_q->cell[0]->sym);
    else
      varname = "/*bad-var*/";
    const char* decl_type = (!ctx.kernel_mode && ctx.n_outputs > 0) ? "float " : "auto ";
    return ind + decl_type + varname + " = " + rut_to_cpp_expr(v->cell[2], ctx) + ";\n";
  }

  // (dotimes {i} N {body})
  if (strcmp(s, "dotimes") == 0 && v->count == 4) {
    lval* sym_q = v->cell[1];
    std::string ivar = (sym_q->type == LVAL_QEXPR && sym_q->count == 1 &&
                        sym_q->cell[0]->type == LVAL_SYM)
                       ? to_cpp_id(sym_q->cell[0]->sym) : "i";
    std::string n    = rut_to_cpp_expr(v->cell[2], ctx);
    std::string body = rut_to_cpp_stmt(v->cell[3], ind + "  ", false, ctx);
    return ind + "for(int " + ivar + " = 0; " + ivar + " < " + n + "; ++" + ivar + ") {\n"
           + body + ind + "}\n";
  }

  // (if cond {} {false-body}) — pure-empty true branch → negated if
  if (strcmp(s, "if") == 0 && v->count == 4) {
    lval* tb = v->cell[2];
    lval* fb = v->cell[3];
    bool tb_empty = (tb->type == LVAL_QEXPR && tb->count == 0);
    bool fb_empty = (fb->type == LVAL_QEXPR && fb->count == 0);

    if (tb_empty && fb_empty) return "";
    if (tb_empty) {
      return ind + "if(!(" + rut_to_cpp_expr(v->cell[1], ctx) + ")) {\n"
             + rut_to_cpp_stmt(fb, ind + "  ", false, ctx)
             + ind + "}\n";
    }
    if (fb_empty) {
      return ind + "if(" + rut_to_cpp_expr(v->cell[1], ctx) + ") {\n"
             + rut_to_cpp_stmt(tb, ind + "  ", false, ctx)
             + ind + "}\n";
    }
    // Both branches non-empty — check if pure (ternary as statement)
    if (rut_is_pure(tb) && rut_is_pure(fb))
      return emit_tail(rut_to_cpp_expr(v, ctx));
    // Full if/else
    return ind + "if(" + rut_to_cpp_expr(v->cell[1], ctx) + ") {\n"
           + rut_to_cpp_stmt(tb, ind + "  ", tail, ctx)
           + ind + "} else {\n"
           + rut_to_cpp_stmt(fb, ind + "  ", tail, ctx)
           + ind + "}\n";
  }

  // (scatter! name idx val) — bounds-checked write to kernel output
  if (ctx.kernel_mode && strcmp(s, "scatter!") == 0 && v->count == 4) {
    if (v->cell[1]->type != LVAL_SYM) return ind + "/*bad-scatter!*/\n";
    std::string col = to_cpp_id(v->cell[1]->sym);
    std::string idx = rut_to_cpp_expr(v->cell[2], ctx);
    std::string val = rut_to_cpp_expr(v->cell[3], ctx);
    return ind + "{ auto __idx = (" + idx + "); if (__idx >= 0 && "
               + "(size_t)__idx < " + col + "_n) "
               + col + "[__idx] = (" + val + "); }\n";
  }

  // (.Method obj args...) — instance method call as statement
  if (s[0] == '.' && s[1] != '\0' && v->count >= 2)
    return emit_tail(rut_to_cpp_expr(v, ctx));

  // Anything else: try as expression statement
  return emit_tail(rut_to_cpp_expr(v, ctx));
}

// Per-type counters so jit-fn / col-jit-fn / col-kernel don't step on each other.
static std::atomic<int> g_jitfn_counter{0};   // __rut_jit_N
static std::atomic<int> g_coljit_counter{0};  // __rut_cjit_N
static std::atomic<int> g_kernel_counter{0};  // __rut_kernel_N

// (jit-fn lambda) — transpile a rooture lambda to a native C++ void function,
// JIT-declare it via gInterpreter->Declare, and return an LVAL_JITFN.
lval* builtin_jit_fn(lenv* e, lval* a) {
  // Accept (jit-fn lambda) or (jit-fn rettype lambda).
  // Per-parameter types are expressed inline in the formals as {type name},
  // e.g. (\{{float snp} tgl} {body}) — untyped formals default to "auto".
  if (a->count < 1 || a->count > 2) {
    lval_del(a);
    return lval_err("jit-fn: expected 1 or 2 arguments, got %d", a->count);
  }
  std::string explicit_ret;
  int fn_idx = 0;
  if (a->count == 2) {
    if (a->cell[0]->type != LVAL_STR) {
      lval_del(a);
      return lval_err("jit-fn: first argument must be a return-type string (e.g. void, float, bool)");
    }
    explicit_ret = a->cell[0]->str;
    fn_idx = 1;
  }
  if (a->cell[fn_idx]->type != LVAL_FUN) {
    lval_del(a);
    return lval_err("jit-fn: argument %d must be a function", fn_idx);
  }
  lval* fn = a->cell[fn_idx];
  if (fn->builtin) {
    lval_del(a);
    return lval_err("jit-fn: cannot JIT a builtin");
  }

  // Build transpilation context: collect formal parameter names so that free
  // variables (not in formals) can be constant-folded from the calling env.
  // NOTE: fn->env is always a fresh empty env (see lval_lambda); the caller's
  // env `e` is where user-defined variables (like minCls) actually live.
  JitCtx ctx;
  ctx.env = e;
  for (int i = 0; i < fn->formals->count; i++) {
    lval* formal = fn->formals->cell[i];
    if (formal->type == LVAL_SYM)
      ctx.params.insert(to_cpp_id(formal->sym));
    else if (formal->type == LVAL_QEXPR && formal->count == 2 &&
             formal->cell[1]->type == LVAL_SYM)
      ctx.params.insert(to_cpp_id(formal->cell[1]->sym));
  }

  // Count non-& formals (formals may be SYM or {type name} QEXPR)
  int nparams = 0;
  for (int i = 0; i < fn->formals->count; i++) {
    lval* f = fn->formals->cell[i];
    if (f->type == LVAL_SYM && strcmp(f->sym, "&") == 0) continue;
    nparams++;
  }

  std::string name = "__rut_jit_" + std::to_string(g_jitfn_counter++);

  // fn->body is a QEXPR with one child (the body expression)
  lval* body_node = (fn->body->count == 1) ? fn->body->cell[0] : fn->body;

  // Determine if the last expression in tail position is returnable (pure).
  // Walk do-blocks and QEXPRs to find the final node.
  auto find_tail = [](lval* node) -> lval* {
    while (true) {
      if (!node) return nullptr;
      if (node->type == LVAL_QEXPR) {
        if (node->count == 0) return nullptr;
        node = node->cell[node->count - 1]; continue;
      }
      if (node->type == LVAL_SEXPR && node->count > 1 &&
          node->cell[0]->type == LVAL_SYM &&
          strcmp(node->cell[0]->sym, "do") == 0) {
        node = node->cell[node->count - 1]; continue;
      }
      return node;
    }
  };
  lval* tail_node = find_tail(body_node);
  bool returns_value = explicit_ret.empty() ? (tail_node && rut_is_pure(tail_node))
                                            : (explicit_ret != "void");
  std::string ret_type = explicit_ret.empty() ? (returns_value ? "auto" : "void")
                                              : explicit_ret;

  // Build signature. Each formal is either:
  //   LVAL_SYM  "name"         → auto name   (C++20 abbreviated function template)
  //   LVAL_QEXPR {type name}   → type name   (explicit, e.g. {float snp})
  // Explicit types make the function concrete so RDF CallableTraits can resolve
  // argument types directly (needed when branch types differ from double).
  std::string sig = ret_type + " " + name + "(";
  for (int i = 0, pi = 0; i < fn->formals->count; i++) {
    lval* formal = fn->formals->cell[i];
    if (formal->type == LVAL_SYM && strcmp(formal->sym, "&") == 0) continue;
    if (pi++) sig += ", ";
    if (formal->type == LVAL_QEXPR && formal->count == 2 &&
        (formal->cell[0]->type == LVAL_SYM || formal->cell[0]->type == LVAL_STR) &&
        formal->cell[1]->type == LVAL_SYM) {
      // {type name} — explicit param type; type may be SYM (unevaluated inside QEXPR)
      std::string pname = (formal->cell[0]->type == LVAL_SYM)
                          ? std::string(formal->cell[0]->sym)
                          : std::string(formal->cell[0]->str);
      sig += pname + " " + to_cpp_id(formal->cell[1]->sym);
    } else if (formal->type == LVAL_SYM) {
      sig += "auto " + to_cpp_id(formal->sym);
    } else {
      lval_del(a);
      return lval_err("jit-fn: invalid formal — expected symbol or {type name}");
    }
  }
  sig += ")";

  std::string body = rut_to_cpp_stmt(body_node, "  ", returns_value, ctx);
  std::string code = sig + " {\n" + body + "}\n";

  if (g_debug) rut_print("[jit-fn] declaring:\n%s\n", code.c_str());

  if (!rut_declare(code.c_str())) {
    lval_del(a);
    return lval_err("jit-fn: Declare failed for %s", name.c_str());
  }

  lval_del(a);
  return lval_jitfn(name.c_str(), nparams);
}

// ---------------------------------------------------------------------------
// (col-jit-fn ret-type-or-{types} lambda)
//
// ret-type:  "float"            — single output column
//            {float float ...}  — multiple output columns
//
// Formals: {col name}  — column argument (emitted as name[i] inside the loop)
//          {type name} — scalar argument (constant across the loop, folded if bound)
//
// Generates a kernel:
//   void __rut_cjit_N(size_t __n, float* __out0, ..., float* a, float* b, ...)
//   { for (size_t i = 0; i < __n; i++) { ... } }
//
// And a fixed-signature dispatch shim (so call sites always use one pointer type):
//   void __rut_cjit_N_dispatch(size_t __n, float** __args)
//   { __rut_cjit_N(__n, __args[0], __args[1], ...); }
// ---------------------------------------------------------------------------
lval* builtin_col_jit_fn(lenv* e, lval* a) {
  if (a->count < 2) {
    lval_del(a);
    return lval_err("col-jit-fn: expected (col-jit-fn ret-type lambda)");
  }

  // Parse return types (first argument).
  std::vector<std::string> ret_types;
  lval* ret_arg = a->cell[0];
  if (ret_arg->type == LVAL_STR) {
    ret_types.push_back(ret_arg->str);
  } else if (ret_arg->type == LVAL_QEXPR) {
    for (int i = 0; i < ret_arg->count; i++) {
      lval* t = ret_arg->cell[i];
      ret_types.push_back((t->type == LVAL_SYM) ? std::string(t->sym) : std::string(t->str));
    }
  } else {
    lval_del(a);
    return lval_err("col-jit-fn: first argument must be a return type or {type ...} list");
  }
  if (ret_types.empty()) { lval_del(a); return lval_err("col-jit-fn: no return types"); }
  int n_outputs = (int)ret_types.size();

  lval* fn = a->cell[1];
  if (fn->type != LVAL_FUN || fn->builtin) {
    lval_del(a);
    return lval_err("col-jit-fn: second argument must be a lambda");
  }

  // Build transpilation context — identify col vs scalar formals.
  JitCtx ctx;
  ctx.env       = e;
  ctx.n_outputs = n_outputs;

  // Collect formal types: {col name} → col_params; {type name} or sym → params.
  std::vector<std::pair<std::string,std::string>> formals_info; // (type, cpp_name)
  for (int i = 0; i < fn->formals->count; i++) {
    lval* formal = fn->formals->cell[i];
    if (formal->type == LVAL_SYM && strcmp(formal->sym, "&") == 0) continue;
    if (formal->type == LVAL_QEXPR && formal->count == 2 &&
        formal->cell[1]->type == LVAL_SYM) {
      std::string type_str = (formal->cell[0]->type == LVAL_SYM)
                             ? std::string(formal->cell[0]->sym)
                             : std::string(formal->cell[0]->str);
      std::string cname = to_cpp_id(formal->cell[1]->sym);
      if (type_str == "col") {
        ctx.col_params.insert(cname);
        formals_info.push_back({"float*", cname});
      } else {
        ctx.params.insert(cname);
        formals_info.push_back({type_str, cname});
      }
    } else if (formal->type == LVAL_SYM) {
      std::string cname = to_cpp_id(formal->sym);
      ctx.params.insert(cname);
      formals_info.push_back({"auto", cname});
    } else {
      lval_del(a);
      return lval_err("col-jit-fn: invalid formal — expected {col name} or {type name}");
    }
  }
  int n_inputs = (int)formals_info.size();

  std::string name = "__rut_cjit_" + std::to_string(g_coljit_counter++);
  std::string dispatch_name = name + "_dispatch";

  // Build kernel signature: void name(size_t __n, float* __restrict__ __out0, ..., inputs...)
  // __restrict__ lets the compiler assume no aliasing between output and input buffers,
  // enabling vectorisation without alias-check overhead.
  std::string sig = "void " + name + "(size_t __n";
  for (int j = 0; j < n_outputs; j++)
    sig += ", " + ret_types[j] + "* __restrict__ __out" + std::to_string(j);
  for (auto& [type, cname] : formals_info) {
    // Pointer inputs (column buffers) also get __restrict__.
    bool is_ptr = !type.empty() && type.back() == '*';
    sig += is_ptr ? ", " + type + " __restrict__ " + cname
                  : ", " + type + " " + cname;
  }
  sig += ")";

  // Build loop body from the lambda body.
  lval* body_node = (fn->body->count == 1) ? fn->body->cell[0] : fn->body;
  // Pre-scan for (= {name} ...) bindings so they're not constant-folded from env.
  collect_locals(body_node, ctx);
  std::string loop_body = rut_to_cpp_stmt(body_node, "    ", /*tail=*/true, ctx);

  std::string kernel_code = sig + " {\n"
    "  for (size_t i = 0; i < __n; i++) {\n"
    + loop_body +
    "  }\n}\n";

  // Build dispatch shim: void name_dispatch(size_t __n, float** __args)
  // args layout: [out0, out1, ..., in0, in1, ...]
  std::string disp_sig = "void " + dispatch_name + "(size_t __n, float** __args)";
  std::string disp_call = "  " + name + "(__n";
  for (int j = 0; j < n_outputs; j++)
    disp_call += ", (" + ret_types[j] + "*)__args[" + std::to_string(j) + "]";
  for (int j = 0; j < n_inputs; j++)
    disp_call += ", (float*)__args[" + std::to_string(n_outputs + j) + "]";
  disp_call += ");\n";

  std::string dispatch_code = disp_sig + " {\n" + disp_call + "}\n";

  // #pragma cling optimize(2) scopes to this transaction only — no need to restore.
  std::string full_code = "#pragma cling optimize(2)\n" + kernel_code + dispatch_code;
  if (g_debug) rut_print("[col-jit-fn] declaring:\n%s\n", full_code.c_str());

  if (!rut_declare(full_code.c_str())) {
    lval_del(a);
    return lval_err("col-jit-fn: Declare failed for %s", name.c_str());
  }

  // Resolve dispatch pointer immediately (we're on the main thread post-Declare).
  void* dp = (void*)rut_calc(("(Long_t)&" + dispatch_name).c_str());
  if (!dp) {
    lval_del(a);
    return lval_err("col-jit-fn: failed to resolve dispatch pointer for %s", dispatch_name.c_str());
  }

  lval_del(a);
  return lval_coljitfn(name.c_str(), n_inputs, n_outputs, dp);
}

// Map C++ type name → ColDtype (mirrors col_dtype_from_name in rut_column.cxx)
static int kernel_dtype_from_name(const char* name) {
  if (strcmp(name, "float")              == 0) return COL_FLOAT32;
  if (strcmp(name, "double")             == 0) return COL_FLOAT64;
  if (strcmp(name, "int")                == 0) return COL_INT32;
  if (strcmp(name, "unsigned int")       == 0) return COL_UINT32;
  if (strcmp(name, "long long")          == 0) return COL_INT64;
  if (strcmp(name, "unsigned long long") == 0) return COL_UINT64;
  if (strcmp(name, "short")              == 0) return COL_INT16;
  if (strcmp(name, "unsigned short")     == 0) return COL_UINT16;
  if (strcmp(name, "char")               == 0) return COL_INT8;
  if (strcmp(name, "unsigned char")      == 0) return COL_UINT8;
  if (strcmp(name, "bool")               == 0) return COL_BOOL;
  return -1;
}

// ---------------------------------------------------------------------------
// (col-kernel decl-lambda body-lambda)
//
// decl-lambda formals: {col name}  — float32 input column
// decl-lambda body:    {out dtype name size-expr init}  — output spec
// body-lambda:         (\{i} { ... (scatter! name idx val) ... })
//
// Generates:
//   void __rut_kernel_N(size_t __n, T* __restrict__ out0, const float* __restrict__ in0, ...)
//   void __rut_kernel_N_dispatch(size_t __n, void** __args)
// ---------------------------------------------------------------------------
lval* builtin_col_kernel(lenv* e, lval* a) {
  if (a->count != 2) {
    lval_del(a);
    return lval_err("col-kernel: expected (col-kernel decl-lambda body-lambda)");
  }
  lval* decl_fn = a->cell[0];
  lval* body_fn = a->cell[1];
  if (decl_fn->type != LVAL_FUN || decl_fn->builtin) {
    lval_del(a);
    return lval_err("col-kernel: first argument must be a lambda");
  }
  if (body_fn->type != LVAL_FUN || body_fn->builtin) {
    lval_del(a);
    return lval_err("col-kernel: second argument must be a lambda");
  }

  JitCtx ctx;
  ctx.env         = e;
  ctx.kernel_mode = true;

  // Parse decl-lambda formals: each must be {col name}
  std::vector<std::string> input_names;
  for (int i = 0; i < decl_fn->formals->count; i++) {
    lval* formal = decl_fn->formals->cell[i];
    if (formal->type == LVAL_QEXPR && formal->count == 2 &&
        formal->cell[0]->type == LVAL_SYM &&
        strcmp(formal->cell[0]->sym, "col") == 0 &&
        formal->cell[1]->type == LVAL_SYM) {
      std::string cname = to_cpp_id(formal->cell[1]->sym);
      ctx.col_params.insert(cname);
      input_names.push_back(cname);
    } else {
      lval_del(a);
      return lval_err("col-kernel: decl-lambda formals must be {col name}");
    }
  }

  // Parse decl-lambda body: either one {out dtype name size init} spec
  // or a QEXPR whose children are such specs.
  lval* decl_body = decl_fn->body;

  KernelMeta* meta = new KernelMeta();
  meta->n_inputs = (int)input_names.size();

  // Check if decl_body IS a single {out ...} spec (starts with SYM "out")
  bool single = (decl_body->type == LVAL_QEXPR && decl_body->count == 5 &&
                 decl_body->cell[0]->type == LVAL_SYM &&
                 strcmp(decl_body->cell[0]->sym, "out") == 0);

  std::vector<lval*> specs;
  if (single) {
    specs.push_back(decl_body);
  } else {
    for (int i = 0; i < decl_body->count; i++)
      specs.push_back(decl_body->cell[i]);
  }

  for (lval* spec : specs) {
    if (spec->type != LVAL_QEXPR || spec->count != 5 ||
        spec->cell[0]->type != LVAL_SYM ||
        strcmp(spec->cell[0]->sym, "out") != 0 ||
        spec->cell[2]->type != LVAL_SYM) {
      delete meta;
      lval_del(a);
      return lval_err("col-kernel: output spec must be {out dtype name size-expr init}");
    }
    // dtype
    std::string dtype_str = (spec->cell[1]->type == LVAL_SYM)
                            ? std::string(spec->cell[1]->sym)
                            : std::string(spec->cell[1]->str);
    int dtype = kernel_dtype_from_name(dtype_str.c_str());
    if (dtype < 0) {
      delete meta;
      lval_del(a);
      return lval_err("col-kernel: unknown output dtype '%s'", dtype_str.c_str());
    }
    std::string out_name = to_cpp_id(spec->cell[2]->sym);
    // Evaluate size-expr in the current environment to get a constant
    lval* size_v = lval_eval(e, lval_copy(spec->cell[3]));
    size_t size_int = 0;
    if (size_v->type == LVAL_NUM)   size_int = (size_t)size_v->num;
    else if (size_v->type == LVAL_FLOAT) size_int = (size_t)size_v->floating;
    else {
      lval_del(size_v);
      delete meta;
      lval_del(a);
      return lval_err("col-kernel: size-expr must evaluate to a number");
    }
    lval_del(size_v);
    std::string init_cpp = rut_to_cpp_expr(spec->cell[4], ctx);

    KernelOutInfo info;
    info.cpp_type = dtype_str;
    info.name     = out_name;
    info.size_val = size_int;
    info.init     = init_cpp;
    ctx.kernel_outs[out_name] = info;
    ctx.kernel_out_order.push_back(out_name);
    meta->outputs.push_back({ dtype, size_int });
  }

  if (meta->outputs.empty()) {
    delete meta;
    lval_del(a);
    return lval_err("col-kernel: no output specs found");
  }

  // Transpile body lambda
  lval* body_node = (body_fn->body->count == 1) ? body_fn->body->cell[0] : body_fn->body;
  collect_locals(body_node, ctx);
  std::string loop_body = rut_to_cpp_stmt(body_node, "    ", /*tail=*/false, ctx);

  std::string name = "__rut_kernel_" + std::to_string(g_kernel_counter++);
  std::string dispatch_name = name + "_dispatch";

  // Build kernel signature: outputs first (in insertion order), then inputs
  std::string kernel_sig = "void " + name + "(size_t __n";
  for (auto& oname : ctx.kernel_out_order)
    kernel_sig += ", " + ctx.kernel_outs.at(oname).cpp_type + "* __restrict__ " + oname;
  for (auto& iname : input_names)
    kernel_sig += ", const float* __restrict__ " + iname;
  kernel_sig += ")";

  // Output init block and main loop (insertion order)
  std::string inits;
  for (auto& oname : ctx.kernel_out_order) {
    auto& oi = ctx.kernel_outs.at(oname);
    inits += "  const size_t " + oname + "_n = " + std::to_string(oi.size_val) + ";\n";
    inits += "  for (size_t __k = 0; __k < " + oname + "_n; __k++) "
           + oname + "[__k] = " + oi.init + ";\n";
  }

  std::string kernel_code = kernel_sig + " {\n"
    + inits
    + "  for (size_t i = 0; i < __n; i++) {\n"
    + loop_body
    + "  }\n}\n";

  // Dispatch shim: void** args layout = [out0, ..., in0, ...]
  std::string disp_sig  = "void " + dispatch_name + "(size_t __n, void** __args)";
  std::string disp_call = "  " + name + "(__n";
  int arg_idx = 0;
  for (auto& oname : ctx.kernel_out_order)
    disp_call += ", (" + ctx.kernel_outs.at(oname).cpp_type + "*)__args[" + std::to_string(arg_idx++) + "]";
  for (int j = 0; j < (int)input_names.size(); j++)
    disp_call += ", (const float*)__args[" + std::to_string(arg_idx++) + "]";
  disp_call += ");\n";

  std::string full_code = "#pragma cling optimize(2)\n" + kernel_code
                        + disp_sig + " {\n" + disp_call + "}\n";

  if (g_debug) rut_print("[col-kernel] declaring:\n%s\n", full_code.c_str());

  if (!rut_declare(full_code.c_str())) {
    delete meta;
    lval_del(a);
    return lval_err("col-kernel: Declare failed for %s", name.c_str());
  }

  void* dp = (void*)rut_calc(("(Long_t)&" + dispatch_name).c_str());
  if (!dp) {
    delete meta;
    lval_del(a);
    return lval_err("col-kernel: failed to resolve dispatch pointer for %s", dispatch_name.c_str());
  }

  meta->dispatch = dp;
  lval_del(a);
  return lval_kerneljitfn(name.c_str(), meta);
}

void lenv_add_builtins_jitfn(lenv* e) {
  lenv_add_builtin(e, "jit-fn",     builtin_jit_fn);
  lenv_add_builtin(e, "col-jit-fn", builtin_col_jit_fn);
  lenv_add_builtin(e, "col-kernel", builtin_col_kernel);
}
