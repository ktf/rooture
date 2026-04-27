# Rooture — notes for Claude Code

## MCP server: do not use batch mode

The rooture MCP server must **not** set `gROOT->SetBatch(true)`.  The user
runs rooture interactively and expects ROOT canvases / the EVE GUI to appear
on screen while Claude is also connected via MCP.  Batch mode suppresses the
display, which breaks that workflow.

If the server needs to save a canvas to a PNG (for `get_canvas`), use
`TCanvas::SaveAs` or `TPad::Print` — these work fine without batch mode.

## MCP server: taking GUI window screenshots

The MCP tool `get_window` captures any open ROOT GUI window as a PNG and returns
it as an image.  Use this to inspect layouts and diagnose visual issues without
leaving Claude Code.

```
# after loading a script that creates a window bound to the symbol "win":
get_window symbol="win"
```

The underlying rooture builtin `save-window` accepts a TGFrame object and a path:
```scheme
(save-window win "/tmp/screenshot.png")
```

**Important caveats on macOS**:
- `save-window` uses `TASImage::FromWindow` which captures the window's backing
  store.  If multiple windows overlap (e.g. after reloading a script that creates
  a second window), the screenshot may show the wrong window.  Close stale windows
  before reloading, or use `(.MapRaised win)` (already the default in GUI examples)
  to bring the new window to the front.
- `libASImage` is loaded on demand by `save-window`; no manual `(.Load gSystem
  "libASImage")` is needed.

## Cling errors are never acceptable noise

Any error printed by Cling (e.g. `error: no matching constructor`, `error: no member named '...'`) **must be eliminated**, even if it is technically "just" a failed probe that is immediately retried successfully.  These errors pollute the user's terminal, slow down execution (Cling error-recovery is expensive), and make it harder to spot real problems.

When Cling errors appear:
- Add caching so the failing probe is skipped on subsequent calls.
- Restructure the probe order so the winning form is tried first.
- Do not leave known-failing probes in the hot path.

## Prefer native rooture over ProcessLine

Rooture examples should use **native rooture syntax** as much as possible.
`(. ProcessLine gInterpreter "...")` and `(. Calc gInterpreter "...")` are
escape hatches for things that genuinely cannot be expressed otherwise (e.g.
declaring a parameterised `TF1` with a custom `double f(double*, double*)`
signature that the callable bridge cannot produce).

Before reaching for `ProcessLine`, ask: can this be done with `new`, method
calls, `dotimes`, or a rooture lambda?  If yes, do it that way.  The goal is
to demonstrate how expressive rooture itself is, not to use it as a wrapper
around Cling string-eval.

## Rooture language quirks

### Undefined symbols auto-convert to strings
At the top level, any symbol not bound in the environment becomes a **string with the same spelling** (see `lenv_get`). Consequences:
- `(load stdlib.rut)` works without quotes — `stdlib.rut` becomes `"stdlib.rut"`.
- If a `def` is skipped (e.g. prior error short-circuits the expression), later uses of that variable silently become strings, causing `"Got String, expected Object"` errors in method calls.

### Lambda bodies: use `do` for multiple expressions
`{expr1 expr2}` as a lambda body evaluates to `(expr1 expr2)` — calling expr1 as a function on expr2. For sequential execution:
```
(\{i} {do expr1 expr2})   ; evaluates both, returns last
```
Use `=` (not `def`) for local variables inside lambdas.

### Static method calls
`(::Method ClassName args...)` and `(:: Method ClassName args...)` are identical — the `::` prefix on the method symbol is syntactic sugar for the two-token form.

### Namespaced C++ types need no quotes
The symbol regex includes `:`, so `ROOT::RDF::TH1DModel` is a single symbol and auto-converts to the string `"ROOT::RDF::TH1DModel"`. Write:
```
(new ROOT::RDF::TH1DModel "name" "title" 512 2. 110.)
(::FromCSV ROOT::RDF fileUrl)
```
No quoting needed for namespaced class or namespace names.

### Use `doto` to reduce verbosity
When making multiple method calls on the same object, always prefer `doto` over repeated `.Method` calls:
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

### `if` branches must be Q-expressions
```
(if cond {true-val} {false-val})   ; correct
(if cond  true-val   false-val)    ; Error: expected Q-Expression
```

## Available skills

- `rooture-lang` (in `.claude/skills/rooture-lang.md`) — explains how the
  rooture language works and its quirks.  Invoke this skill whenever you need
  context on the language before writing or debugging rooture (`.rut`) code.

- `rooture-gui` (in `.claude/skills/rooture-gui.md`) — how to build ROOT GUI
  windows, buttons, and layouts in rooture.  Invoke this skill whenever the
  user asks for a GUI, dialog, or any `TGFrame`/`TGWidget`-based interface.

- `rooture-debug-gui` (in `.claude/skills/rooture-debug-gui.md`) — how to
  debug GUI and layout issues: widgets not appearing, zero-width labels, wrong
  positions, text not refreshing after SetText.  Invoke this skill whenever
  something in a rooture GUI looks wrong or a label fails to update.

- `alice-aod` (in `.claude/skills/alice-aod.md`) — how to read ALICE Run 3
  AO2D data in rooture: file structure, building TChains across time frames,
  friend chains for track+trackextra, RDataFrame column definitions (eta, phi,
  pt, nCls), and the interactive 3D cluster-cut GUI example.  Invoke this skill
  whenever the user is working with AO2D.root files or ALICE O2 track data.
