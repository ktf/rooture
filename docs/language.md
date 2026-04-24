# ROOTure language reference

ROOTure is a Lisp dialect. This page covers the syntax and built-in forms.
For ROOT-specific interop see the examples in [`examples/`](../examples/).

---

## Everything is an expression

```scheme
(def {h} (new TH1F "foo" "bar" 100 -5. 5.))   ; create object
(.FillRandom h gaus 10000)                      ; call method
(.Draw h)                                       ; another method
```

Unquoted symbols auto-convert to strings, so `gaus` and `"gaus"` are identical.

---

## Method calls

```scheme
(.Method obj arg1 arg2)     ; instance method
(::StaticMethod Class arg)  ; static / namespace-scoped method
```

---

## `doto` — multiple methods on one object

```scheme
(doto canvas
  {SetGrid}
  {SetLogx}
  {>> {GetXaxis} {SetTitle "mass [GeV]"}}   ; sub-object pipeline
  {Update})
```

The `{>> {Step1} {Step2}}` form threads the object through a mini-pipeline,
useful when you need to call a method on a sub-object (e.g. an axis).

---

## `|` — tail strings in Q-expressions

A `|` inside a Q-expression starts an inline string that runs to end-of-line
or to the closing `}`, whichever comes first — no quotes needed:

```scheme
{.Filter | pt > 20 && eta < 2.4}          ; same as {.Filter "pt > 20 && eta < 2.4"}
{SetTitle | #tau [ps]}                     ; special characters need no escaping
{.Define "col" | sqrt(x*x + y*y)}         ; earlier args can still be quoted normally
```

Multiple `|` strings can appear on separate lines within one Q-expression:

```scheme
{1 | foo
 0 1 2 | bar}           ; equivalent to {1 "foo" 0 1 2 "bar"}
```

The only character that cannot appear in an inline string is `}`.

---

## `->` — method chaining

```scheme
(-> dataFrame
  {.Filter | pt > 10}
  {.Define "eta2" | eta*eta}
  {.Histo1D "eta2"}
  {.DrawClone})
```

---

## `@name` — look up named objects

```scheme
(doto @myCanvas
  {Modified}
  {Update})
```

Any named ROOT object accessible via `gROOT->FindObject` can be referenced this way.
Typing `@` at the REPL prompt triggers tab-completion over all live named objects.

---

## Lambdas

```scheme
(def {myFilter}
  (\{x} {> x 0.5}))

(.Filter df myFilter {"x"})
```

Rooture lambdas are automatically wrapped as C++ callables when passed to ROOT methods
that expect a function pointer or `std::function`.

---

## `annotate` — documenting symbols for AI-assisted analysis

Attach a human-readable description to any symbol with `annotate`:

```scheme
(def {fitRange} {2.9 3.2})
(annotate fitRange "J/psi fit window in GeV — widen to include more background")

(def {nBins} 128)
(annotate nBins "Histogram binning — increase for better mass resolution at the cost of stats")
```

Annotations are accessible at any time:

```scheme
(annotations)   ; => {{"fitRange" "J/psi fit window ..."} {"nBins" "Histogram binning ..."}}
```

When running with `--mcp`, the `list_annotations` tool exposes all annotations to the
connected AI assistant, letting it understand the meaningful knobs in your analysis.

---

## Built-in functions

| Form | Description |
|------|-------------|
| `(def {x} val)` | Bind `x` globally |
| `(= {x} val)` | Bind `x` locally (inside a lambda) |
| `(\{args} {body})` | Lambda |
| `(if cond {t} {f})` | Conditional |
| `(do e1 e2 ...)` | Sequence; returns last value |
| `(load "file.rut")` | Load and evaluate a file |
| `(print val ...)` | Print values |
| `(new ClassName args...)` | Construct a ROOT object |
| `(. Method obj args...)` | Call instance method |
| `(:: Method Class args...)` | Call static method |
| `(-> val {step} ...)` | Thread value through steps |
| `(doto obj {step} ...)` | Apply multiple steps to one object |
| `(symbols)` | List all user-defined symbol names |
| `(canvases)` | List open canvas names |
| `(annotations)` | List all annotated symbols |
| `(annotate sym "text")` | Attach annotation to a symbol |

---

## Quirks

- **Unknown symbols become strings** — `foo` and `"foo"` are interchangeable at the top level.
- **Lambda bodies need `do` for sequences** — `(\{x} {expr1 expr2})` calls `expr1` on `expr2`; use `(\{x} {do expr1 expr2})` for sequencing.
- **`if` branches must be Q-expressions** — `(if cond {a} {b})`, never bare values.
- **Namespaced types need no quotes** — `ROOT::RDF::TH1DModel` is a single symbol and auto-converts to a string.
