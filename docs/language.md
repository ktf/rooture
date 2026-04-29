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

## Atoms — mutable references

An atom is a thread-safe mutable container.  It holds a single value that can
be read and updated through four operations:

```scheme
(def {counter} (atom 0))    ; create an atom with initial value 0

(deref counter)             ; read the current value  → 0

(reset! counter 42)         ; replace the value unconditionally  → 42

(swap! counter + 1)         ; apply a function: (+ current 1)  → 43
(swap! counter (\ {x} {* x 2}))  ; any callable works  → 86
```

`swap!` accepts extra arguments that are appended after the current value:
```scheme
(swap! counter - 6)         ; (- current 6)
```

**Reference semantics** — assigning an atom to a second variable does not copy
it; both names refer to the same atom:

```scheme
(def {a} (atom 10))
(def {b} a)
(reset! b 99)
(deref a)   ; → 99
```

Atom equality (`==`) is identity: two atom values are equal only if they are the
same atom, not merely atoms holding equal values.

---

## Futures — asynchronous computation

A future runs a body expression on a background thread and returns immediately.
The result is retrieved with `deref`, which blocks until the computation finishes.

```scheme
(def {f} (future {(foldl + 0 {1 2 3 4 5})}))   ; starts immediately on a thread
;;; … do other work …
(deref f)   ; → 15  (blocks if not yet done)
```

`realized?` checks non-blocking whether the future has completed:

```scheme
(realized? f)   ; → 1 (true) or 0 (false)
```

**Environment capture** — the future sees a snapshot of the environment at the
point where `future` is called:

```scheme
(def {base} 100)
(def {f} (future {(+ base 7)}))
(deref f)   ; → 107
```

**Shared atoms** — futures can read and write atoms created on the main thread:

```scheme
(def {counter} (atom 0))
(def {f} (future {(swap! counter + 1)}))
(deref f)
(deref counter)   ; → 1
```

**Concurrency** — multiple futures run in parallel:

```scheme
(def {a} (future {(foldl + 0 {1 2 3 4 5})}))   ; sum = 15
(def {b} (future {(foldl * 1 {1 2 3 4 5})}))   ; product = 120
(assert-eq "sum"     (deref a) 15)
(assert-eq "product" (deref b) 120)
```

**Timeout** — `deref` accepts an optional timeout in milliseconds and a default value:

```scheme
(deref f 500)          ; return {} (nil) if not ready within 500 ms
(deref f 500 -1)       ; return -1 if not ready within 500 ms
```

**Thread-safety** — all Cling/ROOT calls made inside a future body are
automatically dispatched to the main thread, so ROOT objects are safe to use
from futures without any extra locking.

**Thread pool** — futures run on a fixed-size worker pool sized to the number
of logical CPUs.  Use `(parallelism)` to query the pool size and
`(set-parallelism! n)` to resize it (waits for in-flight futures to finish):

```scheme
(parallelism)           ; → 10  (example)
(set-parallelism! 4)    ; shrink pool — useful when ROOT's IMT already uses cores
(set-parallelism! 1)    ; serialize all futures (handy for debugging)
```

Future equality (`==`) is identity: two future values are equal only if they
are literally the same future, not merely futures that computed the same result.

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
| `(atom val)` | Create a mutable atom holding `val` |
| `(deref a)` | Read the current value of atom `a` |
| `(reset! a val)` | Replace the value of atom `a` with `val` |
| `(swap! a f args...)` | Update atom `a` to `(f current args...)` |
| `(future {body})` | Evaluate `body` on a background thread; returns a future |
| `(deref f)` | Block until future `f` completes; return its result (also works on atoms) |
| `(deref f ms)` | Block up to `ms` milliseconds; return `{}` on timeout |
| `(deref f ms default)` | Block up to `ms` milliseconds; return `default` on timeout |
| `(realized? f)` | Return `1` if future `f` has completed, `0` otherwise |
| `(parallelism)` | Return the number of worker threads in the thread pool |
| `(set-parallelism! n)` | Resize the thread pool to `n` workers |
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
