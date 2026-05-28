---
description: Rooture language reference and workflow. Use when writing, running, or debugging rooture (.rut) code, or using the rooture MCP tools (eval, load, list_canvases, list_symbols, get_canvas).
---

# rooture-lang

Rooture is a Lisp-like language for interactive ROOT data analysis. Expressions are evaluated via the `mcp__rooture__eval` tool.

---

## Syntax reference

```scheme
(def {name} value)                          ; global binding
(= {name} value)                            ; local binding (in lambdas/do)
(\{arg1 arg2} {body})                       ; lambda
(new ClassName arg1 arg2)                   ; construct ROOT object
(.MethodName object arg1 arg2)              ; method call
(:: Method ClassName args...)               ; static method
(::Method ClassName args...)                ; same — sugar like (.Method obj)
(doto obj {Method1 a} {Method2 b})         ; multiple methods on same object
(-> expr {. Method1 a} {. Method2 b})      ; threading macro
(load "stdlib.rut")                         ; load file (quotes optional)
```

### doto with sub-object chaining

```scheme
(doto h
  {>> {GetXaxis} {SetTitle "x [GeV]"}}   ; h.GetXaxis() → SetTitle on axis
  {Draw})
```

### Lambda bodies need `do` for multiple expressions

`{expr1 expr2}` evaluates as `(expr1 expr2)` — calls expr1 as a function on expr2.

```scheme
(\{i} {do expr1 expr2})   ; correct: evaluates both, returns last
(\{i} {expr1})            ; fine for single expression
```

Use `=` (not `def`) for locals inside lambdas. `def` writes to global scope.

### `if` branches must be Q-expressions

```scheme
(if cond {true-val} {false-val})   ; correct
(if cond  true-val   false-val)    ; ERROR
```

### `cond` for multiple cases

```scheme
(cond
  {(< x 0)} {-1}
  {(> x 0)} {1}
  {else}     {0})
```

`cond` is a language builtin. It does **not** work inside `jit-fn` or `col-jit-fn` — use nested `(if ...)` there.

### Undefined symbols auto-convert to strings

Any unbound symbol becomes a string with the same spelling. `(load stdlib.rut)` works without quotes. Silent failures show as `"Got String, expected Object"` — trace back to whether the object's `def` ran.

---

## Data member access

```scheme
(field mEventCount obj)          ; access obj->mEventCount (pointer, int, float)
(.@ mEventCount obj)             ; alias for field

(field-at mValues obj 6)         ; access obj->mValues[6] (indexed pointer array)
                                 ; returns {} (empty qexpr) for null pointers
```

`field` / `.@` reads a public data member directly — no parentheses, no method call.
Handles pointer members (returns TOBJ), integral (returns int), and floating-point
(returns float) types. Use for classes without getter methods (e.g. MakeProject-generated
stubs from streamer info).

`field-at` adds array indexing for C-style pointer-array members like `TArray** mValues`.

## Zero-copy column from TArray

```scheme
(def {col} (as-col tarray-obj))  ; wrap TArrayF/D/I/L64 as a column
```

`as-col` creates a zero-copy rooture column from a ROOT `TArray` object.
The column borrows the underlying buffer (no memcpy) and keeps the source
alive via a shared reference. Supported types:

| TArray type | Column dtype |
|-------------|-------------|
| TArrayF     | float       |
| TArrayD     | double      |
| TArrayI     | int32       |
| TArrayL64   | int64       |

Note: `TArray` does not inherit from `TObject`, so `as-col` uses the TClass
name (not `dynamic_cast`) to determine the type.

### Example: compare StepTHn data between two files

```scheme
(def {ph1} (field mPairHist container1))
(def {v1}  (field-at mValues ph1 6))     ; step 6 TArrayF
(def {c1}  (as-col v1))                  ; zero-copy column, 5.9M floats
(def {c2}  (as-col (field-at mValues ph2 6)))
(def {diffs} (col-zip-ptr diff-ptr c1 c2))
(def {total} (col-reduce-ptr sum-ptr 0.0 diffs))
```

## Static method calls

```scheme
(::Create TEveManager)            ; TEveManager::Create()
(::GetColor TColor 15 76 129)     ; TColor::GetColor(r,g,b)
(::Sin TMath 1.5707)              ; TMath::Sin(x)
```

Return types: void → `()`, pointer → TOBJ, float → float, int → integer.

## `@name` syntax

`@foo` resolves `gROOT->FindObject("foo")` — retrieves any named ROOT object (canvas, histogram, fit function).

---

## MCP workflow

1. Load MCP tool schemas: `ToolSearch "select:mcp__rooture__eval,mcp__rooture__load,mcp__rooture__list_canvases,mcp__rooture__list_symbols"`
2. **Use `mcp__rooture__load` to load scripts** — it calls `(load "path")` and returns the provide manifest (list of exported symbols). Prefer this over `eval` + `(load ...)` for discoverability.
3. After loading, call `(run)` via `eval` to execute the analysis (all library scripts wrap analysis code in a `run` function).
4. Create a canvas before drawing: `(def {c} (new TCanvas ...))`
5. Eval expressions one at a time or compose multi-line strings.
6. Use `mcp__rooture__list_symbols` to inspect the environment.
7. Use `mcp__rooture__get_canvas` to view plots (returns PNG inline).

### `provide` system

Library scripts declare their public API with `(provide {sym1 sym2 ...})` at the end.
`load` returns this manifest so you know what is available without reading source.

```scheme
;;; Loading returns the manifest:
;;; Exports: {run tf-pairs h-pt h-p h-pt-species bb0 bb1 bb2 bb3 bb4 ...}
(load "examples/spectra_tpc.rut")

;;; Then run the analysis:
(run)
```

For long-running loads (scripts that scan timeframes), use `eval_async`:
```scheme
eval_async expr="(load \"examples/spectra_tpc.rut\")"
```

## Custom colors

```scheme
(def {colBlue} (new TColor 5001 0.059 0.298 0.506 "PantoneClassicBlue"))
```

Use indices ≥ 6000 to avoid clashing with ROOT's built-in palette.

---

## Common pitfalls

- `doto` methods: bare symbols without dot — `{FillRandom gaus 1000}` not `{.FillRandom ...}`
- Float literals need a decimal point: `10.` not `10`
- `Clone` returns `TObject*` — use `@cloneName` to retrieve with real class
- Enum constants (`kLHintsCenterX` etc.) are not in scope — use numeric values
