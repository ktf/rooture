# rooture-debug-gui

Use this skill when debugging ROOT/rooture GUI issues: widgets not appearing,
wrong sizes or positions, labels not updating, layout not matching expectations.

---

## Visual inspection: screenshot the window

The rooture MCP server exposes a `get_window` tool. Use it to see the actual
rendered window without relying on the user's description:

```
mcp__rooture__get_window { symbol: "win" }
```

Pass the rooture symbol that holds the `TGMainFrame` (or any `TGFrame`).
Call this repeatedly after each change to confirm the fix visually.

---

## Querying widget geometry

All `TGFrame` subclasses expose positional and size methods. Query them via MCP
eval to understand why a widget is invisible or misplaced:

```scheme
(.GetX widget)             ; position within parent
(.GetY widget)
(.GetWidth widget)         ; actual allocated size (after layout)
(.GetHeight widget)
(.GetDefaultWidth widget)  ; preferred size before layout
(.GetDefaultHeight widget)
(.GetWidth parent)         ; compare child vs parent
```

Key diagnostics:
- `GetWidth` = 0 → the parent layout gave the widget no space (see below)
- `GetDefaultWidth` ≠ `GetWidth` → the layout overrode the preferred size
- `GetX` + `GetWidth` > `GetWidth parent` → widget is clipped outside parent

---

## Why a widget gets zero width

In a `TGHorizontalFrame`, children are packed left-to-right. A child gets
**zero width** when:

1. The parent frame is narrower than the sum of all preceding children.
2. The child has `kLHintsLeft` (not `kLHintsExpandX`) and the parent's
   **allocated width** (not default width) is too small.

Key trap: a frame with hint `kLHintsCenterX` (2) in a vertical parent uses
its **default width**, which is the sum of its children's default widths.
But if a label starts with empty text `""`, its default width is 0 or very
small. Later calling `SetText` updates the text but does **not** re-run the
parent's layout — the child stays at zero width.

**Fix**: initialise the label with a placeholder text wide enough for the
expected result (e.g. `"chi2 = 0.0000  ndf = 00"`) so the layout allocates
sufficient space from the start.

---

## Layout hint behaviour

In a **vertical** parent (`TGMainFrame`, `TGVerticalFrame`):

| Hint on child | Child width |
|---|---|
| `kLHintsLeft` (1) | child's default width |
| `kLHintsCenterX` (2) | child's default width, centred |
| `kLHintsRight` (4) | child's default width, right-aligned |
| `kLHintsExpandX` (64) | stretched to fill parent width |

In a **horizontal** parent (`TGHorizontalFrame`):

| Hint on child | Effect |
|---|---|
| `kLHintsLeft` (1) | packed left, natural width |
| `kLHintsCenterY` (16) | packed left, centred vertically |
| `kLHintsCenterX` (2) | centred vertically within the row |
| `kLHintsExpandX` (64) | takes all remaining horizontal space |

`kLHintsRight` (4) in a horizontal frame packs from the **right edge**,
causing elements to appear in **reverse insertion order**. Use
`kLHintsLeft` (1) for left-to-right ordering.

Common correct combinations:

```scheme
17   ; kLHintsLeft|kLHintsCenterY  — left-packed, vertically centred
18   ; kLHintsCenterX|kLHintsCenterY  — centred both axes
65   ; kLHintsLeft|kLHintsExpandX  — full width, left content
```

---

## Forcing a layout refresh

After dynamically changing widget content or adding children:

```scheme
(.Layout win)        ; recalculate all children's positions and sizes
(.MapSubwindows win) ; only needed if new widgets were added after MapWindow
```

Do **not** call `Resize` again unless the window dimensions must change;
`Layout` alone is enough for content changes.

---

## Label text not appearing after SetText (refresh pipeline)

`TGLabel::SetText` → `Layout()` → `fClient->NeedRedraw(this)` queues a
redraw. The redraw is **not** immediate — it waits for the ROOT event loop
to call `TGClient::DoRedraw()`.

`DoRedraw()` is **protected** on `TGClient`, so it cannot be called directly
from interpreted code. The correct way to flush pending widget redraws is
through `TVirtualX::Update`:

- `gVirtualX->Update(2)` — calls `TGClient::DoRedraw()` (TGCocoa is a
  friend of TGClient, so this is allowed)
- `gVirtualX->Update(1)` — flushes the Cocoa/X11 command buffer to screen

rooture's callback handlers (`RutCallbackHandler`, `PipeHandler`) call both
automatically after every callback and MCP eval. If labels still do not
update, check that:

1. `SetText` is actually being reached (add a `(println "debug")` before it).
2. The label has non-zero allocated width (see section above).
3. No error is short-circuiting the callback before `SetText`.

---

## Debugging label content

`TGLabel::GetText` returns a `TGString*`; `TGString::Data()` returns the
`const char*`. Call via rooture:

```scheme
(.Data (.GetText lbl))
```

If this returns `{}` (empty Q-expression / empty string), `SetText` was
never called or was called with an empty string.

---

## Diagnosing layout with stderr prints

Use `process-line` to print diagnostics to stderr (visible in rooture's
terminal):

```scheme
(process-line "{ fprintf(stderr, \"w=%d h=%d x=%d y=%d\\n\",
    (int)my_widget->GetWidth(), (int)my_widget->GetHeight(),
    (int)my_widget->GetX(),    (int)my_widget->GetY()); }")
```

Note: rooture symbols use `-` (e.g. `my-btn`), but Cling sees them as C++
identifiers with `_` (e.g. `my_btn`). In `process-line` strings, use the
underscore form.

---

## Common pitfalls summary

| Symptom | Likely cause | Fix |
|---|---|---|
| Widget invisible, GetWidth=0 | Parent too narrow; child is beyond frame bounds | Use `kLHintsExpandX` on parent, or initialise label with placeholder text |
| Elements appear right-to-left | `kLHintsRight` (4) in horizontal frame | Use `kLHintsLeft` (1) = hint 17 |
| Label shows stale text after SetText | Redraw not flushed | rooture handles this via `Update(2)+Update(1)`; check the label has non-zero width |
| Window appears collapsed | `Resize` called before `MapSubwindows` | Call `MapSubwindows` → `Resize` → `MapWindow` in order |
| Fit result label never shows | Label initialised with `""`, gets 0 width | Initialise with same-width placeholder |
| `kLHintsCenterX` child is narrow | Default width computed from initial content | Pre-fill with widest expected text |
| Canvas update erases labels | Drawing order issue | rooture calls `Update(2)+Update(1)` before `c->Update()` to fix this |
