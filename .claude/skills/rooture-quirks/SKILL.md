---
description: Rooture language quirks and gotchas — symbol-to-string auto-conversion, method-name shadowing, def vs =, lambda do-blocks, if Q-expressions, doto style. Read before writing any non-trivial rooture (.rut) code, and whenever a rooture script fails with a confusing type error ("Got String, expected Object", "Got Object, expected String").
---

# rooture-quirks

Language quirks that routinely bite when writing rooture. Companion to
`rooture-lang` (the general language reference).

## Undefined symbols auto-convert to strings

At the top level, any symbol not bound in the environment becomes a **string
with the same spelling** (see `lenv_get`). Consequences:

- `(load stdlib.rut)` works without quotes — `stdlib.rut` becomes `"stdlib.rut"`.
- If a `def` is skipped (e.g. prior error short-circuits the expression), later
  uses of that variable silently become strings, causing
  `"Got String, expected Object"` errors in method calls.

## Variables can shadow `.Method` sugar — never name a variable after a method

`(.Method obj args...)` is sugar for `(. Method obj args...)`, so the method
name is an ordinary symbol looked up in the environment. If a variable with
that name is in scope, its *value* is passed as the method-name argument:

```
(= {cd} (.Clone h "diff"))   ; local named `cd`
(.cd cnv 1)                  ; expands to (. cd cnv 1) — `cd` resolves to the
                             ; TH1 object, not the string "cd"
; → Error: Function '.' passed incorrect type for argument 0.
;          Got Object, expected String.
```

This only works at all because *unbound* symbols auto-convert to strings (see
above). Watch out for short, lowercase method names (`cd`, `pt`, `ls`, `cp`):
never bind a variable whose name equals a method you call via the `.name`
sugar in the same scope. Rename the variable (e.g. `cd` → `hdiff`).

## Lambda bodies: use `do` for multiple expressions

`{expr1 expr2}` as a lambda body evaluates to `(expr1 expr2)` — calling expr1
as a function on expr2. For sequential execution:

```
(\{i} {do expr1 expr2})   ; evaluates both, returns last
```

Use `=` (not `def`) for local variables inside lambdas. `def` writes to the
global environment — which also means `(def {counter} (+ counter 1))` inside a
lambda is the idiomatic way to update a global accumulator.

## `if` branches must be Q-expressions

```
(if cond {true-val} {false-val})   ; correct
(if cond  true-val   false-val)    ; Error: expected Q-Expression
```

## Static method calls

`(::Method ClassName args...)` and `(:: Method ClassName args...)` are
identical — the `::` prefix on the method symbol is syntactic sugar for the
two-token form.

## Namespaced C++ types need no quotes

The symbol regex includes `:`, so `ROOT::RDF::TH1DModel` is a single symbol
and auto-converts to the string `"ROOT::RDF::TH1DModel"`. Write:

```
(new ROOT::RDF::TH1DModel "name" "title" 512 2. 110.)
(::FromCSV ROOT::RDF fileUrl)
```

No quoting needed for namespaced class or namespace names.

## Use `doto` to reduce verbosity

When making multiple method calls on the same object, always prefer `doto`
over repeated `.Method` calls:

```
; preferred
(def {sl}
  (doto (new TGHSlider parent 160 3 -1)
    {SetRange 1 50}
    {SetPosition 10}))

; avoid
(def {sl} (new TGHSlider parent 160 3 -1))
(.SetRange sl 1 50)
(.SetPosition sl 10)
```

`doto` also works inside callbacks for sequential operations on an object:

```
(doto h {Reset} {FillRandom gf 5000})
```

Inside `doto`, method names are bare symbols without the dot:
`{FillRandom gaus 1000}`, not `{.FillRandom ...}`.

## `col-zip-ptr` output dtype defaults to the *input* dtype

`col-zip-ptr` (and `col-map-ptr`) infer the result column's dtype from the
**input** column, not from the jit-fn's return type. When the function returns
a *different* type than its inputs — the classic case being an equality/compare
test `(Int_t,Int_t)->float` returning 0/1 — you MUST pass the output type as the
trailing string argument:

```
;; WRONG — output defaults to int; the float return is mis-read as an int and
;; the loop silently reads back the first argument (looks like a copy of `a`).
(col-zip-ptr neq-fp a b)

;; RIGHT — output is float, matching the jit-fn's return type.
(col-zip-ptr neq-fp a b "float")
```

This fails **silently**: no error, just a wrong column (e.g. a file-comparison
that returns "0 differences" on data that actually differs). It only bites when
the jit-fn's return type ≠ input dtype, so float-in/float-out and double-in/
double-out happen to work by accident — integer columns are where it shows up.
The comparison itself still runs in the input type, so once the output type is
given, equality is exact for every dtype. See `examples/validate_aod_df.rut`.
