# Skill: spectra-comparison

How to compare `spectra_tpc.rut` output against the reference `AnalysisResults.root`.

## Reference file

Path: `/tmp/AnalysisResults.root`

Structure:
- `tpc-spectra-tiny/pt/{El,Mu,Pi,Ka,Pr,De,Tr,He,Al}` — per-species pT spectra (TH1F)
- `tpc-spectra-tiny/p/{...}`  — momentum spectra
- `tpc-spectra-tiny/event/`  — event-level QA
- `tpc-spectra-tiny/evsel`, `tpc-spectra-tiny/tracksel` — selection counters

## Our histograms (after loading spectra_tpc.rut)

`(load "examples/spectra_tpc.rut")` defines:
- `h-pt-species` — list of 6 TH1F, order `{El Mu Pi Ka Pr De}`, index 0–5
- `h-p-species`  — same order, momentum spectra
- `h-pt`         — inclusive pT
- `h-dcaxy`, `h-dcaz`, `h-vz` — QA histograms

So `(nth 2 h-pt-species)` = π, `(nth 3 h-pt-species)` = K, `(nth 4 h-pt-species)` = p.

## Navigating TFile keys

`TList::At(i)` is unreliable on key lists — use `First`/`After` instead:
```scheme
(= {k} (. First (. GetListOfKeys dir)))
(= {k} (. After (. GetListOfKeys dir) k))  ; repeat for each subsequent key
```

`TFile::Get` with a slash-separated path works for nested directories:
```scheme
(= {h} (. Get f "tpc-spectra-tiny/pt/Pi"))
```

## Comparison snippet

Normalized shape overlay, 3-pad canvas:

```scheme
(do
  (= {f}        (new TFile "/tmp/AnalysisResults.root" "READ"))
  (= {h-pi-ref} (. Get f "tpc-spectra-tiny/pt/Pi"))
  (= {h-ka-ref} (. Get f "tpc-spectra-tiny/pt/Ka"))
  (= {h-pr-ref} (. Get f "tpc-spectra-tiny/pt/Pr"))
  (= {h-pi-us}  (nth 2 h-pt-species))
  (= {h-ka-us}  (nth 3 h-pt-species))
  (= {h-pr-us}  (nth 4 h-pt-species))

  (def {norm} (\ {h nm} {
    do (= {hc} (. Clone h nm))
    (= {i} (. Integral hc))
    (if (> i 0) {. Scale hc (/ 1.0 i)} {})
    hc}))

  (= {rpi} (norm h-pi-ref "rpi")) (= {upi} (norm h-pi-us "upi"))
  (= {rka} (norm h-ka-ref "rka")) (= {uka} (norm h-ka-us "uka"))
  (= {rpr} (norm h-pr-ref "rpr")) (= {upr} (norm h-pr-us "upr"))

  (= {c} (new TCanvas "c_cmp" "Spectra comparison (normalized)" 1400 480))
  (. Divide c 3 1)

  (def {draw-pad} (\ {pad href hus col title} {
    do
    (. cd c pad)
    (doto href {SetLineColor 1} {SetLineWidth 2} {SetLineStyle 2} {SetTitle title} {SetMinimum 0})
    (. SetRangeUser (. GetXaxis href) 0.1 4.0)
    (. Draw href "HIST")
    (doto hus {SetLineColor col} {SetLineWidth 2})
    (. Draw hus "HIST SAME")}))

  (draw-pad 1 rpi upi 2 "#pi  (dashed=ref, solid=ours)")
  (draw-pad 2 rka uka 4 "K")
  (draw-pad 3 rpr upr 8 "p")
  (. Update c))
```

Then `get_canvas name="c_cmp"` to view.

## Gotchas

- `SetRangeUser` is on `TAxis`, not `TH1`: `(. SetRangeUser (. GetXaxis h) lo hi)`
- Clone before scaling so the originals stay intact: `(. Clone h "new_name")`
- `dotimes` with a variable count (not a literal) can fail — use a literal or `map` over a list instead
