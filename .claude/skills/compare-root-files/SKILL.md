---
description: Generic comparison of two ROOT files in rooture — numeric bin-by-bin diff or visual OLD|NEW|DIFF PDF report. Use when the user wants to compare two ROOT/AnalysisResults files histogram-by-histogram, validate that two task outputs are identical, or produce a visual diff report of all histograms.
---

# compare-root-files

Two reusable scripts compare arbitrary histogram files. They answer different
questions — pick by what the user wants:

| | `compare_results.rut` | `compare_hists_pdf.rut` |
|---|---|---|
| Question answered | exact yes/no per object | *what* changed, visually |
| Output | console lines only | multi-page PDF + console lines |
| ≥3-dim objects | full N-dim bin buffer | 1D axis projections only |
| CorrelationContainer / StepTHn | yes (MakeProject + `field`) | no |
| Unsupported / missing objects | console "skipped" | TPaveText note row in PDF |

For thorough validation of N-dim objects run **both**: the PDF's per-axis
projections can hide compensating differences that the full-buffer numeric
check catches.

## Numeric diff — `examples/compare_results.rut`

Walks all directories recursively, compares every histogram bin-by-bin via
`as-col` + jit-fn columns (the *full* N-dim content, not projections), prints
`diffBins=n/N absdiff=X IDENTICAL/DIFFERENT` per object. Also handles
`CorrelationContainer` (StepTHn `mValues` arrays per step + THnSparse
prototype) via MakeProject dictionaries and `field`/`field-at`.

```scheme
(def {file1} "/path/to/reference.root")
(def {file2} "/path/to/candidate.root")
(load "examples/compare_results.rut")
(run)
```

## Visual diff PDF — `examples/compare_hists_pdf.rut`

Renders one row of three pads per histogram — OLD | NEW | DIFF (new−old) —
into a multi-page PDF (3 rows/page). Class dispatch is generic, via
`InheritsFrom` on the object (do not assume a fixed layout):

| Class | Rendering |
|-------|-----------|
| TH1*  | one row, line plots (diff in red) |
| TH2*  | one row, COLZ |
| TH3*  | three rows: X/Y/Z 1D projections |
| THn(Sparse) | one row per axis 1D projection |
| anything else / missing in new file | TPaveText note row in the PDF |

Diff titles carry the exact bin-level verdict (`bins=n/N max min`, computed
via `as-col` on the *drawn* histogram — i.e. on the projection for ≥3-dim
objects — including under/overflow). Console prints one IDENTICAL/DIFFERENT
line per row plus a final summary.

```scheme
(def {file-old} "/path/to/reference.root")
(def {file-new} "/path/to/candidate.root")
(def {out-pdf}  "/path/to/comparison.pdf")
(load "examples/compare_hists_pdf.rut")
(run)          ; use eval_async for large files
```

## Gotchas

- Clone histograms with `{SetDirectory 0}` before modifying them, so they
  detach from the source file and don't clash with same-named originals.
- Never bind a variable whose name equals a method called via `.name` sugar
  (a local `cd` breaks `(.cd cnv 1)`) — see `rooture-quirks`.
- For spectra-specific validation against the reference task, see
  `spectra-comparison`.
