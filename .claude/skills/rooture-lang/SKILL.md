---
description: Rooture language reference and workflow. Use when writing, running, or debugging rooture (.rut) code, or using the rooture MCP tools (eval, list_canvases, list_symbols, get_canvas).
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

1. Load MCP tool schemas: `ToolSearch "select:mcp__rooture__eval,mcp__rooture__list_canvases,mcp__rooture__list_symbols"`
2. Create a canvas before drawing: `(def {c} (new TCanvas ...))`
3. Eval expressions one at a time or compose multi-line strings.
4. Use `mcp__rooture__list_symbols` to inspect the environment.
5. Use `mcp__rooture__get_canvas` to view plots (returns PNG inline).

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
