# Rooture — notes for Claude Code

## MCP server: do not use batch mode

The rooture MCP server must **not** set `gROOT->SetBatch(true)`.  The user
runs rooture interactively and expects ROOT canvases / the EVE GUI to appear
on screen while Claude is also connected via MCP.  Batch mode suppresses the
display, which breaks that workflow.

If the server needs to save a canvas to a PNG (for `get_canvas`), use
`TCanvas::SaveAs` or `TPad::Print` — these work fine without batch mode.

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
