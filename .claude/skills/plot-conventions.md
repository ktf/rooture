# plot-conventions

Use this skill when creating any per-species plot to ensure colors are consistent.

## Species color assignments

Always use these ROOT color indices for particle species. They must be **consistent
across all plots** — PID dE/dx curves, spectra histograms, comparison overlays, etc.

| Species | Index in lists | ROOT color | Name  |
|---------|---------------|------------|-------|
| e       | 0             | 2          | red   |
| μ       | 1             | 3          | green |
| π       | 2             | 4          | blue  |
| K       | 3             | 6          | magenta |
| p       | 4             | 7          | cyan  |
| d       | 5             | 8          | dark green |

These match the BB-curve colors used in the PID dE/dx plot (`examples/pid_plot.rut`)
and must also be used in spectra comparison overlays, legend entries, and any other
per-species histogram.

### Usage in spectra comparison

```scheme
;;; draw-pad arguments: pad href hus color title
(draw-pad 1 rpi upi 4 "#pi  (dashed=ref, solid=ours)")  ; blue
(draw-pad 2 rka uka 6 "K")                               ; magenta
(draw-pad 3 rpr upr 7 "p")                               ; cyan
```

### Usage in BB-curve overlay

```scheme
(map (\ {gr} {.Draw gr "L SAME"})
     (list (bb-curve pid-m-el 2 200)   ; red
           (bb-curve pid-m-mu 3 200)   ; green
           (bb-curve pid-m-pi 4 200)   ; blue
           (bb-curve pid-m-ka 6 200)   ; magenta
           (bb-curve pid-m-pr 7 200)   ; cyan
           (bb-curve pid-m-de 8 200))) ; dark green
```

## General ROOT style conventions

- **Line width**: 2 for all data/fit curves
- **Reference histograms**: `SetLineStyle 2` (dashed), color 1 (black)
- **Log z-axis** for 2D histograms with large dynamic range: `{SetLogz}`
- **Stats box**: suppress with `{SetStats 0}` on 2D histograms; leave on for 1D QA
- **Axis ranges**: use `(. SetRangeUser (. GetXaxis h) lo hi)` — `SetRangeUser` is on
  `TAxis`, not `TH1`
- **Clone before scaling**: always `(. Clone h "new_name")` before `Scale` to preserve originals
