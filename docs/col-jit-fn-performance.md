# col-jit-fn performance: reaching parity with hand-coded C++

This page documents the steps taken to close the performance gap between a
`col-jit-fn` kernel written in rooture and an equivalent hand-coded C++ builtin
(`col-dca-v0`), eventually achieving ~1.04× (essentially identical) throughput.

---

## Benchmark setup

`examples/bench_dca_v0.rut` measures two implementations of the V0 helix-DCA
geometry over all 31 time frames in an AO2D file, averaged over 20 passes after
a warmup run:

- **`col-dca-v0`**: hand-coded C++ builtin compiled into rooture at O2.
- **`dca-v0-jit-fn`**: equivalent logic expressed as a `col-jit-fn` in rooture.

Starting point before any optimisation:

```
col-dca-v0 (C++ builtin):    31 ms/pass
dca-v0-jit-fn (col-jit-fn):  51 ms/pass
Ratio: 1.6× (jit-fn 60% slower)
```

---

## Step 1: fix constant-folding of local variables (`collect_locals`)

The first correctness bug: `(= {crp0} ...)` inside a `col-jit-fn` body could be
constant-folded from the outer environment instead of emitted as a local variable
declaration, producing wrong results or compile errors.

**Fix**: pre-scan the lambda body with `collect_locals` before transpilation.
This traverses all `(= {name} ...)` forms and registers them in `ctx.params` so
the transpiler treats them as local C++ variables rather than env lookups.

```cpp
// in builtin_col_jit_fn (rut_jitfn.cxx):
collect_locals(body_node, ctx);
std::string loop_body = rut_to_cpp_stmt(body_node, "    ", true, ctx);
```

After this fix the `dca-v0-jit-fn` kernel produced correct physics results
(matching the C++ builtin).

---

## Step 2: replace `TMath::*` with native C float math

The original rooture kernel used `(::Cos TMath x)`, `(::Sqrt TMath x)`, etc.
These map to `TMath::Cos(x)` — opaque `extern` calls that the LLVM vectoriser
cannot see through.  With them, the loop cannot be auto-vectorised.

**Replacement**:

| Before | After |
|---|---|
| `(::Abs TMath x)` | `(fabsf x)` |
| `(::Cos TMath x)` | `(cosf x)` |
| `(::Sin TMath x)` | `(sinf x)` |
| `(::Sqrt TMath x)` | `(sqrtf x)` |
| `(::ATan2 TMath y x)` | `(atan2f y x)` |
| `(::ASin TMath x)` | `(asinf x)` |

These map to LLVM builtins (`__builtin_cosf`, etc.) which the vectoriser
recognises and can lower to NEON/SVE intrinsics on Apple Silicon.

**To support this**, the `col-jit-fn` transpiler was extended with a general
direct-call fallthrough in `rut_to_cpp_expr`:

```cpp
// General direct C/C++ function call: (fn arg1 arg2 ...) → fn(arg1, arg2, ...)
{
  std::string call = to_cpp_id(s) + "(";
  for (int i = 1; i < v->count; i++) {
    if (i > 1) call += ", ";
    call += rut_to_cpp_expr(v->cell[i], ctx);
  }
  call += ")";
  return call;
}
```

After this change: **ratio improved to 1.11×**.

---

## Step 3: emit `float` declarations for col-jit-fn locals

After step 2, the generated kernel still had `auto` for all local variables:

```cpp
auto crp0 = (sp[i] * 0.5 * 0.003);   // sp[i] is float, 0.5 and 0.003 are double
auto crp  = ((fabsf(crp0) < 1e-9) ? ...);
```

Because the constants `0.5`, `0.003`, `1e-9` etc. are C `double` literals, type
inference promotes `crp0` to `double`.  Then `fabsf(double)` triggers a Cling
warning **and** the kernel computes in double precision — the expensive scalar
double-precision `cos`/`sin`/`sqrt` path rather than the vectorisable float path.

**Fix**: in `rut_to_cpp_stmt`, emit `float` instead of `auto` for local variable
declarations when inside a `col-jit-fn` context (`ctx.n_outputs > 0`):

```cpp
const char* decl_type = (ctx.n_outputs > 0) ? "float " : "auto ";
return ind + decl_type + varname + " = " + rut_to_cpp_expr(v->cell[2], ctx) + ";\n";
```

With `float crp0 = ...` the RHS is computed as double and then narrowed to float
on store — keeping all intermediate registers in float, enabling vectorisation.
Warnings disappeared and throughput jumped significantly.

After this change: **ratio improved to 1.03–1.05× (essentially parity)**.

---

## Step 4: float32 literal syntax (`1.0f`, `0.003f`)

To eliminate the implicit double→float narrowing entirely, rooture was extended
with native float32 literal syntax.  Writing `0.003f` makes the constant a
single-precision value throughout, avoiding the narrowing:

**Parser** (`rut_repl.cxx` `mpca_lang`): new `float32` grammar rule, tried
before `floating`:

```
float32 : /-?[0-9]+[.][0-9]*[eE][+-]?[0-9]+f/
        | /-?[0-9]+[.][0-9]*f/
        | /-?[.][0-9]+f/
        | /-?[0-9]+[eE][+-]?[0-9]+f/
        | /-?[0-9]+f/ ;
```

**Runtime** (`rut_lang.cxx`): new `LVAL_FLOAT32` type.  Arithmetic normalises
it to `LVAL_FLOAT` via `best_numeric_type` (float32 is a source-level hint, not
a runtime precision change).

**Transpiler** (`rut_jitfn.cxx`): `LVAL_FLOAT32` emits with `f` suffix:

```cpp
if (v->type == LVAL_FLOAT32) {
  char buf[32]; snprintf(buf, sizeof(buf), "%.9gf", (float)v->floating);
  return buf;
}
```

Now kernel constants can be written as `1e-9f`, `0.003f`, `6.2831853f`, etc.,
and the generated C++ contains no double literals at all.

> **Note**: `grammar.js` and `src/parser.c` are the tree-sitter grammar used
> for **syntax highlighting only**.  All interpreter grammar changes go in the
> `mpca_lang()` call in `rut_repl.cxx`.

---

## Final result

```
col-dca-v0 (C++ builtin):    ~32 ms/pass
dca-v0-jit-fn (col-jit-fn):  ~33 ms/pass
Ratio: ~1.04×  (no warnings)
```

A `col-jit-fn` kernel written in rooture with native float math functions and
float32 literal constants compiles — via Cling at O2 — to code that runs at the
same throughput as hand-coded C++.  The rooture programmer does not need to write
a single line of C++.
