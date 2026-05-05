---
description: ROOT color and style conventions for particle species plots. Use when creating any per-species histogram, PID dE/dx curve, spectra overlay, or BB curve to ensure consistent colors across all plots.
---

# plot-conventions

## Species color assignments

Always use these ROOT color indices. Must be **consistent across all plots**.

| Species | Index in lists | ROOT color | Name |
|---------|---------------|------------|------|
| e | 0 | 2 | red |
| μ | 1 | 3 | green |
| π | 2 | 4 | blue |
| K | 3 | 6 | magenta |
| p | 4 | 7 | cyan |
| d | 5 | 8 | dark green |

```scheme
;;; spectra comparison (ours=solid, reference=dashed black)
(draw-pad 1 rpi upi 4 "#pi")   ; π = blue
(draw-pad 2 rka uka 6 "K")     ; K = magenta
(draw-pad 3 rpr upr 7 "p")     ; p = cyan

;;; BB curve overlay
(map (\{gr} {.Draw gr "L SAME"})
     (list (bb-curve pid-m-el 2 200)   ; e   = red
           (bb-curve pid-m-mu 3 200)   ; μ   = green
           (bb-curve pid-m-pi 4 200)   ; π   = blue
           (bb-curve pid-m-ka 6 200)   ; K   = magenta
           (bb-curve pid-m-pr 7 200)   ; p   = cyan
           (bb-curve pid-m-de 8 200))) ; d   = dark green
```

## General ROOT style rules

- **Line width**: 2 for all data/fit curves
- **Reference histograms**: `SetLineStyle 2` (dashed), color 1 (black)
- **Log z-axis** for 2D histograms: `{SetLogz}`
- **Stats box**: suppress with `{SetStats 0}` on 2D histograms; leave on for 1D QA
- **Axis range**: `(. SetRangeUser (. GetXaxis h) lo hi)` — `SetRangeUser` is on `TAxis`, not `TH1`
- **Clone before scaling**: `(. Clone h "new_name")` before `Scale` to preserve originals
