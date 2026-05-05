---
description: Building ROOT GUI windows, buttons, sliders, and layouts in rooture. Use when the user wants a GUI, dialog, or any TGFrame/TGWidget-based interface.
---

# rooture-gui

Always invoke `rooture-lang` first for general language context.

---

## Setup

```scheme
(.Load gSystem "libGui")
(global "gClient")   ; must come AFTER libGui loads
```

## Window skeleton

```scheme
(def {win} (new TGMainFrame (.GetRoot gClient) 400 200))
; ... add widgets ...
(.SetWindowName win "My Window")
(.MapSubwindows win)
(.Resize win (.GetDefaultWidth win) (.GetDefaultHeight win))
(.MapRaised win)   ; prefer MapRaised over MapWindow — brings window to front
```

Always call `MapSubwindows` → `Resize` → `MapRaised` in that order.

## Layout hints (TGLayoutHints)

```
(new TGLayoutHints flags left right top bottom)
```

Key values: Left=1, CenterX=2, Right=4, Top=8, CenterY=16, Bottom=32, ExpandX=64, ExpandY=128

```scheme
17   ; Left|CenterY  — left-packed, vertically centred
18   ; CenterX|CenterY  — centred both axes
65   ; Left|ExpandX  — full width
```

## Common widgets

```scheme
;;; Button
(def {btn} (new TGTextButton parent "Label"))
(.AddFrame parent btn (new TGLayoutHints 18 5 5 5 5))

;;; Label — initialise with placeholder text wide enough for expected content
(def {lbl} (new TGLabel parent "placeholder text    "))
(.AddFrame parent lbl (new TGLayoutHints 65 5 5 5 5))

;;; Horizontal frame
(def {hf} (new TGHorizontalFrame parent 0 0))
(.AddFrame hf widgetA (new TGLayoutHints 17 2 2 2 2))
(.AddFrame parent hf (new TGLayoutHints 65 5 5 5 5))

;;; Number entry
(def {num} (new TGNumberEntry parent 42. 6 -1))

;;; Horizontal slider (id=-1, style=3=kSlider1 — flat)
(def {sl} (doto (new TGHSlider parent 160 3 -1) {SetRange 1 50} {SetPosition 10}))
```

## Signal/slot: connect

```scheme
(connect btn "Clicked()" (\{} {do
  (def {var} new-val)         ; use def, not =, to update globals
  (.SetText lbl (new TGString (str var)))
  (.Layout win)}))
```

**Critical**: always use `\{}` (0 formals) for every `connect` callback. Signal arguments are **not** forwarded — the rooture shim is `static void fire()`. A lambda with formals gets partially applied (body never runs).

To read state inside a callback, query the widget directly:
```scheme
(connect sl "PositionChanged(Int_t)" (\{} {do
  (= {pos} (.GetPosition sl))
  ...}))
```

## Dynamic show/hide of rows

```scheme
;;; Add all rows unconditionally, then hide/show AFTER MapRaised
(def {row-a} (new TGHorizontalFrame win 0 0))
(.AddFrame win row-a (new TGLayoutHints 65 5 5 2 2))

(doto win {MapSubwindows} {Resize (.GetDefaultWidth win) (.GetDefaultHeight win)} {MapRaised})

;;; Apply initial visibility AFTER mapping — HideFrame before MapSubwindows is a no-op
(.HideFrame win row-a)
(.Layout win)
```

## Hello World example

See [hello-world.rut](hello-world.rut).

## Pitfalls

- `gClient` must be loaded after `libGui` — it's null before.
- `"Label"` in `new TGTextButton` must be a quoted string.
- `AddFrame` returns void — call before `MapSubwindows`.
- `def` (not `=`) to update globals from callbacks.
- `HideFrame` before `MapSubwindows` is silently ignored — apply after `MapRaised`.
- `kLHintsRight` in a horizontal frame packs from the right (reverse order). Use `kLHintsLeft` (1).
- Initialise labels with placeholder text — zero-width label stays zero-width after `SetText`.
