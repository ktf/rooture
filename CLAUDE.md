# Rooture — notes for Claude Code

## MCP server: do not use batch mode

The rooture MCP server must **not** set `gROOT->SetBatch(true)`.  The user
runs rooture interactively and expects ROOT canvases / the EVE GUI to appear
on screen while Claude is also connected via MCP.  Batch mode suppresses the
display, which breaks that workflow.

If the server needs to save a canvas to a PNG (for `get_canvas`), use
`TCanvas::SaveAs` or `TPad::Print` — these work fine without batch mode.

## MCP server: taking GUI window screenshots

The MCP tools `get_window` and `get_canvas` capture ROOT GUI windows / canvases
as PNG images, archive them to a git repository, and return the image together
with a unique **id**.

### Required screenshot workflow

Every `get_canvas` / `get_window` call **must** be followed by `amend_screenshot`.
The full sequence is:

1. Call `get_canvas` or `get_window` with:
   - `motivation` — one-line reason (git commit subject)
   - `description` — your hypothesis: what you *expect* to see
2. Receive the image **and** an `id` (e.g. `20260507_123456_canvas_gauss`)
3. **Examine the image carefully** — does it match your hypothesis?
4. Call `amend_screenshot(id, observations)` with what you actually see:
   confirm or refute the hypothesis, note anomalies, quality issues,
   unexpected features, or follow-up actions needed.

Skipping step 4 leaves the archive incomplete. The sidecar `.md` committed by
`amend_screenshot` is the permanent record that pairs expectation with reality.

```
# Example
get_canvas name="c_spectra"
           motivation="Check pT spectrum shape after DCA cut"
           description="Expect a falling power-law spectrum peaking near 0.3 GeV/c with no discontinuities."

# → returns image + id: 20260507_123456_canvas_c_spectra

amend_screenshot id="20260507_123456_canvas_c_spectra"
                 observations="Spectrum matches expectation. Peak at ~0.3 GeV/c, smooth fall-off. No artefacts visible. High-pT tail (>10 GeV/c) is sparse but consistent with statistics."
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

## No Cling JIT inside concurrent pmap/future lambdas

**`new`, `.Method` calls, and any operation routed through `gInterpreter->Calc`
must NOT appear inside a `pmap` lambda (or any lambda run by multiple concurrent
futures).**

On macOS, LLVM ORC JIT calls `dispatchOutstandingMUs` → `MCContext::reset()`
during Cling symbol materialization.  This modifies shared dyld stubs while
other threads execute JIT-managed code (e.g. `TH1::Fill`), causing SIGSEGV or
abort ("Pure virtual function called").

**Rule:** perform all ROOT object construction (histograms, branches, trees)
**before** launching futures.  Pass the pre-created objects into the parallel
lambdas by including them in the element list (e.g. via `zip`).

```scheme
;;; correct — histograms pre-created on the main thread before pmap
(def {tf-histos}
  (map (\ {tf} {new TH1F (concat "h_" tf) "title" 200 0. 20.}) tf-names))

(pmap (\ {tf-h} {
  do
  (= {tf} (fst tf-h))
  (= {h}  (snd tf-h))
  ...
  (col-fill-h1 h data-col)
  h
}) (zip tf-names tf-histos))

;;; wrong — new TH1F inside pmap triggers Cling JIT during concurrent Fill
(pmap (\ {tf} {
  do
  ...
  (= {h} (new TH1F ...))   ; UNSAFE: Cling JIT races with other futures
  (col-fill-h1 h data-col)
  h
}) tf-names)
```

Operations safe inside futures: `load-branch`, `col-map-ptr`, `col-filter-ptr`,
`col-reduce-ptr`, `col-zip-ptr`, `col-fill-h1`, arithmetic, string operations.

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

## Use jit-fn for RDataFrame column and filter functions

When passing callables to `RDataFrame::Define` or `RDataFrame::Filter`, always
use `jit-fn` rather than C++ string expressions.  String expressions are an
escape hatch of last resort; jit-fn callables are idiomatic rooture and compile
to native C++ function pointers that RDF's `CallableTraits` can resolve without
ambiguity.

```scheme
;;; preferred — idiomatic rooture
(def {phi-fn}
  (jit-fn float (\{{float fAlpha} {float fSnp}}
    {(+ fAlpha (::ASin TMath fSnp))})))

(-> rdf {.Define "phi" phi-fn {"fAlpha" "fSnp"}})

;;; avoid — C++ string eval, opaque to the rooture reader
(-> rdf {.Define "phi" "fAlpha + TMath::ASin(fSnp)"})
```

Rules:
- Always declare explicit C++ types in formals (`{float name}`, `{UChar_t name}`,
  etc.) so RDF `CallableTraits` resolves argument types without `.Define` casts.
- Use the typed return form `(jit-fn rettype lambda)` for all non-void columns
  (`float`, `int`, `bool`, …).
- Filter functions use `(jit-fn bool lambda)` with the column list as the second
  argument to `.Filter`.
- Shared column functions (e.g. `ncls-fn`, `sigpt-fn`) should be defined once at
  the top of the script and reused across multiple RDataFrame pipelines.

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

## Script provide system — side-effect-free loading

All library scripts in `examples/` wrap their analysis code in `(fun {run} {do ...})`
and declare their public API with `(provide {...})` at the end. This means:

- **Loading a script is side-effect-free** — no histograms are filled, no canvases are
  drawn. Call `(run)` explicitly to execute the analysis.
- **`load` returns the manifest** — the Q-expression of exported symbol names from
  `provide`, so you immediately know what a script defines.
- **Pure library files** (`pid_tpc.rut`, `jit_progress.rut`, `eventsel.rut`, `rootlib.rut`)
  have `provide` but no `run` function — their definitions take effect on load.

### Standard two-step pattern

```scheme
;;; Step 1: set up inputs, then load — returns the manifest
(def {aod-paths} ...)
(def {trk-tree} "O2track_iu")
(load "examples/spectra_tpc.rut")
;;; → Exports: {run tf-pairs propagate-to-dca h-pt h-p ...}

;;; Step 2: run the analysis
(run)
```

### MCP `load` tool

Use the `load` MCP tool (not `eval` + `(load ...)`) to load scripts from the client side.
It wraps `(load ...)` and formats the output:

```
load path="examples/spectra_tpc.rut"
→ Loaded: examples/spectra_tpc.rut
  Exports: {run tf-pairs propagate-to-dca h-pt h-p h-pt-species ...}
  Output:
  Timeframes: 127
```

If a script has no `provide`, it returns `(no provide declaration — exports nothing)`.
For long-running loads (scripts that discover timeframes), use `eval_async` with
`(load "path")` instead.

## Available skills

All skills use the folder-based format (`.claude/skills/<name>/SKILL.md`) with YAML
frontmatter `description` fields that drive auto-invocation. Supporting scripts are
in separate files within each skill's folder and referenced from `SKILL.md`.

- `alice-analysis` — ALICE analysis umbrella: standard flow, tree name conventions,
  critical rules. Auto-invoked for any ALICE data analysis question. Points to sub-skills.

- `alice-aod` — reading AO2D files: file structure, timeframe iteration, branch names,
  and the **native column API** (`load-branch` + `col-*`) as the preferred approach.

- `rooture-analysis-basics` — full analysis setup: event selection, track quality cuts,
  DCA propagation, TPC+TOF PID via BB, CCDB parameter fetch, pmap patterns.
  Supporting file: `minimal-example.rut`.

- `spectra-comparison` — running `spectra_tpc.rut` and comparing output to reference.
  Supporting files: `comparison.rut`, `pid-plot.rut`.

- `plot-conventions` — per-species ROOT color assignments and general style rules.

- `rooture-lang` — language reference: syntax, semantics, MCP workflow, gotchas.

- `rooture-gui` — building TGFrame-based GUIs. Supporting file: `hello-world.rut`.

- `rooture-debug-gui` — diagnosing widget sizing, label update, and layout issues.

- `ccdb-proxy` — fetching calibration/PID objects from ALICE CCDB via the local
  credential proxy (`http://localhost:8888`). Covers auth, path for TPC BB params,
  `mPar` layout, and usage with `spectra_tpc.rut`.

- `alice-datasets` — tree names, file paths, and branch names for known local datasets
  (LHC15o, Run 3 AO2D). **Read this before running any example that loads AO2D data.**

- `col-kernel-perf` — when to use `col-kernel` vs `col-group-sum` / `col-fill-h1`.
  Decision rule, benchmark numbers (LHC15o), API pitfalls. Read before implementing
  any scatter-accumulate loop or per-centrality histogram filling.

- `rooture-gl` — native 3D rendering via `glDrawElements` on `TGLSAViewer`,
  bypassing TEve. Covers `gl-viewer`, `gl-mesh`, `gl-add`, geometry column helpers
  (`col-interleave`, `col-gen-tri-grid`), and critical pitfalls (output dtype,
  screenshot limitations). Demo: `examples/gl_torus.rut`.
