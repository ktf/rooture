# rooture-gui

Use this skill when the user wants to build a ROOT GUI (windows, buttons, dialogs,
layouts) in rooture. Always invoke `rooture-lang` first for general language context.

---

## Setup

ROOT's GUI toolkit lives in `libGui`. Load it and register `gClient` before doing
anything else:

```scheme
(.Load gSystem "libGui")
(global "gClient")   ; makes gClient available as a rooture object
```

`(global name)` resolves any C++ global by name through Cling and binds it in the
rooture environment. It must be called at the top level (after `libGui` is loaded).

---

## Window skeleton

```scheme
(.Load gSystem "libGui")
(global "gClient")

(def {win} (new TGMainFrame (.GetRoot gClient) 400 200))

; ... add widgets ...

(.SetWindowName win "My Window")
(.MapSubwindows win)
(.Resize win (.GetDefaultWidth win) (.GetDefaultHeight win))
(.MapWindow win)
```

Always call `MapSubwindows` → `Resize` → `MapWindow` in that order.

---

## Layout hints

`TGLayoutHints` controls how a widget is placed inside its parent frame.
The constructor is:

```
(new TGLayoutHints flags left right top bottom)
```

Key flag values (combine by adding):

| Flag | Value | Meaning |
|------|-------|---------|
| `kLHintsLeft`    | 1  | Align left |
| `kLHintsCenterX` | 2  | Centre horizontally |
| `kLHintsRight`   | 4  | Align right |
| `kLHintsTop`     | 8  | Align top |
| `kLHintsCenterY` | 16 | Centre vertically |
| `kLHintsBottom`  | 32 | Align bottom |
| `kLHintsExpandX` | 64 | Stretch horizontally |
| `kLHintsExpandY` | 128| Stretch vertically |

Common combinations:

```scheme
(new TGLayoutHints 18 5 5 5 5)   ; CenterX|CenterY, 5px padding
(new TGLayoutHints 65 2 2 2 2)   ; Left|ExpandX — fills the row
(new TGLayoutHints 9  5 5 5 5)   ; Left|Top (default-ish)
```

---

## Common widgets

### Button

```scheme
(def {btn} (new TGTextButton parent "Label"))
(.AddFrame parent btn (new TGLayoutHints 18 5 5 5 5))
```

### Label

```scheme
(def {lbl} (new TGLabel parent "Some text"))
(.AddFrame parent lbl (new TGLayoutHints 9 5 5 5 5))
```

### Single-line text entry

```scheme
(def {entry} (new TGTextEntry parent "default text"))
(.Resize entry 200 (.GetDefaultHeight entry))
(.AddFrame parent entry (new TGLayoutHints 65 5 5 5 5))
```

### Number entry

```scheme
(def {num} (new TGNumberEntry parent 42. 6 -1))
(.AddFrame parent num (new TGLayoutHints 9 5 5 5 5))
```

### Horizontal frame (for side-by-side widgets)

```scheme
(def {hf} (new TGHorizontalFrame parent 0 0))
(.AddFrame hf widgetA (new TGLayoutHints 9 2 2 2 2))
(.AddFrame hf widgetB (new TGLayoutHints 9 2 2 2 2))
(.AddFrame parent hf (new TGLayoutHints 65 5 5 5 5))
```

---

## Signal/slot: connecting a button to a rooture lambda

Use `(connect widget "Signal()" lambda)` to wire a ROOT signal to a rooture function:

```scheme
(def {click-count} 0)

(connect btn "Clicked()" (\
  {}
  {do
    (def {click-count} (+ click-count 1))
    (.SetText lbl (new TGString (str click-count)))
    (.Layout win)}))
```

- `connect` returns an integer callback ID.
- **Signal arguments are never forwarded.** The rooture shim is declared as `static void fire()` with zero parameters. ROOT drops the signal argument (e.g. the `Int_t` from `Selected(Int_t)`) before reaching rooture. **Always use 0-formal lambdas** (`\{}`) for every signal callback, even when the signal carries a value. Query widget state directly inside the callback instead:
  ```scheme
  ; WRONG — 1-formal lambda is partially applied, body never executes:
  (connect combo "Selected(Int_t)" (\{id} {doSomethingWith id}))

  ; CORRECT — 0-formal, reads state from widget directly:
  (connect combo "Selected(Int_t)" (\{} {do
    (= {sel} (.GetSelected combo))
    ...}))
  ```
- Use `(str num)` to convert a number to a string for label updates.
- Use `(def {var} val)` — **not** `(= var val)` — to update global variables from
  inside the callback. `=` only writes to the lambda's local scope.
- `TGLabel::SetText` takes a `TGString*`: `(new TGString "text")` creates one.
- Call `(.Layout win)` after updating label text to force a geometry refresh.

### How it works internally

rooture's `connect` builtin:
1. Stores a `__rut_fire` function-pointer global in Cling (once, at first call).
2. Defines a `__rut_dispatch(int)` shim in Cling (once).
3. Calls `TQObject::Connect(widget, signal, nullptr, nullptr, "__rut_dispatch(N)")`.
4. When the signal fires, Cling executes the shim, which writes the callback ID
   to a pipe.
5. A `TFileHandler` drains the pipe in the **next event-loop iteration** — outside
   Cling's slot-dispatch context — and runs the rooture lambda safely.

The deferred-pipe design is essential: running Cling builtins (`new`, `.method`)
**inside** a Cling-dispatched slot causes silent re-entrancy failures.

---

## Dynamic show/hide of widget rows

Use `ShowFrame`/`HideFrame` + `Layout` to reveal or hide entire rows based on widget state.
A typical pattern is a combo box that controls which parameter row is visible:

```scheme
;;; build the rows and add them to win unconditionally
(def {deg-row}  (new TGHorizontalFrame win 580 36))
(.AddFrame win deg-row  (new TGLayoutHints 65 5 5 2 2))
(def {npts-row} (new TGHorizontalFrame win 580 36))
(.AddFrame win npts-row (new TGLayoutHints 65 5 5 2 2))

;;; callback — hide all optional rows, then show the relevant one
(def {on-combo-change} (\{} {do
  (= {sel} (.GetSelected combo))
  (.HideFrame win deg-row)
  (.HideFrame win npts-row)
  (if (== sel 1.) {.ShowFrame win deg-row}  {})
  (if (== sel 2.) {.ShowFrame win npts-row} {})
  (.Layout win)}))

(connect combo "Selected(Int_t)" on-combo-change)

;;; apply initial state AFTER windows are mapped
(doto win
  {MapSubwindows}
  {Resize (.GetDefaultWidth win) (.GetDefaultHeight win)}
  {MapRaised})
(on-combo-change)   ; <-- must come after MapRaised
```

**Critical timing rule**: `HideFrame` calls `UnmapWindow()` internally. If the frame has
never been mapped yet (i.e. you call `HideFrame` before `MapSubwindows`), `UnmapWindow()`
is a no-op, and `MapSubwindows` subsequently maps all children — overriding the hide.
Always apply the initial show/hide state **after** `MapSubwindows` + `MapRaised`.

**Use flat `if` sequences, not nested `if/do`**: when multiple rows can be independently
shown or hidden, hide all of them first and then conditionally show the right one:

```scheme
; preferred — clear and correct
(.HideFrame win row-a)
(.HideFrame win row-b)
(if (== sel 1.) {.ShowFrame win row-a} {})
(if (== sel 2.) {.ShowFrame win row-b} {})

; avoid — nested do/if chains are error-prone and harder to read
(if (== sel 1.)
  {do (.ShowFrame win row-a) (.HideFrame win row-b)}
  {do (.HideFrame win row-a) (if (== sel 2.) ...)})
```

---

## Runtime inspection of bound symbols

To verify that a reload applied correctly or to diagnose why a callback isn't working,
use `(print symbol)` — `lval_print` renders a lambda as `(\ formals body)`:

```scheme
(print on-combo-change)
; prints: (\ {} (do (= {sel} (.GetSelected combo)) ...))
```

If the formals show `{id}` when you expected `{}`, the old definition is still active
and the callback is being partially applied (body never executes). Reload the script.

---

## Full example: Hello World with click counter

```scheme
(.Load gSystem "libGui")
(global "gClient")

(def {win} (new TGMainFrame (.GetRoot gClient) 300 120))
(def {btn} (new TGTextButton win "Hello World"))
(def {lbl} (new TGLabel win "Clicks: 0"))
(def {click-count} 0)

(def {hints} (new TGLayoutHints 18 5 5 5 5))
(.AddFrame win btn hints)
(.AddFrame win lbl hints)

(connect btn "Clicked()" (\
  {}
  {do
    (def {click-count} (+ click-count 1))
    (.SetText lbl (new TGString (str click-count)))
    (.Layout win)}))

(.SetWindowName win "Hello World")
(.MapSubwindows win)
(.Resize win (.GetDefaultWidth win) (.GetDefaultHeight win))
(.MapWindow win)
```

See `examples/hello_gui.rut` for the runnable version.

---

## Pitfalls

- `(global "gClient")` must come **after** `(.Load gSystem "libGui")` — `gClient` is
  null before the GUI library is initialised.
- `"Hello World"` in `(new TGTextButton ...)` must be a **quoted string** — the `|`
  tail-string syntax only works inside `{...}` Q-expressions, not `(...)` S-expressions.
- Always `Resize` using `GetDefaultWidth`/`GetDefaultHeight` after `MapSubwindows`,
  otherwise the window may appear collapsed.
- `AddFrame` returns `()` (void) — chain it before `MapSubwindows`, not after.
- Enum constants like `kLHintsCenterX` are not in scope; use their numeric values.
- **`def` vs `=` in callbacks**: use `(def {var} val)` to update globals; `=` only
  writes to the lambda's local scope and the change won't be visible outside.
- **Signal argument not forwarded**: the rooture shim has zero parameters. A lambda with
  one or more formals will be partially applied (body never runs) when the signal fires.
  Always use `\{}` (0 formals) for every `connect` callback.
- **`HideFrame` before `MapSubwindows` is a no-op**: `HideFrame` calls `UnmapWindow()`
  which does nothing on an unmapped window. Call `HideFrame`/`ShowFrame` only after
  `MapSubwindows` + `MapRaised` (or equivalent `MapWindow`).
- **Prefer `MapRaised` over `MapWindow`** for top-level frames — it brings the window
  to the front, which is important when reloading scripts that re-create the same window.
