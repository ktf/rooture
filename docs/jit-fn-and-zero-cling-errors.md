# jit-fn transpiler and zero-error AO2D track GUI

This page documents the design and implementation of the `jit-fn` builtin,
the native-rooture rewrite of `examples/tracks_3d_gui.rut`, and the
`build_deref_mask` mechanism that eliminates all Cling error output on load.

---

## Motivation

`examples/tracks_3d_gui.rut` is a 3-pad interactive ALICE AO2D track explorer:
- pad 1 — COLZ density η × σ(pT)/pT
- pad 2 — COLZ density η × log₂(σ(pT)/pT)
- pad 3 — true 3D helices in global X/Y/Z, drawn with `RDataFrame::Foreach`

The original version used `ProcessLine` for almost everything, and called the
helix-drawing lambda through the interpreter callable bridge — 200 round-trips
per slider update.  Two goals:

1. **Speed**: replace the interpreter bridge with a JIT-compiled native C++ function.
2. **Cleanliness**: eliminate all Cling error output so the terminal stays quiet.

---

## Scientific notation parsing fix

First blocker: `2.99792458e-4` tokenised as `INT(2) · FLOAT(.99792458) · SYM(e-4)`.

The mpc grammar `floating` rule in `mpca_lang()` (rooture.cxx) only handled
`/-?[0-9]+[.][0-9]*/`.  Fixed by adding five explicit alternatives in
longest-match-first PEG order:

```
floating : /-?[0-9]+[.][0-9]*[eE][+-]?[0-9]+/
         | /-?[0-9]+[.][0-9]*/
         | /-?[.][0-9]+[eE][+-]?[0-9]+/
         | /-?[.][0-9]+/
         | /-?[0-9]+[eE][+-]?[0-9]+/ ;
```

`grammar.js` is for tree-sitter syntax highlighting **only** — it has no effect
on the interpreter.

---

## jit-fn builtin

`(jit-fn [rettype] lambda)` transpiles a restricted rooture lambda to C++ and
declares it via `gInterpreter->Declare`.  Returns a new lval type `LVAL_JITFN`
whose C++ name (`__rut_jit_0`, …) is emitted verbatim into subsequent Cling
expressions.

### Why not `emit_jit_wrapper`?

The existing `emit_jit_wrapper` generates a C++20 abbreviated function template:

```cpp
double __rut_fn_0_wrapper(auto _a0, auto _a1, ...) { ... }
```

`RDataFrame::Foreach` deduces `F = decltype(__rut_fn_0_wrapper)` — but
`decltype` of a function template is ill-formed, so Cling refuses it.
`jit-fn` emits a **concrete** function with a fixed signature:

```cpp
void __rut_jit_0(float snp, float tgl, float alpha,
                 float refx, float refy, float refz, float s1pt) { ... }
```

RDF deduces `F = void(*)(float,float,float,float,float,float,float)` without
ambiguity.

### Typed formals

Lambda formals can carry an explicit C++ type:

```scheme
(\{{float snp} {float tgl} ...} body)
```

`{float snp}` is a Q-expression whose head is the type symbol and whose tail is
the parameter name.  `builtin_lambda` accepts these; `lval_call` unwraps them
before the `&` variadic check.

### Transpiler coverage

| Construct | Output |
|---|---|
| `LVAL_NUM` / `LVAL_FLOAT` / `LVAL_STR` / `LVAL_SYM` | literal |
| `LVAL_FLOAT32` (e.g. `1.5f`) | `f`-suffixed float literal |
| `(do e1 e2 …)` | sequential statements |
| `(= {x} rhs)` | `float x = rhs;` (col-jit-fn) or `auto x = rhs;` (jit-fn) |
| `(dotimes {i} n {body})` | `for(int i=0; i<n; ++i)` |
| `(if …)` pure branches | ternary |
| `(if …)` statement branches | `if/else` |
| `(+ - * /)` | binary operators, left-folded for N args |
| `(< > <= >= == !=)` | comparison operators |
| `(::Method Class args)` | `Class::Method(args)` |
| `(.Method obj args)` | `obj->Method(args)` |
| `(new ClassName args)` | `new ClassName(args)` |

---

## tracks_3d_gui.rut — native rooture rewrite

All `ProcessLine` calls eliminated:

### update-scatter (pad 2)

Original: `TChain::Draw("...", cut, "goff")` → raw `Double_t*` from `GetV1/V2/V3`
→ `new TGraph2D(n, ptr, ptr, ptr)`.  Inexpressible in rooture (no raw pointer
indexing).

**Fix**: pre-build `h3log` (η × log₂(σpT/pT) × nCls) via RDataFrame at load time.
`update-scatter` becomes identical to `update-density`: `SetRange` + `Project3D("yx")`
+ `Draw "COLZ"`.

### update-helices (pad 3)

Original: 35-line duplicated C++ lambda inside a ProcessLine string, called via
`TChain::Draw + GetV1/V2/V3`.

**Fix**: define `helix-fn` as a top-level `jit-fn void` at load time, then:

```scheme
(-> (new ROOT::RDataFrame trk-chain)
  {.Filter cut-str}
  {.Range 200}
  {.Foreach helix-fn helix-cols})
```

`helix-fn` runs at native C++ speed — zero interpreter overhead per track.

### draw-cylinder

Beam pipe and TPC inner wall wireframes moved from ProcessLine to native rooture
and then — after profiling revealed they dominated render time — converted to a
`jit-fn` (see [Performance profiling](#performance-profiling) below).

---

## Performance profiling and jit-fn direct call

### Instrumentation

`TStopwatch` timing was added inside `update-helices` to measure three phases:

```scheme
(.Start sw)
(-> rdf-helix {.Range 500} {.Filter filter-fn cols} {.Range 200} {.Foreach helix-fn helix-cols})
(.Stop sw) (= {t-rdf} (.RealTime sw))

(.Start sw)
(draw-cylinder 2. -250. 250. 5 12 400 2)
(draw-cylinder 83. -250. 250. 3  8 921 1)
(.Stop sw) (= {t-cyl} (.RealTime sw))
```

Typical output with `draw-cylinder` as a rooture lambda:

```
helices: rdf=0.84s  cyl=5.06s  update=0.04s
```

The rooture interpreter dispatches ~584 Cling method calls per cylinder-pair update
(2 cylinders × (nCirc × (nc+1 + 3 method calls) + nSeg × 4 calls)). Each call
goes through `TMethodCall::Execute`, producing the 5 s overhead.

### draw-cylinder as jit-fn

Converting `draw-cylinder` to a `jit-fn` required two engine changes:

**1. Hyphen-to-underscore sanitisation (`to_cpp_id`)**

rooture symbol names may contain `-` (e.g. `two-pi`), which is not a valid C++
identifier character.  A helper `to_cpp_id` was added to `rut_jitfn.cxx`:

```cpp
static std::string to_cpp_id(const char* s) {
  std::string r(s);
  for (char& c : r) if (c == '-') c = '_';
  return r;
}
```

Applied to: LVAL_SYM emission, `=` variable names, `dotimes` loop variable names,
and formal parameter names inserted into `ctx.params`.

**2. LVAL_JITFN direct call in `lval_call`**

Calling `(draw-cylinder 2. ...)` from rooture must invoke the JIT-compiled function.
Added a handler at the top of `lval_call` in `rut_lang.cxx`:

```cpp
if (f->type == LVAL_JITFN) {
  std::string call = std::string(f->sym) + "(";
  for (int i = 0; i < a->count; i++) {
    if (i) call += ", ";
    // serialise NUM/FLOAT/STR/TOBJ arguments to C++ literals
    ...
  }
  call += ")";
  gInterpreter->ProcessLine(call.c_str());
  lval_del(a);
  return lval_sexpr();
}
```

`lval_eval_sexpr` was also updated to allow `LVAL_JITFN` through the type guard
(previously only `LVAL_FUN` was accepted).

**3. `lval_jitfn` / `lval_copy` initialise `builtin` to `nullptr`**

`lval_jitfn` uses `malloc`; without explicitly setting `builtin = nullptr`, copied
JITFN lvals had garbage in the `builtin` field and would mis-fire the builtin check.

### Result after jit-fn conversion

```
helices: rdf=0.85s  cyl=0.008s  update=0.04s
```

650× speedup on cylinder drawing.  The `TStopwatch` instrumentation was removed
once the bottleneck was resolved.

---

## Mutable threshold via LVAL_TOBJ constant-folding

### Problem

Naively, updating the cluster-count filter requires recompiling a new jit-fn on
every slider move (1–2 s of `gInterpreter->Declare` overhead per update).

### Solution

A `TArrayI` of length 1 is allocated on the heap and its address folded into the
filter jit-fn at compile time:

```scheme
(def {min-cls-box} (new TArrayI 1))
(.SetAt min-cls-box 0 0)

(def {filter-fn}
  (jit-fn bool
    (\{{UChar_t fTPCNClsFindable} {Char_t fTPCNClsFindableMinusFound}}
      {(>= (- fTPCNClsFindable fTPCNClsFindableMinusFound) (.GetAt min-cls-box 0))})))
```

The transpiler sees `min-cls-box` as an `LVAL_TOBJ` in the calling environment and
folds it to a C++ pointer cast:

```cpp
bool __rut_jit_0(UChar_t fTPCNClsFindable, Char_t fTPCNClsFindableMinusFound) {
  return ((fTPCNClsFindable - fTPCNClsFindableMinusFound) >=
          ((TArrayI*)0x1234abcd)->GetAt(0));
}
```

On each slider release, rooture calls `(.SetAt min-cls-box minCls 0)` — no
recompilation, the filter reads the updated value at runtime.

### Constant-folding implementation (`JitCtx`)

The transpiler carries a context struct:

```cpp
struct JitCtx {
  std::set<std::string> params;  // formal parameter names — emitted as-is
  lenv* env;                     // calling env — free variables folded from here
};
```

Key: `ctx.env = e` (the calling environment), **not** `fn->env`.  Lambda creation
via `lval_lambda` always sets `fn->env = lenv_new()` (empty); the actual bindings
(like `min-cls-box`) only exist in the calling scope `e`.

TOBJ folding in `rut_to_cpp_expr`:

```cpp
} else if (found->type == LVAL_TOBJ && found->obj) {
  std::string cls_name = found->cls ? found->cls->GetName() : "void";
  char addr_buf[32]; snprintf(addr_buf, sizeof(addr_buf), "%p", found->obj);
  r = "((" + cls_name + "*)" + std::string(addr_buf) + ")";
}
```

---

## Slider and proportional helix display

- **Label update** wired to `PositionChanged(Int_t)` (fires on every drag position).
- **Redraw** wired to `Released()` (fires only on mouse release).

This prevents expensive redraws while dragging.

To keep the number of drawn helices proportional to the cluster cut (strict cuts
should show fewer tracks), the RDF pipeline is:

```scheme
(-> rdf-helix
  {.Range 500}      ; limit raw entries read from chain
  {.Filter filter-fn cols}
  {.Range 200}      ; cap drawn helices
  {.Foreach helix-fn helix-cols})
```

At a 0.44% pass rate (minCls = 150), `.Range 500` yields ~2 helices drawn —
visually reflecting the strict cut.

---

## AO2D branch types

The `O2trackextra_002` branches `fTPCNClsFindable` and `fTPCNClsFindableMinusFound`
are `UChar_t` / `Char_t` (ROOT's fixed-size 8-bit integer types), **not** `uint8_t` /
`int8_t`.  Formals in the filter jit-fn must use the ROOT type names:

```scheme
(\{{UChar_t fTPCNClsFindable} {Char_t fTPCNClsFindableMinusFound}} ...)
```

Using `uint8_t` / `int8_t` causes RDF `CallableTraits` to fail to resolve the
argument types.

---

## build_deref_mask — eliminating Cling error output

### The problem

rooture emits TOBJ arguments as `((Cls*)ptr)` — a pointer.  Many ROOT methods
take `T&` (reference).  Every first call to such a method printed a full Clang
error wall before the probe mechanism fell back to the deref form `*((Cls*)ptr)`.

Affected at load time:
- `new ROOT::RDataFrame(trk-chain)` — constructor takes `TTree&`
- `RInterface::Histo3D(model, …)` — first arg is `const TH3DModel&`

### Solution: build_deref_mask

`build_deref_mask(TClass* cls, const char* method, lval* a, int offset)` returns
a per-argument boolean mask: `mask[i] = true` means dereference that argument.
Called before `lval_to_cpp_arg`, it produces the correct pointer/deref form
upfront so probe-1 succeeds without ever emitting an error.

Two strategies:

**S1 — GetListOfMethods() iteration**

```cpp
TIter next(cls->GetListOfMethods());
TObject* obj;
while ((obj = next())) {
  TMethod* m = (TMethod*)obj;   // NOT dynamic_cast — RTTI broken for ROOT on macOS
  ...
  TList* al = m->GetListOfMethodArgs();
  if (!al || al->GetSize() == 0) continue;  // skip template methods (args not reflected)
  // check ft.find('&') per arg
}
```

Key lessons:
- `dynamic_cast<TMethod*>` always returns `nullptr` for ROOT types on macOS —
  C-style cast required.
- Template methods on template classes (e.g. `RInterface<...>::Histo3D`) have
  `GetNargs() > 0` but `GetListOfMethodArgs()` returns an empty list.  Must skip
  and fall through to S2.

**S2 — GetMethodWithPrototype per-TOBJ-arg probe**

For each TOBJ argument, check whether the class has a method accepting the arg
type as a pointer.  If not, try the reference form:

```cpp
std::string ptr_proto  = cname + "*";
bool ptr_ok = (cls->GetMethodWithPrototype(method_name, ptr_proto.c_str()) != nullptr);
if (!ptr_ok) {
  bool ref_ok = cls->GetMethodWithPrototype(method_name, (cname+"&").c_str()) ||
                cls->GetMethodWithPrototype(method_name, ("const "+cname+"&").c_str());
  if (ref_ok) mask[idx] = true;
}
```

`GetMethodWithPrototype("Histo3D", "ROOT::RDF::TH3DModel const&")` finds the
single-arg overload, confirming that the first argument of all Histo3D calls
needs dereferencing.

### Integration

- **`builtin_member`**: calls `build_deref_mask` before `lval_to_cpp_arg`, passes
  mask so the initial Cling probe already has the right form.
- **`builtin_new`**: when a non-empty mask is found, takes an early-return path
  that calls `gInterpreter->Calc` directly with deref'd args — completely bypasses
  the try/retry loop (and its first-try error output).

### Result

```
rooture> (load "examples/tracks_3d_gui.rut")
()
```

Zero Cling errors from a fresh rooture process.

---

## Probe caching and smart-pointer bypass

Also implemented in the same session:

- **`probe_form_cache`**: maps a normalised `->method(args)` key (hex addresses
  and string literals wildcarded) to the winning probe form (1–4).  After the
  first successful call, subsequent calls jump straight to the known-good form.

- **Smart-pointer bypass**: when `obj_cls->GetMethodAny("operator->") != nullptr`,
  skip direct probes 1 & 2 and start from arrow form.  Required for
  `RResultPtr<TH3D>` returned by `Histo3D` — `GetZaxis`, `Project3D`, `SetRange`
  all go via `operator->` to the underlying `TH3D`.

- **`ctor_deref_cache`**: maps `className:arg_type_pattern` → bool for constructors
  not covered by `build_deref_mask` (e.g. classes without ROOT dictionary).
