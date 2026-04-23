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

## Full example: Hello World

```scheme
(.Load gSystem "libGui")
(global "gClient")

(def {win} (new TGMainFrame (.GetRoot gClient) 300 100))
(def {btn} (new TGTextButton win "Hello World"))
(.AddFrame win btn (new TGLayoutHints 18 5 5 5 5))
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
