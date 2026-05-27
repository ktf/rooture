#include "rooture.h"

// ---------------------------------------------------------------------------
// GLSL shader transpiler
// ---------------------------------------------------------------------------

// Sanitise a rooture identifier to a valid GLSL identifier (hyphen → underscore).
static std::string to_shader_id(const char* s) {
  std::string r(s);
  for (char& c : r) if (c == '-') c = '_';
  return r;
}

// Format a double for GLSL: shortest representation that round-trips,
// always with a decimal point so GLSL 1.20 sees a float literal (not int).
static std::string fmt_glsl_float(double v) {
  char buf[32];
  std::string s;
  for (int p = 1; p <= 17; p++) {
    snprintf(buf, sizeof(buf), "%.*g", p, v);
    double rt;
    if (sscanf(buf, "%lf", &rt) == 1 && rt == v) { s = buf; break; }
  }
  if (s.empty()) { snprintf(buf, sizeof(buf), "%.17g", v); s = buf; }
  // Ensure a decimal point so GLSL treats it as float, not int.
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos)
    s += ".0";
  return s;
}

// TMath static method → GLSL function / literal
static const std::unordered_map<std::string, std::string> tmath_to_glsl = {
  {"Sin",    "sin"},
  {"Cos",    "cos"},
  {"Tan",    "tan"},
  {"ASin",   "asin"},
  {"ACos",   "acos"},
  {"ATan",   "atan"},
  {"ATan2",  "atan"},
  {"Abs",    "abs"},
  {"Sqrt",   "sqrt"},
  {"Log",    "log"},
  {"Log2",   "log2"},
  {"Log10",  "log2"},  // GLSL has no log10; could emulate, but map to log2 for now
  {"Exp",    "exp"},
  {"Floor",  "floor"},
  {"Ceil",   "ceil"},
  {"Power",  "pow"},
  {"Min",    "min"},
  {"Max",    "max"},
  {"Pi",     "3.14159265358979"},
  {"TwoPi",  "6.28318530717959"},
  {"PiOver2","1.57079632679490"},
};

// GLSL builtin function names
static const std::set<std::string> glsl_builtins = {
  "sin", "cos", "tan", "asin", "acos", "atan",
  "radians", "degrees",
  "pow", "exp", "exp2", "log", "log2", "sqrt", "inversesqrt",
  "abs", "sign", "floor", "ceil", "fract", "mod",
  "min", "max", "clamp", "mix", "step", "smoothstep",
  "length", "distance", "dot", "cross", "normalize",
  "faceforward", "reflect", "refract",
  "texture2D", "texture",
  "dFdx", "dFdy", "fwidth",
};

// GLSL type constructors
static const std::set<std::string> glsl_constructors = {
  "float", "int", "bool",
  "vec2", "vec3", "vec4",
  "ivec2", "ivec3", "ivec4",
  "bvec2", "bvec3", "bvec4",
  "mat2", "mat3", "mat4",
};

// GLSL global variables (no constant-folding)
static const std::set<std::string> glsl_globals = {
  "gl_Vertex", "gl_Position", "gl_Normal",
  "gl_ModelViewMatrix", "gl_ModelViewProjectionMatrix", "gl_NormalMatrix",
  "gl_ProjectionMatrix",
  "gl_Color", "gl_FrontColor", "gl_BackColor",
  "gl_FragColor", "gl_FragCoord", "gl_FrontFacing",
  "gl_PointSize", "gl_TexCoord",
};

// Returns true if all chars are valid swizzle components
static bool is_swizzle(const char* s) {
  if (!s || !*s) return false;
  for (const char* p = s; *p; p++)
    if (!strchr("xyzwrgbastpq", *p)) return false;
  return true;
}

// Transpilation context
struct ShaderCtx {
  lenv* env;
  std::set<std::string> locals;  // names bound by (= {type name} ...)
};

// Forward declaration
static std::string shader_to_stmt(lval* v, const std::string& ind, ShaderCtx& ctx);

// Pre-scan an AST node and collect every name bound by (= {name} ...) or
// (= {type name} ...) into ctx.locals.
static void collect_shader_locals(lval* v, ShaderCtx& ctx) {
  if (!v) return;
  if (v->type == LVAL_QEXPR || v->type == LVAL_SEXPR) {
    if (v->type == LVAL_SEXPR && v->count == 3 &&
        v->cell[0]->type == LVAL_SYM &&
        strcmp(v->cell[0]->sym, "=") == 0) {
      lval* lhs = v->cell[1];
      if (lhs->type == LVAL_QEXPR) {
        if (lhs->count == 1 && lhs->cell[0]->type == LVAL_SYM)
          ctx.locals.insert(to_shader_id(lhs->cell[0]->sym));
        else if (lhs->count == 2 && lhs->cell[1]->type == LVAL_SYM)
          ctx.locals.insert(to_shader_id(lhs->cell[1]->sym));
      }
    }
    for (int i = 0; i < v->count; i++) collect_shader_locals(v->cell[i], ctx);
  }
}

// Transpile a single expression to a GLSL expression string.
static std::string shader_to_expr(lval* v, ShaderCtx& ctx) {
  if (!v) return "/*null*/";

  // Numeric literals
  if (v->type == LVAL_NUM) return std::to_string(v->num);
  if (v->type == LVAL_FLOAT) return fmt_glsl_float(v->floating);
  if (v->type == LVAL_FLOAT32) return fmt_glsl_float((double)(float)v->floating);

  // Symbols
  if (v->type == LVAL_SYM) {
    std::string id = to_shader_id(v->sym);
    // GLSL globals — emit as-is
    if (glsl_globals.count(id)) return id;
    // Local variable — emit as-is
    if (ctx.locals.count(id)) return id;
    // Try constant-folding from env (NUM/FLOAT only)
    if (ctx.env) {
      lval tmp; tmp.type = LVAL_SYM; tmp.sym = v->sym;
      lval* found = lenv_get(ctx.env, &tmp);
      if (found) {
        std::string r;
        if (found->type == LVAL_NUM)
          r = std::to_string(found->num);
        else if (found->type == LVAL_FLOAT)
          r = fmt_glsl_float(found->floating);
        else if (found->type == LVAL_FLOAT32)
          r = fmt_glsl_float((double)(float)found->floating);
        lval_del(found);
        if (!r.empty()) return r;
      }
    }
    return id;  // fallback
  }

  // Unwrap single-element Q-expression
  if (v->type == LVAL_QEXPR) {
    if (v->count == 1) return shader_to_expr(v->cell[0], ctx);
  }

  if ((v->type != LVAL_SEXPR && v->type != LVAL_QEXPR) || v->count == 0)
    return "/*unsupported*/";

  lval* head = v->cell[0];
  if (head->type != LVAL_SYM) return "/*unsupported-head*/";
  const char* s = head->sym;

  // Binary ops
  const char* binop = nullptr;
  if (strcmp(s, "+")  == 0) binop = "+";
  if (strcmp(s, "-")  == 0) binop = "-";
  if (strcmp(s, "*")  == 0) binop = "*";
  if (strcmp(s, "/")  == 0) binop = "/";
  if (strcmp(s, "<")  == 0) binop = "<";
  if (strcmp(s, ">")  == 0) binop = ">";
  if (strcmp(s, "<=") == 0) binop = "<=";
  if (strcmp(s, ">=") == 0) binop = ">=";
  if (strcmp(s, "==") == 0) binop = "==";
  if (strcmp(s, "!=") == 0) binop = "!=";
  if (strcmp(s, "%")  == 0) binop = "%";
  if (binop && v->count >= 3) {
    std::string result = shader_to_expr(v->cell[1], ctx);
    for (int i = 2; i < v->count; i++)
      result = "(" + result + " " + binop + " " + shader_to_expr(v->cell[i], ctx) + ")";
    return result;
  }

  // Unary minus: (- x) with exactly 2 children
  if (strcmp(s, "-") == 0 && v->count == 2)
    return "(-" + shader_to_expr(v->cell[1], ctx) + ")";

  // Logic
  if (strcmp(s, "not") == 0 && v->count == 2)
    return "(!(" + shader_to_expr(v->cell[1], ctx) + "))";
  if (strcmp(s, "and") == 0 && v->count == 3)
    return "(" + shader_to_expr(v->cell[1], ctx) + " && " + shader_to_expr(v->cell[2], ctx) + ")";
  if (strcmp(s, "or") == 0 && v->count == 3)
    return "(" + shader_to_expr(v->cell[1], ctx) + " || " + shader_to_expr(v->cell[2], ctx) + ")";

  // Casts: (to-int x) → int(x), (to-float x) → float(x)
  if (strcmp(s, "to-int") == 0 && v->count == 2)
    return "int(" + shader_to_expr(v->cell[1], ctx) + ")";
  if (strcmp(s, "to-float") == 0 && v->count == 2)
    return "float(" + shader_to_expr(v->cell[1], ctx) + ")";

  // Swizzle: (.xyz v) where xyz is all swizzle chars and count==2
  if (s[0] == '.' && s[1] != '\0' && v->count == 2 && is_swizzle(s + 1))
    return shader_to_expr(v->cell[1], ctx) + "." + std::string(s + 1);

  // TMath static: (:: Method TMath args...) or (::Method TMath args...)
  if (strcmp(s, "::") == 0 && v->count >= 3) {
    lval* meth = v->cell[1];
    lval* cls  = v->cell[2];
    if (meth->type == LVAL_SYM && cls->type == LVAL_SYM &&
        strcmp(cls->sym, "TMath") == 0) {
      auto it = tmath_to_glsl.find(meth->sym);
      if (it != tmath_to_glsl.end()) {
        // Constants (no args): Pi, TwoPi, etc.
        if (v->count == 3 && (strcmp(meth->sym, "Pi") == 0 ||
            strcmp(meth->sym, "TwoPi") == 0 ||
            strcmp(meth->sym, "PiOver2") == 0))
          return it->second;
        // Function call
        std::string call = it->second + "(";
        for (int i = 3; i < v->count; i++) {
          if (i > 3) call += ", ";
          call += shader_to_expr(v->cell[i], ctx);
        }
        call += ")";
        return call;
      }
    }
  }
  // (::Method TMath args...) — sugared form
  if (s[0] == ':' && s[1] == ':' && v->count >= 2) {
    lval* cls = v->cell[1];
    if (cls->type == LVAL_SYM && strcmp(cls->sym, "TMath") == 0) {
      std::string meth_name(s + 2);
      auto it = tmath_to_glsl.find(meth_name);
      if (it != tmath_to_glsl.end()) {
        if (v->count == 2 && (meth_name == "Pi" || meth_name == "TwoPi" || meth_name == "PiOver2"))
          return it->second;
        std::string call = it->second + "(";
        for (int i = 2; i < v->count; i++) {
          if (i > 2) call += ", ";
          call += shader_to_expr(v->cell[i], ctx);
        }
        call += ")";
        return call;
      }
    }
  }

  // GLSL type constructor: (vec3 a b c) → vec3(a, b, c)
  {
    std::string head_id = to_shader_id(s);
    if (glsl_constructors.count(head_id)) {
      std::string call = head_id + "(";
      for (int i = 1; i < v->count; i++) {
        if (i > 1) call += ", ";
        call += shader_to_expr(v->cell[i], ctx);
      }
      call += ")";
      return call;
    }
  }

  // GLSL builtin function: (sin x) → sin(x)
  {
    std::string head_id = to_shader_id(s);
    if (glsl_builtins.count(head_id)) {
      std::string call = head_id + "(";
      for (int i = 1; i < v->count; i++) {
        if (i > 1) call += ", ";
        call += shader_to_expr(v->cell[i], ctx);
      }
      call += ")";
      return call;
    }
  }

  // Ternary: (if cond {t} {f}) as expression
  if (strcmp(s, "if") == 0 && v->count == 4)
    return "(" + shader_to_expr(v->cell[1], ctx) + " ? "
         + shader_to_expr(v->cell[2], ctx) + " : "
         + shader_to_expr(v->cell[3], ctx) + ")";

  // Fallback: treat as generic function call
  {
    std::string fn = to_shader_id(s);
    std::string call = fn + "(";
    for (int i = 1; i < v->count; i++) {
      if (i > 1) call += ", ";
      call += shader_to_expr(v->cell[i], ctx);
    }
    call += ")";
    return call;
  }
}

// Transpile a statement (or block) to GLSL.
static std::string shader_to_stmt(lval* v, const std::string& ind, ShaderCtx& ctx) {
  if (!v) return "";

  // Unwrap QEXPR
  if (v->type == LVAL_QEXPR) {
    if (v->count == 0) return "";
    if (v->count == 1) return shader_to_stmt(v->cell[0], ind, ctx);
    // {do e1 e2 ...}
    if (v->cell[0]->type == LVAL_SYM && strcmp(v->cell[0]->sym, "do") == 0) {
      std::string out;
      for (int i = 1; i < v->count; i++)
        out += shader_to_stmt(v->cell[i], ind, ctx);
      return out;
    }
    // Multi-element QEXPR — treat as sequential block
    std::string out;
    for (int i = 0; i < v->count; i++)
      out += shader_to_stmt(v->cell[i], ind, ctx);
    return out;
  }

  if (v->type != LVAL_SEXPR || v->count == 0)
    return ind + shader_to_expr(v, ctx) + ";\n";

  lval* head = v->cell[0];
  if (head->type != LVAL_SYM)
    return ind + shader_to_expr(v, ctx) + ";\n";
  const char* s = head->sym;

  // (do e1 e2 ...)
  if (strcmp(s, "do") == 0) {
    std::string out;
    for (int i = 1; i < v->count; i++)
      out += shader_to_stmt(v->cell[i], ind, ctx);
    return out;
  }

  // (= {type name} expr) → typed local
  // (= {name} expr)      → float name = expr;
  if (strcmp(s, "=") == 0 && v->count == 3) {
    lval* lhs = v->cell[1];
    if (lhs->type == LVAL_QEXPR && lhs->count == 2 &&
        lhs->cell[0]->type == LVAL_SYM && lhs->cell[1]->type == LVAL_SYM) {
      std::string type = to_shader_id(lhs->cell[0]->sym);
      std::string name = to_shader_id(lhs->cell[1]->sym);
      ctx.locals.insert(name);
      return ind + type + " " + name + " = " + shader_to_expr(v->cell[2], ctx) + ";\n";
    }
    if (lhs->type == LVAL_QEXPR && lhs->count == 1 && lhs->cell[0]->type == LVAL_SYM) {
      std::string name = to_shader_id(lhs->cell[0]->sym);
      ctx.locals.insert(name);
      return ind + "float " + name + " = " + shader_to_expr(v->cell[2], ctx) + ";\n";
    }
  }

  // (set! name expr) → name = expr;
  if (strcmp(s, "set!") == 0 && v->count == 3) {
    std::string name;
    if (v->cell[1]->type == LVAL_SYM)
      name = to_shader_id(v->cell[1]->sym);
    else
      name = shader_to_expr(v->cell[1], ctx);
    return ind + name + " = " + shader_to_expr(v->cell[2], ctx) + ";\n";
  }

  // (+= name expr) → name += expr;
  if (strcmp(s, "+=") == 0 && v->count == 3) {
    std::string name;
    if (v->cell[1]->type == LVAL_SYM)
      name = to_shader_id(v->cell[1]->sym);
    else
      name = shader_to_expr(v->cell[1], ctx);
    return ind + name + " += " + shader_to_expr(v->cell[2], ctx) + ";\n";
  }

  // (*= name expr) → name *= expr;
  if (strcmp(s, "*=") == 0 && v->count == 3) {
    std::string name;
    if (v->cell[1]->type == LVAL_SYM)
      name = to_shader_id(v->cell[1]->sym);
    else
      name = shader_to_expr(v->cell[1], ctx);
    return ind + name + " *= " + shader_to_expr(v->cell[2], ctx) + ";\n";
  }

  // (if cond {t-body} {f-body})
  if (strcmp(s, "if") == 0 && v->count == 4) {
    std::string out = ind + "if (" + shader_to_expr(v->cell[1], ctx) + ") {\n";
    out += shader_to_stmt(v->cell[2], ind + "  ", ctx);
    out += ind + "} else {\n";
    out += shader_to_stmt(v->cell[3], ind + "  ", ctx);
    out += ind + "}\n";
    return out;
  }
  // (if cond {t-body})
  if (strcmp(s, "if") == 0 && v->count == 3) {
    std::string out = ind + "if (" + shader_to_expr(v->cell[1], ctx) + ") {\n";
    out += shader_to_stmt(v->cell[2], ind + "  ", ctx);
    out += ind + "}\n";
    return out;
  }

  // (dotimes {i} N {body}) → for loop
  if (strcmp(s, "dotimes") == 0 && v->count == 4) {
    lval* sym_q = v->cell[1];
    std::string ivar = (sym_q->type == LVAL_QEXPR && sym_q->count == 1 &&
                         sym_q->cell[0]->type == LVAL_SYM)
                        ? to_shader_id(sym_q->cell[0]->sym) : "i";
    ctx.locals.insert(ivar);
    std::string n = shader_to_expr(v->cell[2], ctx);
    std::string body = shader_to_stmt(v->cell[3], ind + "  ", ctx);
    return ind + "for (int " + ivar + " = 0; " + ivar + " < " + n + "; " + ivar + "++) {\n"
           + body + ind + "}\n";
  }

  // Fallback: expression statement
  return ind + shader_to_expr(v, ctx) + ";\n";
}

// Parse declarations: each child of the Q-expression is (qualifier type name).
static std::string parse_declarations(lval* decls) {
  if (!decls || decls->type != LVAL_QEXPR) return "";
  std::string out;
  for (int i = 0; i < decls->count; i++) {
    lval* d = decls->cell[i];
    if (d->type != LVAL_SEXPR && d->type != LVAL_QEXPR) continue;
    if (d->count != 3) continue;
    // (qualifier type name) — all symbols
    std::string qual = (d->cell[0]->type == LVAL_SYM) ? d->cell[0]->sym : "";
    std::string type = (d->cell[1]->type == LVAL_SYM) ? d->cell[1]->sym : "";
    std::string name = (d->cell[2]->type == LVAL_SYM) ? to_shader_id(d->cell[2]->sym) : "";
    out += qual + " " + type + " " + name + ";\n";
  }
  return out;
}

// (glsl-vert declarations body) → LVAL_STR
lval* builtin_glsl_vert(lenv* e, lval* a) {
  LASSERT_NUM("glsl-vert", a, 2);
  LASSERT_TYPE("glsl-vert", a, 0, LVAL_QEXPR);
  LASSERT_TYPE("glsl-vert", a, 1, LVAL_QEXPR);

  ShaderCtx ctx;
  ctx.env = e;

  // Pre-scan body for local variable names
  collect_shader_locals(a->cell[1], ctx);

  std::string decls = parse_declarations(a->cell[0]);
  std::string body  = shader_to_stmt(a->cell[1], "  ", ctx);
  std::string result = decls + "void main() {\n" + body + "}\n";

  lval_del(a);
  return lval_str(result.c_str());
}

// (glsl-frag declarations body) → LVAL_STR
lval* builtin_glsl_frag(lenv* e, lval* a) {
  LASSERT_NUM("glsl-frag", a, 2);
  LASSERT_TYPE("glsl-frag", a, 0, LVAL_QEXPR);
  LASSERT_TYPE("glsl-frag", a, 1, LVAL_QEXPR);

  ShaderCtx ctx;
  ctx.env = e;

  collect_shader_locals(a->cell[1], ctx);

  std::string decls = parse_declarations(a->cell[0]);
  std::string body  = shader_to_stmt(a->cell[1], "  ", ctx);
  std::string result = decls + "void main() {\n" + body + "}\n";

  lval_del(a);
  return lval_str(result.c_str());
}

void lenv_add_builtins_shader(lenv* e) {
  lenv_add_builtin(e, "glsl-vert", builtin_glsl_vert);
  lenv_add_builtin(e, "glsl-frag", builtin_glsl_frag);
}
