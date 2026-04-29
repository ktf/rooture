#include "rooture.h"

// ---------------------------------------------------------------------------
// TMethodCall cache
// ---------------------------------------------------------------------------

// Which SetParam overload to call for a given C++ parameter type.
// Derived from TMethodArg::GetFullTypeName() at cache-population time so that
// LVAL_NUM (Long_t) is correctly promoted to Double_t / Float_t when the
// method signature requires it, and 64-bit integer types get the right overload.
enum class ParamKind : uint8_t {
  kLong,    // int, long, bool, pointer, enum → SetParam(Long_t)
  kFloat,   // float, Float_t, Float16_t      → SetParam(Float_t)
  kDouble,  // double, Double_t, Double32_t   → SetParam(Double_t)
  kLong64,  // long long, Long64_t            → SetParam(Long64_t)
  kULong64  // unsigned long long, ULong64_t  → SetParam(ULong64_t)
};

static ParamKind classify_param(const char* tn) {
  // Unsigned 64-bit must be checked before signed (ULong64_t contains "Long64").
  if (strstr(tn, "ULong64") || strstr(tn, "unsigned long long") || strstr(tn, "uint64"))
    return ParamKind::kULong64;
  if (strstr(tn, "Long64") || strstr(tn, "long long") || strstr(tn, "int64"))
    return ParamKind::kLong64;
  // Double before Float (Double32_t still matches "Double").
  if (strstr(tn, "double") || strstr(tn, "Double"))
    return ParamKind::kDouble;
  if (strstr(tn, "float") || strstr(tn, "Float"))
    return ParamKind::kFloat;
  return ParamKind::kLong;
}

struct CachedMethodCall {
  TClass*                       cached_cls;   // obj_cls at population time
  std::unique_ptr<TMethodCall>  mc;
  TMethodCall::EReturnType      ret_type;
  bool                          is_ptr_return;
  TClass*                       ptr_ret_cls;  // valid when is_ptr_return
  std::vector<ParamKind>        param_kinds;  // per-parameter SetParam overload
};
static std::unordered_map<std::string, CachedMethodCall> g_method_cache;

// Typed argument collected before lval_to_cpp_arg consumes the lval list.
struct CacheArg {
  int         type; // LVAL_NUM / LVAL_FLOAT / LVAL_STR / LVAL_TOBJ
  long        num;
  double      flt;
  std::string str;
  void*       obj;
};

// ---------------------------------------------------------------------------
// Cling string utilities
// ---------------------------------------------------------------------------

// Escape a raw C string for embedding inside a C++ double-quoted literal that
// will be fed to Cling's ProcessLine / Execute.  Backslashes and double-quotes
// must be escaped; other characters are passed through unchanged.
std::string escape_for_cling_str(const char* s) {
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

// ---------------------------------------------------------------------------
// Callable bridge
// ---------------------------------------------------------------------------

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

  // C++20 abbreviated function template: valid at global scope and lets RDF
  // (or any other caller) instantiate with the actual argument types.
  std::string decl = "double " + key + "_wrapper(";
  std::string arr;
  for (int i = 0; i < nargs; i++) {
    if (i) { decl += ", "; arr += ", "; }
    decl += "auto _a" + std::to_string(i);
    arr  += "(double)_a" + std::to_string(i);
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

// build_deref_mask: return mask[i]=true if the i-th method argument is a reference
// type (meaning rooture should emit *((Cls*)ptr) rather than ((Cls*)ptr)).
// Uses two strategies:
//   S1: iterate GetListOfMethods() → TMethodArg::GetFullTypeName() — works for most classes.
//   S2: per-TOBJ-arg GetMethodWithPrototype probe — fallback for template-class methods
//       where GetListOfMethodArgs() returns nothing (ROOT reflection limitation).
// Returns empty vector if no information could be determined.
static std::vector<bool> build_deref_mask(TClass* cls, const char* method_name,
                                           lval* a, int offset) {
  if (!cls || !method_name) return {};
  int nargs = a->count - offset;
  if (nargs <= 0) return {};

  // S1: iterate method list, check reflected arg types.
  {
    TIter next(cls->GetListOfMethods());
    TObject* obj;
    while ((obj = next())) {
      TMethod* m = (TMethod*)obj;
      if (strcmp(m->GetName(), method_name) != 0) continue;
      if (m->GetNargs() < nargs || m->GetNargs() - m->GetNargsOpt() > nargs) continue;
      TList* al = m->GetListOfMethodArgs();
      if (!al || al->GetSize() == 0) continue;  // template method — args not reflected
      std::vector<bool> mask;
      TIter mi(al);
      TObject* aobj;
      while ((aobj = mi())) {
        TMethodArg* ma = (TMethodArg*)aobj;
        std::string ft = ma->GetFullTypeName();
        mask.push_back(ft.find('&') != std::string::npos);
      }
      if (!mask.empty()) return mask;
    }
  }

  // S2: for each TOBJ argument, probe whether the class accepts the arg type as
  // a pointer.  If GetMethodWithPrototype fails for the pointer form but succeeds
  // for the const-reference form, the argument needs dereferencing.
  // (Works even when GetListOfMethodArgs() is empty for template methods.)
  std::vector<bool> mask(nargs, false);
  bool any_deref = false;
  for (int i = offset; i < a->count; i++) {
    lval* v = a->cell[i];
    if (v->type != LVAL_TOBJ || !v->cls) continue;
    int idx = i - offset;
    std::string cname = v->cls->GetName();
    std::string ptr_proto  = cname + "*";
    std::string ref_proto  = cname + "&";
    std::string cref_proto = "const " + cname + "&";
    bool ptr_ok = (cls->GetMethodWithPrototype(method_name, ptr_proto.c_str())  != nullptr);
    if (!ptr_ok) {
      bool ref_ok = (cls->GetMethodWithPrototype(method_name, ref_proto.c_str())  != nullptr) ||
                    (cls->GetMethodWithPrototype(method_name, cref_proto.c_str()) != nullptr);
      if (ref_ok) { mask[idx] = true; any_deref = true; }
    }
  }
  return any_deref ? mask : std::vector<bool>{};
}

std::string lval_to_cpp_arg(lenv* e, lval* a, int offset,
                             const std::vector<bool>* deref_mask) {
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
        if (v->cls) {
          int arg_idx = i - offset;
          bool deref = deref_mask && arg_idx < (int)deref_mask->size() && (*deref_mask)[arg_idx];
          std::string ptr_expr = "((" + std::string(v->cls->GetName()) + "*)" + ptr_to_hex(v->obj) + ")";
          args += deref ? ("*" + ptr_expr) : ptr_expr;
        } else {
          args += ptr_to_hex(v->obj);
        }
        break;
      case LVAL_JITFN:
        args += v->sym;
        break;
      case LVAL_QEXPR: {
        // Q-expression of strings → inline std::vector<std::string>{"a","b",...}
        // This avoids push_back dispatch issues for column-name vectors.
        bool all_str = (v->count > 0);
        for (int j = 0; j < v->count && all_str; j++)
          if (v->cell[j]->type != LVAL_STR) all_str = false;
        if (all_str) {
          args += "std::vector<std::string>{";
          for (int j = 0; j < v->count; j++) {
            if (j) args += ",";
            args += "\"" + escape_for_cling_str(v->cell[j]->str) + "\"";
          }
          args += "}";
        } else {
          rut_print("Cannot use Q-expr with non-string elements as C++ argument.\n");
        }
        break;
      }
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

// ---------------------------------------------------------------------------
// builtin_member
// ---------------------------------------------------------------------------

// - The first argument must be a string.
// - The second argument must be an object.
// - Rest of the arguments should be passed to the method call, if
//   we are referring to one.
lval* builtin_member(lenv *e, lval *a) {
  if (g_in_future) {
    lval* ac = lval_copy(a); lval* res = nullptr;
    rut_dispatch_work([&]{ res = builtin_member(e, ac); });
    lval_del(a); return res;
  }
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

  // Collect typed arg snapshots for the cache BEFORE lval_to_cpp_arg frees them.
  std::string           cache_key;
  std::vector<CacheArg> cache_args;
  if (!has_callable) {
    cache_key = class_name + "::" + method_name + "(";
    bool cacheable = true;
    for (int i = 0; i < a->count; i++) {
      if (i) cache_key += ",";
      lval* v = a->cell[i];
      CacheArg ca; ca.type = v->type; ca.num = 0; ca.flt = 0.0; ca.obj = nullptr;
      switch (v->type) {
        case LVAL_NUM:   cache_key += "L"; ca.num = (long)v->num;  break;
        case LVAL_FLOAT: cache_key += "D"; ca.flt = v->floating;   break;
        case LVAL_TOBJ:  cache_key += "O"; ca.obj = v->obj;        break;
        // LVAL_STR: no SetParam(const char*) in this ROOT version — skip cache
        default:         cacheable = false;                         break;
      }
      if (!cacheable) break;
      cache_args.push_back(ca);
    }
    if (!cacheable) cache_key = "";
    else            cache_key += ")";
  }

  auto deref_mask_m = build_deref_mask(obj_cls, method_name.c_str(), a, 0);
  std::string args = lval_to_cpp_arg(e, a, 0,
      deref_mask_m.empty() ? nullptr : &deref_mask_m);
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

    // Cache the winning probe form per normalised method+args signature.
    // Key: everything from "->" onward, with hex addresses and string literals
    // replaced by wildcards.  The object class is excluded so different template
    // instantiations (e.g. RInterface<RRange<...>> vs RInterface<RJittedFilter>)
    // share the same Histo3D / Define / … cache entry.
    static std::unordered_map<std::string,int> probe_form_cache;
    static const std::regex pfc_hex(R"(0x[0-9a-fA-F]+)");
    static const std::regex pfc_str(R"("([^"\\]|\\.)*")");
    auto make_probe_key = [](const std::string& s) -> std::string {
      std::string t = std::regex_replace(s, pfc_hex, "*");
      t = std::regex_replace(t, pfc_str, "\"*\"");
      auto pos = t.find("->");
      return (pos != std::string::npos) ? t.substr(pos) : t;
    };
    auto apply_probe_form = [&](const std::string& o, int form) -> std::string {
      switch (form) {
        case 2: return deref_args(o);
        case 3: return arrow_form(o);
        case 4: return deref_args(arrow_form(o));
        default: return o;
      }
    };

    auto pick_expr = [&](const std::string& orig) -> std::pair<std::string,std::string> {
      std::string key = make_probe_key(orig);

      // Cache hit: jump straight to the known-good form, skipping failing probes.
      auto cache_it = probe_form_cache.find(key);
      if (cache_it != probe_form_cache.end()) {
        std::string winning = apply_probe_form(orig, cache_it->second);
        std::string n = std::to_string(rut_tmp_n++);
        std::string al = "__rut_t" + n;
        gInterpreter->ProcessLine(("using " + al + " = decltype(" + winning + ");").c_str());
        return {winning, al};
      }

      if (try_direct) {
        // 1. direct, orig args — skipped for smart-pointer objects (operator-> present)
        auto [e1, al1] = probe(orig);
        if (!e1.empty()) { probe_form_cache[key] = 1; return {e1, al1}; }
        // 2. direct, deref args (handles pointer-where-reference-expected)
        std::string da = deref_args(orig);
        if (da != orig) {
          auto [e2, al2] = probe(da);
          if (!e2.empty()) { probe_form_cache[key] = 2; return {e2, al2}; }
        }
      }
      // 3. arrow (operator->), orig args (handles smart-pointer dispatch, e.g. RResultPtr)
      // For smart-pointer objects (!try_direct), probes 1&2 are skipped entirely:
      // the method lives on the pointee, so direct dispatch always fails.
      if (!try_direct) {
        std::string ar = arrow_form(orig);
        if (ar != orig) {
          auto [e3, al3] = probe(ar);
          if (!e3.empty()) { probe_form_cache[key] = 3; return {e3, al3}; }
          // 4. arrow + deref args
          std::string ar_da = deref_args(ar);
          if (ar_da != ar) {
            auto [e4, al4] = probe(ar_da);
            if (!e4.empty()) { probe_form_cache[key] = 4; return {e4, al4}; }
          }
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

  // ── cache hit path ──────────────────────────────────────────────────────
  if (!cache_key.empty()) {
    auto it = g_method_cache.find(cache_key);
    if (it != g_method_cache.end() && it->second.cached_cls == obj_cls) {
      CachedMethodCall& cached = it->second;
      const int nargs = (int)cache_args.size();

      if (pad_snapshot) { pad_snapshot->cd(); gROOT->SetSelectedPad(pad_snapshot); }

      // Helper: set parameters on the cached TMethodCall from the current
      // call's typed arg snapshots.  Used by kLong and as a fallback.
      auto set_params = [&]() {
        cached.mc->ResetParam();
        for (int i = 0; i < nargs; i++) {
          const CacheArg& ca = cache_args[i];
          ParamKind pk = (i < (int)cached.param_kinds.size())
                         ? cached.param_kinds[i] : ParamKind::kLong;
          double dval = (ca.type == LVAL_FLOAT) ? ca.flt : (double)ca.num;
          long   ival = (ca.type == LVAL_FLOAT) ? (long)ca.flt : ca.num;
          switch (pk) {
            case ParamKind::kFloat:   cached.mc->SetParam((Float_t)dval);   break;
            case ParamKind::kDouble:  cached.mc->SetParam((Double_t)dval);  break;
            case ParamKind::kLong64:  cached.mc->SetParam((Long64_t)ival);  break;
            case ParamKind::kULong64: cached.mc->SetParam((ULong64_t)(unsigned long)ival); break;
            default: {
              Long_t l = (ca.type == LVAL_TOBJ) ? (Long_t)(intptr_t)ca.obj : (Long_t)ival;
              cached.mc->SetParam(l); break;
            }
          }
        }
      };

      switch (cached.ret_type) {
        case TMethodCall::kLong: {
          // Use SetParam + Execute(Long_t&) rather than ExecWithArgsAndReturn.
          // ExecWithArgsAndReturn writes only sizeof(ReturnType) bytes into the
          // return buffer; for Int_t (32-bit) returns, the high 32 bits of the
          // Long_t buffer stay zero, giving wrong values for negative results
          // (e.g. a TGHSlider position of -25 becomes 4294967271 instead of -25).
          // Execute(Long_t&) always promotes to Long_t with proper sign extension.
          set_params();
          Long_t ret = 0;
          cached.mc->Execute(obj_ptr, ret);
          if (cached.is_ptr_return) return lval_tobj((void*)ret, cached.ptr_ret_cls);
          return lval_num((long)ret);
        }
        case TMethodCall::kDouble: {
          // ExecWithArgsAndReturn is safe for Double_t: always 8 bytes, no
          // sign-extension issue.  Pack args directly into typed storage.
          uint64_t   arg_vals[8] = {};
          const void* arg_ptrs[8] = {};
          for (int i = 0; i < nargs && i < 8; i++) {
            const CacheArg& ca = cache_args[i];
            ParamKind pk = (i < (int)cached.param_kinds.size())
                           ? cached.param_kinds[i] : ParamKind::kLong;
            double dval = (ca.type == LVAL_FLOAT) ? ca.flt : (double)ca.num;
            long   ival = (ca.type == LVAL_FLOAT) ? (long)ca.flt : ca.num;
            switch (pk) {
              case ParamKind::kFloat: {
                Float_t f = (Float_t)dval; memcpy(&arg_vals[i], &f, sizeof(f)); break;
              }
              case ParamKind::kDouble: {
                Double_t d = dval;         memcpy(&arg_vals[i], &d, sizeof(d)); break;
              }
              case ParamKind::kLong64: {
                Long64_t ll = (Long64_t)ival; memcpy(&arg_vals[i], &ll, sizeof(ll)); break;
              }
              case ParamKind::kULong64: {
                ULong64_t ull = (ULong64_t)(unsigned long)ival;
                memcpy(&arg_vals[i], &ull, sizeof(ull)); break;
              }
              default: {  // kLong — int, bool, pointer, enum
                Long_t l = (ca.type == LVAL_TOBJ) ? (Long_t)(intptr_t)ca.obj : (Long_t)ival;
                memcpy(&arg_vals[i], &l, sizeof(l)); break;
              }
            }
            arg_ptrs[i] = &arg_vals[i];
          }
          Double_t ret = 0;
          cached.mc->Execute(obj_ptr, arg_ptrs, nargs, &ret);
          return lval_floating(ret);
        }
        case TMethodCall::kString: {
          set_params();
          char* sret = nullptr;
          cached.mc->Execute(obj_ptr, &sret);
          return lval_str(sret ? sret : "");
        }
        default:  // kNone / kOther+void
          set_params();
          cached.mc->Execute(obj_ptr);
          return lval_qexpr();
      }
    }
  }
  // ── end cache hit path ───────────────────────────────────────────────────

  TMethodCall mc(obj_cls, method_name.c_str(), args.c_str());
  if (!mc.IsValid()) {
    // Method not found directly — try smart-pointer dereference via operator->().
    // First try TMethodCall (works for non-template classes).
    cache_key = "";  // smart-ptr and Cling paths are not cached
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

  // ── cache miss: execute, collect return metadata, then populate cache ────
  TMethodCall::EReturnType ret_type = mc.ReturnType();
  bool    is_ptr_return = false;
  TClass* ptr_ret_cls   = nullptr;
  // Only cache for the TMethodCall-handled return types.
  // Non-void kOther needs cling_new_auto_typed (not cacheable).
  // Smart-pointer classes (operator->) already cleared cache_key above.
  bool cache_this = !cache_key.empty();
  lval* result = nullptr;

  switch (ret_type) {
    case TMethodCall::kLong: {
      Long_t ret = 0;
      mc.Execute(obj_ptr, ret);
      TFunction* mf = mc.GetMethod();
      std::string retname = mf ? mf->GetReturnTypeName() : "";
      if (!retname.empty() && retname.back() == '*') {
        // Raw pointer return — strip '*' and look up class
        std::string bare = retname.substr(0, retname.size() - 1);
        while (!bare.empty() && bare.back() == ' ') bare.pop_back();
        ptr_ret_cls = TClass::GetClass(bare.c_str());
        // When the declared return type is TObject (the ROOT base class), it is
        // almost always a polymorphic container return (TList::At, TDirectory::Get,
        // etc.) whose concrete type is more specific.  Always recover the real
        // dynamic class so callers can invoke subclass methods (e.g. TKey::GetClassName).
        // Also recover when name lookup failed entirely.
        // Use TObject::IsA() (a virtual ROOT method) rather than C++ typeid, because
        // Cling's Calc context may not honour RTTI dynamic dispatch for typeid.
        bool need_dynamic = ret != 0 &&
          (!ptr_ret_cls || strcmp(ptr_ret_cls->GetName(), "TObject") == 0);
        if (need_dynamic) {
          // The result is pointer-instance-specific; don't cache.
          cache_this = false;
          TClass* dyn = reinterpret_cast<TObject*>(ret)->IsA();
          if (dyn) ptr_ret_cls = dyn;
        }
        is_ptr_return = true;
        result = lval_tobj((void*)ret, ptr_ret_cls);
        break;
      }
      result = lval_num((long)ret);
      break;
    }
    case TMethodCall::kDouble: {
      Double_t ret = 0;
      mc.Execute(obj_ptr, ret);
      result = lval_floating(ret);
      break;
    }
    case TMethodCall::kOther: {
      TFunction* mf = mc.GetMethod();
      std::string retname = mf ? mf->GetReturnTypeName() : "";
      if (retname == "void") {
        mc.Execute(obj_ptr);
        result = lval_qexpr();
        break;
      }
      // Non-void kOther — Cling typeid path; not cacheable.
      cache_this = false;
      std::string base = "((" + class_name + "*)" + ptr_to_hex(obj_ptr) + ")->"
                       + method_name + "(" + args + ")";
      return cling_new_auto_typed(base, [&]{ mc.Execute(obj_ptr); });
    }
    case TMethodCall::kString: {
      char* sret = nullptr;
      mc.Execute(obj_ptr, &sret);
      result = lval_str(sret ? sret : "");
      break;
    }
    default:  // kNone (void)
      mc.Execute(obj_ptr);
      result = lval_qexpr();
      break;
  }

  // Populate the cache so future calls with the same (class, method, arg-type-sig)
  // skip TMethodCall construction entirely.
  if (cache_this) {
    CachedMethodCall entry;
    entry.cached_cls    = obj_cls;
    entry.mc            = std::make_unique<TMethodCall>(obj_cls, method_name.c_str(), args.c_str());
    entry.ret_type      = ret_type;
    entry.is_ptr_return = is_ptr_return;
    entry.ptr_ret_cls   = ptr_ret_cls;
    // Derive the SetParam overload for each parameter from its C++ type name.
    // Use mc.GetMethod() (already resolved) instead of GetMethodAny, which
    // fails for methods inherited from base classes on some ROOT class hierarchies.
    TMethod* m2 = static_cast<TMethod*>(mc.GetMethod());
    if (m2) {
      TList* margs = m2->GetListOfMethodArgs();
      TIter  next(margs);
      while (TObject* o = next()) {
        TMethodArg* ma = static_cast<TMethodArg*>(o);
        entry.param_kinds.push_back(classify_param(ma->GetFullTypeName()));
      }
    }
    g_method_cache[cache_key] = std::move(entry);
  }

  return result;
}

// (:: Method ClassName args...)  — static method call
// Sugar: (::Method ClassName args...) desugars to the above in lval_eval_sexpr.
// Return type dispatch:
//   void    → empty Q-expression
//   pointer → TOBJ with dynamic TClass (via typeid)
//   float   → LVAL_FLOAT (bits smuggled through Long_t)
//   other   → LVAL_NUM (integral / enum)
lval* builtin_static(lenv* e, lval* a) {
  if (g_in_future) {
    lval* ac = lval_copy(a); lval* res = nullptr;
    rut_dispatch_work([&]{ res = builtin_static(e, ac); });
    lval_del(a); return res;
  }
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

// ---------------------------------------------------------------------------
// Main-thread Cling dispatch — future threads post work here
// ---------------------------------------------------------------------------

std::mutex                              g_cling_mu;
std::queue<std::unique_ptr<ClingWork>> g_cling_queue;
int g_cling_pipe[2] = {-1, -1};

void rut_drain_cling_queue() {
  while (true) {
    std::unique_ptr<ClingWork> work;
    {
      std::lock_guard<std::mutex> lock(g_cling_mu);
      if (g_cling_queue.empty()) break;
      work = std::move(g_cling_queue.front());
      g_cling_queue.pop();
    }
    work->fn();
    work->done.set_value();
  }
}

void rut_dispatch_work(std::function<void()> fn) {
  if (!g_in_future) { fn(); return; }
  auto work    = std::make_unique<ClingWork>();
  work->fn     = std::move(fn);
  auto fut     = work->done.get_future();
  {
    std::lock_guard<std::mutex> lock(g_cling_mu);
    g_cling_queue.push(std::move(work));
  }
  char c = 'C';
  ::write(g_cling_pipe[1], &c, 1);  // wake the event loop
  fut.get();                         // block until main thread executes the work
}

Long_t rut_calc(const char* expr, TInterpreter::EErrorCode* ec) {
  Long_t r = 0;
  rut_dispatch_work([&]{ r = gInterpreter->Calc(expr, ec); });
  return r;
}

Long_t rut_process_line(const char* code, TInterpreter::EErrorCode* ec) {
  Long_t r = 0;
  rut_dispatch_work([&]{ r = gInterpreter->ProcessLine(code, ec); });
  return r;
}

bool rut_declare(const char* code) {
  bool r = false;
  rut_dispatch_work([&]{ r = gInterpreter->Declare(code); });
  return r;
}

TFile* rut_open_file(const char* path) {
  TFile* f = nullptr;
  std::string p(path);
  rut_dispatch_work([&]{ f = TFile::Open(p.c_str(), "READ"); });
  return f;
}

// TFileHandler that drains the Cling dispatch queue when woken by a future thread.
class ClingPipeHandler : public TFileHandler {
public:
  explicit ClingPipeHandler(int fd) : TFileHandler(fd, 1) {}
  Bool_t ReadNotify() override {
    char buf[64];
    while (::read(GetFd(), buf, sizeof(buf)) > 0) {}  // drain notification bytes
    rut_drain_cling_queue();
    return kTRUE;
  }
  Bool_t Notify() override { return ReadNotify(); }
};

TFileHandler* rut_make_cling_handler(int fd) {
  return new ClingPipeHandler(fd);
}

// ---------------------------------------------------------------------------
// Thread pool
// ---------------------------------------------------------------------------

struct RutThreadPool {
  std::vector<std::thread>          workers;
  std::queue<std::function<void()>> tasks;
  std::mutex                        mu;
  std::condition_variable           cv;
  bool                              stop = false;

  explicit RutThreadPool(int n) {
    for (int i = 0; i < n; i++) {
      workers.emplace_back([this] {
        g_in_future = true;   // all pool threads are future-context threads
        while (true) {
          std::function<void()> task;
          {
            std::unique_lock<std::mutex> lock(mu);
            cv.wait(lock, [this]{ return stop || !tasks.empty(); });
            if (stop && tasks.empty()) return;
            task = std::move(tasks.front());
            tasks.pop();
          }
          task();
        }
      });
    }
  }

  void submit(std::function<void()> task) {
    { std::lock_guard<std::mutex> lock(mu); tasks.push(std::move(task)); }
    cv.notify_one();
  }

  ~RutThreadPool() {
    { std::lock_guard<std::mutex> lock(mu); stop = true; }
    cv.notify_all();
    for (auto& t : workers) t.join();
  }
};

static RutThreadPool* g_thread_pool = nullptr;

void rut_pool_create(int n_workers) {
  g_thread_pool = new RutThreadPool(n_workers);
}

void rut_pool_submit(std::function<void()> task) {
  if (g_thread_pool) {
    g_thread_pool->submit(std::move(task));
  } else {
    // Fallback: spawn a raw thread (pool not yet initialised).
    std::thread([t = std::move(task)]() mutable {
      g_in_future = true;
      t();
    }).detach();
  }
}

void rut_pool_destroy() {
  delete g_thread_pool;
  g_thread_pool = nullptr;
}

int rut_pool_size() {
  return g_thread_pool ? (int)g_thread_pool->workers.size() : 0;
}

void rut_pool_set_size(int n_workers) {
  if (n_workers < 1) n_workers = 1;
  delete g_thread_pool;                       // joins all workers (drains queue)
  g_thread_pool = new RutThreadPool(n_workers);
}

// ---------------------------------------------------------------------------
// Callback system
// ---------------------------------------------------------------------------

struct RutCallback { lval* fn; lenv* env; };
static std::map<int, RutCallback> g_callbacks;
static int  g_next_callback_id = 0;

// Pipe used to defer callback execution out of Cling's slot-dispatch context.
// Calling Cling (via new/method-call builtins) from within a Cling-dispatched
// slot causes re-entrancy that silently corrupts results.  Instead we write the
// callback id to the pipe; a TFileHandler drains it in the next event-loop
// iteration, when Cling is idle.
int g_cb_pipe[2] = {-1, -1};

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

TFileHandler* rut_make_callback_handler(int fd, lenv* env) {
  return new RutCallbackHandler(fd, env);
}

// ---------------------------------------------------------------------------
// builtin_new, builtin_invoke
// ---------------------------------------------------------------------------

// Creates a new C++ object
lval *builtin_new(lenv *e, lval* a) {
  if (g_in_future) {
    lval* ac = lval_copy(a); lval* res = nullptr;
    rut_dispatch_work([&]{ res = builtin_new(e, ac); });
    lval_del(a); return res;
  }
  LASSERT(a, a->count >= 1,
    "Function 'new' needs at least 1 argument: <class name>.");
  LASSERT_TYPE("new", a, 0, LVAL_STR);
  std::string className = a->cell[0]->str;  // copy before lval_del
  TClass *cls = TClass::GetClass(className.c_str());
  if (!cls) {
    lval_del(a);
    return lval_err("Unknown class '%s'", className.c_str());
  }
  {
    std::string simple = className;
    auto dpos = simple.rfind("::");
    if (dpos != std::string::npos) simple = simple.substr(dpos + 2);
    auto deref_mask_c = build_deref_mask(cls, simple.c_str(), a, 1);
    if (!deref_mask_c.empty()) {
      // Reflection says which params are references — build args with correct form
      // directly, skipping the try/retry probe loop below.
      std::string rargs = lval_to_cpp_arg(e, a, 1, &deref_mask_c);
      lval_del(a);
      std::string expr = "new " + className + "(" + rargs + ");";
      TInterpreter::EErrorCode ec = TInterpreter::kNoError;
      void* obj = (void*)gInterpreter->Calc(expr.c_str(), &ec);
      if (!obj || ec != TInterpreter::kNoError)
        return lval_err("Constructor failed for '%s'", className.c_str());
      TClass* real_cls = (TClass*)gInterpreter->Calc(
          ("(Long_t)TClass::GetClass(typeid(*((" + className + "*)" +
           ptr_to_hex(obj) + ")))").c_str());
      return lval_tobj(obj, real_cls ? real_cls : cls);
    }
  }
  std::string args = lval_to_cpp_arg(e, a, 1);
  lval_del(a);

  // Try direct args first; if that fails (e.g. pointer where reference
  // expected), retry with TOBJ pointer args dereferenced: ((T*)p) → (*((T*)p))
  // Cache the working form per (className, arg-type-pattern) to avoid
  // re-probing on every call (e.g. RDataFrame(TChain&) vs RDataFrame(TChain*)).
  static const std::regex ctor_arg_re(R"(\(\([^)*]+\*\)(0x[0-9a-fA-F]+)\)(?!->))");
  static const std::regex hex_re(R"(0x[0-9a-fA-F]+)");
  static std::unordered_map<std::string,bool> ctor_deref_cache;
  auto try_ctor = [&](const std::string& ctorArgs) -> void* {
    std::string expr = "new " + className + "(" + ctorArgs + ");";
    TInterpreter::EErrorCode ec = TInterpreter::kNoError;
    void* result = (void*)gInterpreter->Calc(expr.c_str(), &ec);
    return (ec == TInterpreter::kNoError) ? result : nullptr;
  };
  std::string cache_key = className + ":" + std::regex_replace(args, hex_re, "*");
  void *obj = nullptr;
  {
    auto it = ctor_deref_cache.find(cache_key);
    if (it != ctor_deref_cache.end()) {
      std::string effective = it->second
          ? std::regex_replace(args, ctor_arg_re, "(*$&)") : args;
      obj = try_ctor(effective);
    } else {
      obj = try_ctor(args);
      if (!obj) {
        std::string derefed = std::regex_replace(args, ctor_arg_re, "(*$&)");
        if (derefed != args) {
          obj = try_ctor(derefed);
          ctor_deref_cache[cache_key] = (bool)obj;
          if (!obj) obj = nullptr;
        }
      } else {
        ctor_deref_cache[cache_key] = false;
      }
    }
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
  if (g_in_future) {
    lval* ac = lval_copy(a); lval* res = nullptr;
    rut_dispatch_work([&]{ res = builtin_invoke(e, ac); });
    lval_del(a); return res;
  }
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

// ---------------------------------------------------------------------------
// Inspection / output builtins
// ---------------------------------------------------------------------------

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
  if (g_in_future) {
    lval* ac = lval_copy(a); lval* res = nullptr;
    rut_dispatch_work([&]{ res = builtin_save_window(e, ac); });
    lval_del(a); return res;
  }
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
  if (g_in_future) {
    lval* ac = lval_copy(a); lval* res = nullptr;
    rut_dispatch_work([&]{ res = builtin_global(e, ac); });
    lval_del(a); return res;
  }
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
  if (g_in_future) {
    lval* ac = lval_copy(a); lval* res = nullptr;
    rut_dispatch_work([&]{ res = builtin_connect(e, ac); });
    lval_del(a); return res;
  }
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

// ---------------------------------------------------------------------------
// lenv_add_builtins_root
// ---------------------------------------------------------------------------

void lenv_add_builtins_root(lenv* e) {
  /* ROOT/Cling object interaction */
  lenv_add_builtin(e, ".",        builtin_member);
  lenv_add_builtin(e, "member",   builtin_member);
  lenv_add_builtin(e, "::",       builtin_static);
  lenv_add_builtin(e, "new",      builtin_new);
  lenv_add_builtin(e, "invoke",   builtin_invoke);
  lenv_add_builtin(e, "global",   builtin_global);
  lenv_add_builtin(e, "connect",  builtin_connect);
  /* Inspection / output */
  lenv_add_builtin(e, "symbols",     builtin_symbols);
  lenv_add_builtin(e, "canvases",    builtin_canvases);
  lenv_add_builtin(e, "save-png",    builtin_save_png);
  lenv_add_builtin(e, "save-window", builtin_save_window);
  /* Global ROOT objects */
  lenv_add_global_object(e, "gSystem",      gSystem,      TClass::GetClass("TSystem"));
  lenv_add_global_object(e, "gInterpreter", gInterpreter, TClass::GetClass("TInterpreter"));
  lenv_add_global_object(e, "gROOT",        gROOT,        TClass::GetClass("TROOT"));
  lenv_add_global_object(e, "gFile",        gFile,        TClass::GetClass("TFile"));
  lenv_add_global_object(e, "gPad",         gPad,         TClass::GetClass("TVirtualPad"));
  lenv_add_global_object(e, "gDirectory",   gDirectory,   TClass::GetClass("TDirectory"));
  lenv_add_global_object(e, "gRandom",      gRandom,      TClass::GetClass("TRandom"));
  lenv_add_global_object(e, "gStyle",       gStyle,       TClass::GetClass("TStyle"));
  /* Register the callable bridge by address so Cling's JIT can call it
     without relying on symbol export (-rdynamic/-export_dynamic). */
  gInterpreter->Declare(
    "typedef double (*__rooture_invoke_t)(const char*, const double*, int);\n"
    "__rooture_invoke_t __rooture_invoke_ptr;");
  gInterpreter->ProcessLine(
    ("__rooture_invoke_ptr = (__rooture_invoke_t)" +
     ptr_to_hex((void*)rooture_invoke_callable_c) + ";").c_str());
}
