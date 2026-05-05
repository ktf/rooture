---
description: Debugging ROOT/rooture GUI issues. Use when widgets are not appearing, have wrong sizes or positions, labels are not updating after SetText, or layout does not match expectations.
---

# rooture-debug-gui

Always invoke `rooture-lang` first for general language context.

---

## Step 1: screenshot the window

```
mcp__rooture__get_window { symbol: "win" }
```

Use this repeatedly after each fix to confirm changes visually.

---

## Step 2: query widget geometry

```scheme
(.GetWidth widget)         ; 0 = layout gave no space
(.GetHeight widget)
(.GetDefaultWidth widget)  ; preferred size before layout
(.GetX widget)             ; position within parent
(.GetY widget)
```

If `GetWidth` = 0: the parent layout gave the widget no space (see below).

---

## Why a widget gets zero width

In a `TGHorizontalFrame`, a child gets **zero width** when:
- The parent's allocated width is smaller than preceding children
- The child has `kLHintsLeft` (not `kLHintsExpandX`) and the parent is too small

**Key trap**: a label initialised with `""` gets zero default width. Calling `SetText` later updates the text but doesn't re-run layout — the child stays at zero width forever.

**Fix**: initialise every label with placeholder text wide enough for the expected result: `"chi2 = 0.0000  ndf = 00"`.

---

## Layout hint quick reference

**In a vertical parent** (TGMainFrame, TGVerticalFrame):

| Hint | Child width |
|------|-------------|
| Left (1) | child's default width |
| CenterX (2) | child's default width, centred |
| ExpandX (64) | stretched to fill parent |

**In a horizontal parent** (TGHorizontalFrame):

| Hint | Effect |
|------|--------|
| Left (1) | packed left, natural width |
| Left\|CenterY (17) | packed left, vertically centred |
| ExpandX (64) | takes all remaining horizontal space |
| Right (4) | packs from right edge — items appear in **reverse order** |

---

## Label text not updating

`SetText` → `Layout()` → `NeedRedraw()` queues a redraw. It flushes via `gVirtualX->Update(2)` + `Update(1)`. rooture's callback handlers call both automatically after every eval.

If labels still don't update:
1. Confirm `SetText` is reached: `(print "debug")` before it
2. Confirm label has non-zero width: `(.GetWidth lbl)`
3. Confirm no error short-circuits the callback before `SetText`

To read label text: `(.Data (.GetText lbl))`

---

## Forcing a layout refresh

```scheme
(.Layout win)        ; after content change (enough for text updates)
(.MapSubwindows win) ; only needed if new widgets were added after MapWindow
```

---

## HideFrame timing trap

`HideFrame` calls `UnmapWindow()` which is a no-op on an unmapped window. If called before `MapSubwindows`, `MapSubwindows` subsequently maps all children — overriding the hide.

**Fix**: always apply initial show/hide state **after** `MapSubwindows` + `MapRaised`.

---

## Cling identifier in process-line

rooture symbols use `-` (e.g. `my-btn`), but Cling sees them as C++ identifiers with `_` (e.g. `my_btn`). In `process-line` strings, use underscore form.

---

## Quick diagnostics table

| Symptom | Cause | Fix |
|---------|-------|-----|
| Widget invisible, GetWidth=0 | Parent too narrow or label init with "" | Use ExpandX on parent; init label with placeholder |
| Elements appear right-to-left | `kLHintsRight` (4) in horizontal frame | Use `kLHintsLeft` (1) = hint 17 |
| Label shows stale text | Has zero width | Init with same-width placeholder |
| Window appears collapsed | Resize before MapSubwindows | Call MapSubwindows → Resize → MapRaised in order |
| HideFrame has no effect | Called before MapSubwindows | Apply after MapRaised |
| Callback body never runs | Lambda has formals (partially applied) | Use `\{}` — signal args not forwarded |
