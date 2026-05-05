---
description: Running spectra_tpc.rut and comparing output to AnalysisResults.root reference. Use when the user asks for spectra, a spectra comparison plot, a PID dE/dx plot, or wants to validate analysis output against the reference task.
---

# spectra-comparison

`examples/spectra_tpc.rut` reads ALICE AO2D data, applies global track quality cuts,
propagates tracks to the DCA, performs TPC+TOF PID via ALEPH Bethe-Bloch, and fills
per-species pT spectra. After loading it defines the symbols below.

## Quick setup

```scheme
;;; Run 3 defaults (use "O2track" etc. for Run 2 converted files)
(def {aod-paths} (list "/path/to/AO2D.root"))
(load "examples/spectra_tpc.rut")
```

Canvas `c_spectra` appears automatically with 5 pads: pT, p, vertex Z, DCA_XY, DCA_Z.

## Symbols defined after loading

| Symbol | Description |
|--------|-------------|
| `h-pt`, `h-p` | Inclusive pT / p spectra |
| `h-pt-species` | List of 6 TH1F `{El Mu Pi Ka Pr De}` — index 2=π, 3=K, 4=p |
| `h-p-species` | Same, momentum |
| `h-dcaxy`, `h-dcaz`, `h-vz` | QA histograms |
| `tf-pairs` | List of `{file tf-name}` pairs used for iteration |
| `bb0`–`bb4`, `pid-masks` | BB params and PID function |

## Reference file: AnalysisResults.root

Path: `/tmp/AnalysisResults.root`

Navigation:
```scheme
(= {h} (. Get f "tpc-spectra-tiny/pt/Pi"))  ; TFile::Get with slash path
```

**Do not use `TList::At(i)` on key lists** — use `First`/`After`:
```scheme
(= {k} (. First (. GetListOfKeys dir)))
(= {k} (. After (. GetListOfKeys dir) k))
```

## Comparison snippet

See [comparison.rut](comparison.rut) — normalized shape overlay, 3-pad canvas.
Then `get_canvas name="c_cmp"` to view.

## PID plot snippet

See [pid-plot.rut](pid-plot.rut) — 2D dE/dx vs p_inner histogram with BB curves overlaid.

Notes:
- `pinner-c` = `fTPCInnerParam` — momentum at inner TPC wall, for BB evaluation
- `p-c` = total momentum, for the `p > p-min-pid` validity cut only
- `bb0`–`bb4` are already bound after `pid_tpc.rut` loads

## Gotchas

- `SetRangeUser` is on `TAxis`, not `TH1`: `(. SetRangeUser (. GetXaxis h) lo hi)`
- Clone before scaling: `(. Clone h "new_name")`
- Colors per convention: π=4(blue), K=6(magenta), p=7(cyan) — see `plot-conventions`
