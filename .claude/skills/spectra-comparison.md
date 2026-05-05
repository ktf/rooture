# Skill: spectra-comparison

`examples/spectra_tpc.rut` is the **primary end-to-end demonstrator** for
rooture: it reads ALICE Run 2 AO2D data, applies global track quality cuts
(ITS, TPC χ², crossed rows ≥ 70 and ≥ 0.8 × findable), propagates tracks to
the primary vertex (DCA), performs TPC PID via the ALEPH Bethe-Bloch
parameterisation (`examples/pid_tpc.rut`), and fills per-species pT spectra.

## Quick setup (Run 2 AO2D files)

```scheme
(def {aod-paths} (list "/tmp/AO2D_021.root" "/tmp/AO2D_030.root"))
(def {trk-tree}   "O2track")
(def {extra-tree} "O2trackextra")
(def {coll-tree}  "O2collision")
(def {bc-tree}    "O2bc")
(load "examples/spectra_tpc.rut")
```

The canvas `c_spectra` appears automatically.  Symbols bound after loading:
`h-pt`, `h-p`, `h-pt-species`, `h-p-species`, `h-dcaxy`, `h-dcaz`, `h-vz`,
`tf-pairs`, and all pid/jit-fn helpers (`pid-masks`, `bb0`–`bb4`, etc.).

---

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
- `range` is **not** in stdlib — use `dotimes {i} (len tf-pairs) {...}` for index loops
- `col-fill-h2` argument order: x-column first, then y-column

---

## PID plot (dE/dx vs p_inner with Bethe-Bloch curves)

After loading `spectra_tpc.rut`, fill a TH2F from each timeframe and overlay
BB curves. The math builtins `pow`, `log`, `sqrt` work in regular rooture
lambdas (not just `jit-fn`), so BB can be computed in a `dotimes` loop.

```scheme
;;; ── Fill 2D histogram ───────────────────────────────────────────────────
(def {h2}
  (doto (new TH2F "h2pid" ";p_{inner} (GeV/c);TPC dE/dx (a.u.)" 400 0. 5. 400 0. 300.)
    {SetDirectory 0}))

(dotimes {i} (len tf-pairs) {
  do
  (= {pr}        (nth i tf-pairs))
  (= {aod-path}  (nth 0 pr))
  (= {tf}        (nth 1 pr))
  (= {sig-c}     (load-branch aod-path (concat tf "/" extra-tree) "fTPCSignal"))
  (= {pinner-c}  (load-branch aod-path (concat tf "/" extra-tree) "fTPCInnerParam"))
  (= {find-c}    (load-branch aod-path (concat tf "/" extra-tree) "fTPCNClsFindable"))
  (= {fmin-c}    (load-branch aod-path (concat tf "/" extra-tree) "fTPCNClsFindableMinusFound"))
  (= {fcrows-c}  (load-branch aod-path (concat tf "/" extra-tree) "fTPCNClsFindableMinusCrossedRows"))
  (= {itsmap-c}  (load-branch aod-path (concat tf "/" extra-tree) "fITSClusterMap"))
  (= {itschi2-c} (load-branch aod-path (concat tf "/" extra-tree) "fITSChi2NCl"))
  (= {tpcchi2-c} (load-branch aod-path (concat tf "/" extra-tree) "fTPCChi2NCl"))
  (= {tgl-c}     (load-branch aod-path (concat tf "/" trk-tree) "fTgl"))
  (= {q1pt-c}    (load-branch aod-path (concat tf "/" trk-tree) "fSigned1Pt"))
  (= {cidx-c}    (load-branch aod-path (concat tf "/" trk-tree) "fIndexCollisions"))
  (= {pvzc-c}    (load-branch aod-path (concat tf "/" coll-tree) "fPosZ"))
  (= {evsel-cols} (compute-evsel-tf aod-path tf))
  (= {evsel-col}  (evsel-and evsel-cols))
  (= {m-ev}      (col-gather evsel-col cidx-c))
  (= {ncls-c}    (col-zip-ptr sub-ptr (col-cast-f32 find-c) (col-cast-f32 fmin-c)))
  (= {ncrows-c}  (col-zip-ptr sub-ptr (col-cast-f32 find-c) (col-cast-f32 fcrows-c)))
  (= {m-eta}     (col-map-ptr eta-ok-ptr tgl-c))
  (= {m-cls}     (col-map-ptr cls-ok-ptr ncls-c))
  (= {m-its}     (col-zip-ptr its-ok-ptr (col-cast-f32 itsmap-c) (col-cast-f32 itschi2-c)))
  (= {m-tpcq}    (col-map-ptr tpcchi2-ok-ptr (col-cast-f32 tpcchi2-c)))
  (= {m-crows}   (col-zip-ptr crows-ok-ptr ncrows-c (col-cast-f32 find-c)))
  (= {mask}      (col-zip-ptr mul-ptr m-ev
                   (col-zip-ptr mul-ptr m-eta
                     (col-zip-ptr mul-ptr m-cls
                       (col-zip-ptr mul-ptr m-its
                         (col-zip-ptr mul-ptr m-tpcq m-crows))))))
  (col-fill-h2 h2 (col-mask mask pinner-c) (col-mask mask sig-c))
})

;;; ── Draw with BB curves ─────────────────────────────────────────────────
(def {cpid} (doto (new TCanvas "cpid" "TPC PID" 900 700) {SetLogz}))
(doto h2 {SetStats 0} {Draw "COLZ"})

;;; BB curve helper — uses pow/log/sqrt builtins (available since math-builtins build)
(def {bb-curve} (\{mass color npts} {
  do
  (= {gr} (doto (new TGraph npts) {SetLineColor color} {SetLineWidth 2}))
  (= {dp} (/ 4.9 npts))   ;;; scan p from 0.1 to 5.0 GeV/c
  (dotimes {i} npts {
    do
    (= {p}    (+ 0.1 (* i dp)))
    (= {bg}   (/ p mass))
    (= {beta} (/ bg (sqrt (+ 1.0 (* bg bg)))))
    (= {aa}   (pow beta bb3))
    (= {lnt}  (log (+ bb2 (pow (/ 1.0 bg) bb4))))
    (= {ex}   (/ (* bb0 (- (- bb1 aa) lnt)) aa))
    (.SetPoint gr i p ex)
  })
  gr
}))

(map (\{gr} {.Draw gr "L SAME"})
     (list (bb-curve pid-m-el 2 200)
           (bb-curve pid-m-mu 3 200)
           (bb-curve pid-m-pi 4 200)
           (bb-curve pid-m-ka 6 200)
           (bb-curve pid-m-pr 7 200)
           (bb-curve pid-m-de 8 200)))
(.Update cpid)
;;; then: get_canvas name="cpid"
```

Notes:
- `pinner-c` = `fTPCInnerParam` — momentum at TPC inner wall, used for BB evaluation
- `p-c` = total momentum, used only for the `p > p-min-pid` validity cut in `pid-masks`
- `bb0`–`bb4` are already bound after `pid_tpc.rut` loads
