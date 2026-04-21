# rooture-lang

Use this skill when the user wants to write, run, or debug rooture (`.rut`) code, or use the rooture MCP tools (`mcp__rooture__eval`, `mcp__rooture__list_canvases`, `mcp__rooture__list_symbols`, `mcp__rooture__get_canvas`).

Rooture is a Lisp-like language for interactive ROOT data analysis. Expressions are evaluated via the `mcp__rooture__eval` tool.

---

## Syntax reference

### Variable definition
```
(def {name} value)
```

### Function definition
```
(defn name [arg1 arg2] body)
```

### Object construction
```
(new ClassName arg1 arg2 ...)
```
Strings use double quotes. Bare symbols are passed as identifiers (e.g. ROOT names).

### Method calls
```
(.MethodName object arg1 arg2)
```

### Multiple methods on one object (`doto`)
```
(doto obj
  {MethodName arg1 arg2}
  {OtherMethod}
)
```
Note: inside `doto` blocks, method names are bare symbols (no dot prefix).

### Threading macro (`->`)
```
(-> expr
    {. MethodName arg}
    {. OtherMethod})
```

### Global object access
```
(. MethodName gObject arg)   ; e.g. (. Rndm gRandom)
(. Exec gSystem "ls")
```

### Static method calls (`::`)
```
(:: Method ClassName args...)     ; long form
(::Method ClassName args...)      ; sugar — mirrors (.Method obj args...)
```
Examples:
```
(::Create TEveManager)                    ; TEveManager::Create()
(::GetColor TColor 15 76 129)             ; TColor::GetColor(r,g,b) → Int_t
(::Sin TMath 1.5707)                      ; TMath::Sin(x) → Double_t
```
Return types are dispatched automatically: void → `()`, pointer → TOBJ,
floating-point → float value, integral/enum → integer value.

### Lambda
```
(\ {arg1 arg2} { body })
(fn [arg] body)
```

### Load a file
```
(load "stdlib.rut")
(load stdlib.rut)   ; unquoted also works
```

---

## Examples

### Histogram with gaussian fill
```
(def {c} (new TCanvas "c" "My Canvas" 10. 10. 700. 500.))
(def {h} (new TH1F "h" "Gaussian" 100 -5. 5.))
(.FillRandom h "gaus" 1000)
(.Draw h)
```

### doto style
```
(def {c} (new TCanvas nut FirstSession 10. 10. 700. 900.))
(def {a} (new TH1F "foo" "bar" 100 -10. 60.))
(doto a
  {SetFillColor 3}
  {SetFillStyle 3003}
  {FillRandom gaus 1000}
  {Draw}
)
```

### RDataFrame with threading macro
```
(load "stdlib.rut")
(-> (new ROOT::RDataFrame 1000)
    {. Define "x" (\ {} { (. Rndm gRandom) })}
    {. Histo1D "x"}
    {. DrawClone})
```

---

## Workflow

1. Load MCP tool schemas with ToolSearch before first use:
   `ToolSearch "select:mcp__rooture__eval,mcp__rooture__list_canvases,mcp__rooture__list_symbols"`
2. Create a canvas before drawing: `(def {c} (new TCanvas ...))`
3. Eval expressions one at a time or compose multi-line strings.
4. Use `mcp__rooture__list_symbols` to inspect the current environment.
5. Use `mcp__rooture__list_canvases` to see open canvases.

## Viewing plots

`mcp__rooture__get_canvas` returns a PNG inline — only visible in environments that render images (e.g. claude.ai web). In Claude Code desktop/CLI, save to disk instead:
```
(.SaveAs c "plot.png")
```
Then open the file externally.

## Making plots prettier

### Custom Pantone colors
Use `new TColor` with a unique index (≥5000 to avoid clashes) and normalised RGB (0–1):
```
(def {colBlue}    (new TColor 5001 0.059 0.298 0.506 "PantoneClassicBlue"))   ; #0F4C81
(def {colMagenta} (new TColor 5002 0.733 0.149 0.286 "PantoneVivaMagenta"))   ; #BB2649
```
Then refer to the color by its integer index in `SetFillColor`, `SetLineColor`, etc.

### Transparent fill + styled line
```
(doto h
  {SetFillColorAlpha 5001 0.6}   ; fill with 60 % opacity
  {SetLineColor 5001}
  {SetLineWidth 2}
)
```

### Styling a fit function after fitting
After `{Fit gaus}`, ROOT registers a `TF1` named `"gaus"`. Retrieve it with the `@name`
syntax (looks up any TNamed object via `gROOT->FindObject`):
```
(doto @gaus
  {SetLineColor 5002}
  {SetLineWidth 3}
)
```

### Grid lines
Call `SetGridx`/`SetGridy` on the canvas, then force a refresh:
```
(doto @canvas
  {SetGridx 1}
  {SetGridy 1}
  {Modified}
  {Update}
)
```

### `@name` syntax
`@foo` is shorthand for `gROOT->FindObject("foo")` — useful for retrieving any named
ROOT object (canvases, histograms, fit functions) without holding an explicit variable.

---

## Language semantics: undefined symbols become strings

At the top level, any symbol not bound in the environment is **auto-converted to a string** with the same spelling (see `lenv_get` in rooture.cxx). This has two important consequences:

1. **Unquoted filenames work**: `(load stdlib.rut)` and `(load stdlib.rut)` are equivalent — the bare symbol `stdlib.rut` becomes the string `"stdlib.rut"` automatically.
2. **Silent failures look like type errors**: If a variable is never bound (e.g. because its `def` was short-circuited by an earlier error), later uses of that variable evaluate to a string rather than an error. This shows up as `"Function '.' passed incorrect type for argument 1. Got String, expected Object."` — the object variable silently became a string.

When debugging "Got String, expected Object", trace back to whether the object's `def` actually ran successfully.

## Workaround: typeless pointers via `ProcessLine`

Some methods return objects whose TClass can't be recovered (rooture stores them as `<object @0x...>` with no class name). These can't be passed to constructors or methods because rooture generates an untyped hex literal instead of a cast pointer.

Workaround: run the affected code as a Cling string via `gInterpreter->ProcessLine`:
```
(.ProcessLine gInterpreter "{ MyClass* obj = someMethod(); obj->doThing(); }")
```

**Caveat: no inner double-quotes.** rooture wraps strings in `"..."` when building the Cling argument, so any `"` inside the string will break the expression. Avoid string literals inside `ProcessLine` calls, or restructure to not need them.

## Lambda bodies with multiple expressions

A lambda body is a Q-expression `{...}`. When called, the body `{expr1 expr2}` is evaluated as the S-expression `(expr1 expr2)`, which tries to **call `expr1` as a function on `expr2`** — almost certainly wrong.

For sequential execution, use `do` as the first element of the body:
```
(\ {i} {do
  expr1
  expr2
})
```
This evaluates to `(do expr1 expr2)`, evaluating both in order and returning the last result.

Use `=` (not `def`) for local bindings inside lambdas — `def` writes to the global scope.

## `if` branches must be Q-expressions

`if` requires its then/else branches to be Q-expressions:
```
(if cond {true-value} {false-value})   ; correct
(if cond  true-value   false-value)    ; ERROR: "Got Number/Float, expected Q-Expression"
```

## Common pitfalls

- `doto` block methods use bare symbols without a dot: `{FillRandom gaus 1000}`, not `{.FillRandom gaus 1000}`.
- For `FillRandom`, the distribution name must be a quoted string when called via `.Method` syntax: `(.FillRandom h "gaus" 1000)`.
- Floating-point literals need a decimal point: `10.` not `10`.
- Canvas must exist before calling `Draw`.
- Static method calls (e.g. `TColor::GetColor`) are not supported via the `.` operator — use `new TColor` with an explicit index instead.
- `mcp__rooture__list_symbols` / `(symbols)` only returns non-function user symbols; it currently returns `<builtin>` in MCP mode — use `@name` to retrieve known objects instead.
- Use color indices ≥6000 for custom `TColor`s to avoid clashes with ROOT's built-in palette (indices up to ~5000 are often already taken).
- For prettier plots, use line width 3–4 (`SetLineWidth 3` or `4`) on all curves.
- `Clone` returns a `TObject*` in rooture, losing the subclass type. Use `@cloneName` to retrieve the clone with its real class for method calls like `SetContour`.
