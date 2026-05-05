---
description: Setting up a full ALICE physics analysis in rooture from scratch. Use when the user wants to read AO2D data, apply event selection, track quality cuts, DCA propagation, TPC+TOF PID, parallel processing, or histogram filling.
---

# rooture-analysis-basics

Use this skill whenever setting up a physics analysis in rooture from scratch:
reading AO2D data, event selection, track quality cuts, track propagation to DCA,
TPC PID via Bethe-Bloch, TOF extension, CCDB parameter fetch, parallel processing,
and histogram filling.

---

## 1. Script structure

A typical analysis script follows this order:

```
(load stdlib.rut)

;;; 1. Guard: check required user inputs
;;; 2. Set tree-name defaults
;;; 3. Set event-selection parameters → (load examples/eventsel.rut)
;;; 4. Set BB parameter overrides → (load examples/pid_tpc.rut)
;;; 5. Enumerate timeframes across all input files
;;; 6. Define jit-fns (track quality, kinematic, PID)
;;; 7. Pre-create histograms on main thread (one per TF + merged globals)
;;; 8. pmap over timeframe indices
;;; 9. Merge per-TF histograms
;;; 10. Draw
```

---

## 2. Input files and tree names

Users supply a list of AO2D.root paths as `aod-paths`. Always guard against the
symbol being unset (rooture auto-converts undefined symbols to strings):

```scheme
(if (== aod-paths "aod-paths") {
  do
  (print "ERROR: aod-paths must be defined before loading this script.")
  (def {aod-paths} {})
} {})
```

### Run 3 defaults (O2 online pass)

```scheme
(default {trk-tree}       "O2track_iu")
(default {extra-tree}     "O2trackextra_002")
(default {coll-tree}      "O2collision_001")
(default {bc-tree}        "O2bc_001")
(default {its-map-branch} "fITSClusterMap")   ; or "fITSClusterSizes" for Open Data
```

### Run 2 converted files

```scheme
(def {trk-tree}   "O2track")
(def {extra-tree} "O2trackextra")
(def {coll-tree}  "O2collision")
(def {bc-tree}    "O2bc")
```

### Enumerating timeframes

Each AO2D.root file has one `TDirectoryFile` per time frame named `DF_<timestamp>`.
Build a list of `{file-path tf-name}` pairs across all input files:

```scheme
(def {tf-pairs} (do
  (= {acc} {})
  (dotimes {pi} (len aod-paths) {
    do
    (= {path} (nth pi aod-paths))
    (= {keys} (.GetListOfKeys (::Open TFile path)))
    (dotimes {i} (.GetEntries keys) {
      do
      (= {k} (.At keys i))
      (if (== (.GetClassName k) "TDirectoryFile")
        {= {acc} (join acc (list (list path (.GetName k))))}
        {})
    })
  })
  acc))
(print "Timeframes:" (len tf-pairs))
```

Inside the pmap worker, load branches as:
```scheme
(= {pr}      (nth i tf-pairs))
(= {aod-path} (nth 0 pr))
(= {tf}       (nth 1 pr))
(load-branch aod-path (concat tf "/" trk-tree) "fX")
```

---

## 3. Loading branches

`load-branch` reads an entire TTree branch into a flat in-memory float32/int column.

```scheme
(load-branch aod-path tree-path branch-name)
; → col pointer (float32 unless branch is ULong64_t/int, in which case typed)
```

`tree-path` is always `"DF_<timestamp>/O2track_iu"` etc. — the TF directory name
followed by the tree name, joined with `/`.

Type coercion before arithmetic: use `col-cast-f32` on integer columns:
```scheme
(= {ncls-c} (col-zip-ptr sub-ptr (col-cast-f32 find-c) (col-cast-f32 fmin-c)))
```

### Key branches

**`O2track_iu`** — track inner-update helix parameters:

| Branch | Type | Description |
|--------|------|-------------|
| `fAlpha` | float | sector angle (rad) |
| `fX`, `fY`, `fZ` | float | local position |
| `fSnp` | float | sin(φ_local) |
| `fTgl` | float | tan(λ) = p_z/p_T |
| `fSigned1Pt` | float | q/p_T |
| `fIndexCollisions` | int | collision index for this track |

**`O2trackextra_002`** — track quality and PID:

| Branch | Description |
|--------|-------------|
| `fTPCNClsFindable` | findable TPC clusters |
| `fTPCNClsFindableMinusFound` | findable − found |
| `fTPCNClsFindableMinusCrossedRows` | for crossed-rows cut |
| `fTPCSignal` | TPC dE/dx in a.u. |
| `fTPCInnerParam` | momentum at inner TPC wall (for BB evaluation) |
| `fTPCChi2NCl` | TPC χ²/cluster |
| `fITSClusterMap` / `fITSClusterSizes` | ITS hit pattern |
| `fITSChi2NCl` | ITS χ²/cluster |
| `fTOFChi2` | TOF χ² (< 0 = no TOF hit) |
| `fTOFExpMom` | momentum at TOF surface (for t_exp) |
| `fLength` | track length (cm) |
| `fTrackTime` | measured TOF time (ns) |
| `fTrackTimeRes` | TOF time resolution (ns) |

**`O2collision_001`** — primary vertex:

| Branch | Description |
|--------|-------------|
| `fPosX`, `fPosY`, `fPosZ` | vertex position |
| `fIndexBCs` | index into BC table |

**`O2bc_001`** — bunch-crossing:

| Branch | Description |
|--------|-------------|
| `fGlobalBC` | global bunch-crossing number |
| `fTriggerMask` | trigger bits |

---

## 4. Event selection

Load `examples/eventsel.rut` after setting the run parameters. It defines
`compute-evsel-tf` and `evsel-and`.

```scheme
;;; Parameters — constant-folded into jit-fns at load time
(def {bcSOR}     0)        ; first globalBC of run; 0 = skip TF/ROF border checks
(def {nBCsPerTF} 456192)   ; 128 * 3564 (Run 3)
(def {rofLength} 594)      ; ITS ROF length in BCs (Run 3)
(def {rofOffset} 0)
(def {tvx-bit}   -1)       ; -1 = skip TVX check; 0 for most Run 3 data
(def {pvz-cut}   10.0)     ; |pvZ| < pvz-cut cm
(load examples/eventsel.rut)
```

Per time frame, compute a per-collision float32 mask:

```scheme
(= {evsel-cols} (compute-evsel-tf aod-path tf))
; returns list of 5 columns: {m-tvx m-tf m-itsrof m-pvz m-nopileup}

(= {evsel-col}  (evsel-and evsel-cols))   ; AND of all 5 flags
```

To use sel8 only (TVX + TF border + ITS ROF + no-pileup, without pvZ):
```scheme
(= {evsel-col}  (evsel-sel8 evsel-cols))
```

Gather to per-track using `fIndexCollisions`:
```scheme
(= {m-ev} (col-gather evsel-col cidx-c))
```

---

## 5. Track quality cuts

All track-quality functions are `jit-fn float` returning 1.0 (pass) or 0.0 (fail).
Define them once at script load time so JIT compilation happens before pmap launches.

### Kinematic cuts

```scheme
;;; η = asinh(tgl); |η| < 0.8
(def {eta-ok-fn}
  (jit-fn float (\{{float tgl}}
    {(if (< (fabsf (asinhf tgl)) 0.8f) {1.0f} {0.0f})})))

;;; pT = 1 / |q1Pt|
(def {pt-fn}
  (jit-fn float (\{{float s1pt}}
    {(if (== s1pt 0.0f) {0.0f} {(/ 1.0f (fabsf s1pt))})})))

;;; p = sqrt(1 + tgl²) / |q/pT|
(def {p-fn}
  (jit-fn float (\{{float s1pt} {float tgl}}
    {(if (== s1pt 0.0f) {0.0f}
      {(/ (sqrtf (+ 1.0f (* tgl tgl))) (fabsf s1pt))})})))
```

### TPC cluster cuts (isGlobalTrack)

```scheme
;;; nCls = fTPCNClsFindable - fTPCNClsFindableMinusFound  (compute first)
(def {min-cls-box} (new TArrayI 1))   ; mutable box — lets you change threshold at runtime
(.SetAt min-cls-box 70 0)

(def {cls-ok-fn}
  (jit-fn float (\{{float ncls}}
    {(if (>= ncls (.GetAt min-cls-box 0)) {1.0f} {0.0f})})))

;;; nCrossedRows >= 70, crossed/findable >= 0.8
(def {crows-ok-fn}
  (jit-fn float (\{{float ncrows} {float nfind}}
    {(if (>= ncrows 70.0f)
      {(if (> nfind 0.5f)
        {(if (>= (/ ncrows nfind) 0.8f) {1.0f} {0.0f})}
        {0.0f})}
      {0.0f})})))

;;; TPC chi2/cluster < 4
(def {tpcchi2-ok-fn}
  (jit-fn float (\{{float chi2}}
    {(if (< chi2 4.0f) {1.0f} {0.0f})})))
```

### ITS cuts (isGlobalTrack)

```scheme
;;; ITS: >= 1 cluster AND ITS chi2/cluster < 36
;;; fITSClusterMap and fITSClusterSizes: nonzero ↔ has ITS hit
(def {its-ok-fn}
  (jit-fn float (\{{float itsmap} {float itschi2}}
    {(if (> itsmap 0.5f)
      {(if (< itschi2 36.0f) {1.0f} {0.0f})}
      {0.0f})})))
```

### DCA cut (isGlobalTrack — pT-parameterized)

**Critical**: use the tight pT-parameterized form. A flat cut (e.g. 2.4 cm) passes
Λ→pπ secondaries, producing a spurious low-pT proton peak.

```scheme
;;; |dcaXY| < 0.0105 + 0.035 / pT^1.1   (O2 isGlobalTrack definition)
;;; |dcaZ|  < 2.0 cm
(def {dca-ok-fn}
  (jit-fn float (\{{float dxy} {float dz} {float pt}}
    {(if (< (fabsf dxy) (+ 0.0105f (/ 0.0350f (powf (fabsf pt) 1.1f))))
      {(if (< (fabsf dz) 2.0f) {1.0f} {0.0f})}
      {0.0f})})))
```

### Resolve function pointers (before pmap)

```scheme
(def {eta-ok-ptr}     (jitfn-ptr eta-ok-fn))
(def {pt-ptr}         (jitfn-ptr pt-fn))
(def {p-ptr}          (jitfn-ptr p-fn))
(def {cls-ok-ptr}     (jitfn-ptr cls-ok-fn))
(def {crows-ok-ptr}   (jitfn-ptr crows-ok-fn))
(def {tpcchi2-ok-ptr} (jitfn-ptr tpcchi2-ok-fn))
(def {its-ok-ptr}     (jitfn-ptr its-ok-fn))
(def {dca-ok-ptr}     (jitfn-ptr dca-ok-fn))
(def {mul-ptr}        (jitfn-ptr (jit-fn float (\{{float a} {float b}} {(* a b)}))))
(def {sub-ptr}        (jitfn-ptr (jit-fn float (\{{float a} {float b}} {(- a b)}))))
```

### Combining into a mask

```scheme
;;; derived columns
(= {ncls-c}   (col-zip-ptr sub-ptr (col-cast-f32 find-c) (col-cast-f32 fmin-c)))
(= {ncrows-c} (col-zip-ptr sub-ptr (col-cast-f32 find-c) (col-cast-f32 fcrows-c)))
(= {pt-c}     (col-map-ptr pt-ptr q1pt-c))
(= {p-c}      (col-zip-ptr p-ptr q1pt-c tgl-c))

;;; per-track quality masks
(= {m-ev}    (col-gather evsel-col cidx-c))   ; event selection
(= {m-eta}   (col-map-ptr eta-ok-ptr tgl-c))
(= {m-cls}   (col-map-ptr cls-ok-ptr ncls-c))
(= {m-crows} (col-zip-ptr crows-ok-ptr ncrows-c (col-cast-f32 find-c)))
(= {m-tpcq}  (col-map-ptr tpcchi2-ok-ptr (col-cast-f32 tpcchi2-c)))
(= {m-its}   (col-zip-ptr its-ok-ptr (col-cast-f32 itsmap-c) (col-cast-f32 itschi2-c)))
(= {m-dca}   (col-zip-ptr dca-ok-ptr dcaxy-c dcaz-c pt-c))

;;; combined — order doesn't matter; all are AND'd
(= {mask} (col-zip-ptr mul-ptr m-ev
            (col-zip-ptr mul-ptr m-eta
              (col-zip-ptr mul-ptr m-cls
                (col-zip-ptr mul-ptr m-dca
                  (col-zip-ptr mul-ptr m-its
                    (col-zip-ptr mul-ptr m-tpcq m-crows)))))))
```

---

## 6. Track propagation to DCA

`propagate-to-dca` (defined in `examples/spectra_tpc.rut`) computes dcaXY and dcaZ
in one pass by inlining `TrackParametrization::rotateParam` + `propagateParamTo`.

Requires per-track primary vertex coordinates (gathered from collision table):

```scheme
;;; gather per-track PV coordinates
(= {cidx-c} (load-branch aod-path (concat tf "/" trk-tree) "fIndexCollisions"))
(= {pvxc-c} (load-branch aod-path (concat tf "/" coll-tree) "fPosX"))
(= {pvyc-c} (load-branch aod-path (concat tf "/" coll-tree) "fPosY"))
(= {pvzc-c} (load-branch aod-path (concat tf "/" coll-tree) "fPosZ"))
(= {pvX-c}  (col-gather pvxc-c cidx-c))
(= {pvY-c}  (col-gather pvyc-c cidx-c))
(= {pvZ-c}  (col-gather pvzc-c cidx-c))

;;; propagate
(= {dca}    (propagate-to-dca
               fx-c fy-c fz-c snp-c tgl-c q1pt-c alp-c
               pvX-c pvY-c pvZ-c))
(= {dcaxy-c} (nth 0 dca))
(= {dcaz-c}  (nth 1 dca))
```

The function is defined in `spectra_tpc.rut` using:
```scheme
(def {bz} 0.5)   ; solenoid field in Tesla — must be set before loading
```

---

## 7. TPC PID via Bethe-Bloch

Load `examples/pid_tpc.rut`. Override BB parameters and cuts first if needed.

```scheme
;;; BB shape defaults (O2Physics ALEPH, MIP(pion) ~ 50 a.u.):
(default {bb0}        1.60491)
(default {bb1}        19.9768)
(default {bb2}        2.52666e-16)
(default {bb3}        2.72123)
(default {bb4}        6.08092)
(default {bb-res}     0.07)     ; relative resolution 7%
(default {nsigma-max} 3.0)      ; |nσ| cut
(default {p-min-pid}  0.1)      ; min p (GeV/c) — below this TPC PID is unreliable
(default {sig-min}    20.0)     ; min TPC signal — near-zero = failed extraction
(load examples/pid_tpc.rut)
```

Per time frame, call `pid-masks`:

```scheme
;;; returns list of 6 float32 masks: {el mu pi ka pr de}
(= {pmasks} (pid-masks sig-c p-c pinner-c tofchi2-c tofp-c length-c time-c timeres-c))
```

Combine with the quality mask:
```scheme
(= {msk-pi} (col-zip-ptr mul-ptr mask (nth 2 pmasks)))
(= {msk-pr} (col-zip-ptr mul-ptr mask (nth 4 pmasks)))
```

Species index: **0=El, 1=Mu, 2=Pi, 3=Ka, 4=Pr, 5=De**.

### ALEPH Bethe-Bloch formula

```
dE/dx = bb0/beta^bb3 * (bb1 - beta^bb3 - ln(bb2 + 1/beta_gamma^bb4))
```

`pinner-c` (`fTPCInnerParam`) is the momentum at the inner TPC wall — always use
this for BB evaluation, not `pt-c`. `p-c` (total momentum) is used only for the
validity cut (`p > p-min-pid`).

### TOF extension

`pid_tpc.rut` includes TOF automatically. For tracks without a TOF hit
(`fTOFChi2 < 0`), the TOF factor passes by default — the combined mask reduces to
TPC-only.

**Units**: `fTrackTime` and `fTrackTimeRes` are in **nanoseconds**.
Speed of light: `tof-c = 29.9792458 cm/ns`.

Expected TOF time: `t_exp [ns] = fLength [cm] / (beta_exp × tof-c)`

---

## 8. CCDB BB parameters

Override the built-in defaults with CCDB-fitted parameters for the specific run.

```scheme
;;; Set these before loading spectra_tpc.rut (which loads pid_tpc.rut)
(def {ccdb-pid-ts}      1448150400000)   ; Unix timestamp in ms for the run
(def {ccdb-pid-url}     "http://localhost:8888")   ; CCDB proxy
(def {ccdb-pid-headers} (list "Authorization: Bearer <token>"))  ; if auth needed
(load "examples/spectra_tpc.rut")
```

`spectra_tpc.rut` handles the fetch automatically when `ccdb-pid-ts > 0`:

```scheme
;;; What spectra_tpc.rut does internally:
(= {_bb-url} (concat ccdb-pid-url "/Analysis/PID/TPC/BetheBloch/" (str ccdb-pid-ts)))
(= {_bb-f}   (deref (fetch-url _bb-url ccdb-pid-headers)))
(= {_bb-obj} (tfile-get _bb-f "ccdb_object"))
;;; mPar = {kp1 kp2 kp3 kp4 kp5 mMIP resolution}
;;; Note: bb0 = mMIP * kp1 (pid_tpc.rut absorbs mMIP into bb0)
(= {_par} (tobj-member _bb-obj "mPar"))
(def {bb0} (* (nth 5 _par) (nth 0 _par)))
(def {bb1} (nth 1 _par))
...
```

`fetch-url` is async — it returns a `future`. The `deref` waits for the download.
It handles 303 redirects from CCDB and rewrites URLs through the proxy if needed.

---

## 9. Parallel processing with pmap

**Rule**: create all ROOT objects (histograms) on the main thread BEFORE `pmap`.
Never call `new TH1F` or ROOT method calls that trigger Cling JIT inside pmap
workers — LLVM ORC JIT is not safe when called from concurrent threads.

### Pattern: pre-create per-TF histograms

```scheme
;;; one histogram per TF, pre-created on main thread
(def {tf-hpt} (map (\ {pr} {
  doto (new TH1F (concat "pt_" (nth 1 pr)) "" 100 0. 20.)
    {SetDirectory 0}
}) tf-pairs))

;;; global merged histograms
(def {h-pt} (doto (new TH1F "hpt" ";p_{T} (GeV/c);Tracks" 100 0. 20.) {SetDirectory 0}))
```

### Pattern: pmap over TF indices

```scheme
(pmap (\ {i} {
  do
  (= {pr}   (nth i tf-pairs))
  (= {hpt}  (nth i tf-hpt))
  ;;; ... load branches, compute masks ...
  (col-fill-h1 hpt (col-mask mask pt-c))
}) (do
  (= {acc} {})
  (dotimes {i} (len tf-pairs) {= {acc} (join acc (list i))})
  acc))
```

### Pattern: merge after pmap

```scheme
(map (\ {h} {.Add h-pt h}) tf-hpt)
```

### Operations safe inside pmap workers

`load-branch`, `col-map-ptr`, `col-filter-ptr`, `col-zip-ptr`, `col-reduce-ptr`,
`col-gather`, `col-cast-f32`, `col-mask`, `col-fill-h1`, `col-unique-mask`,
`col-test-bit`, arithmetic, string ops.

**Unsafe inside pmap**: `new`, `.Method` calls, any `gInterpreter->Calc` path.

---

## 10. Histogram filling

```scheme
;;; fill with selected column (mask = 1.0 → include, 0.0 → skip)
(col-fill-h1 h (col-mask mask pt-c))

;;; fill vertex Z (collision-level, not track-level)
(col-fill-h1 hvz (col-mask evsel-col pvzc-c))
```

Set `{SetDirectory 0}` on all histograms so they don't auto-delete when TFiles close.

---

## 11. Full minimal example

See [minimal-example.rut](minimal-example.rut) for a complete runnable skeleton:
event selection + η cut + pT spectrum, one pmap loop, merge and draw.

---

## 12. Common pitfalls

| Symptom | Cause | Fix |
|---------|-------|-----|
| Spurious low-pT proton peak | Loose flat DCA cut passes Λ→pπ secondaries | Use pT-parameterized cut: `\|dcaXY\| < 0.0105 + 0.035/pT^1.1` |
| SIGSEGV in pmap | `new` or method call triggering Cling JIT in worker thread | Move all ROOT object creation before pmap |
| "Got String, expected Object" | Undefined symbol auto-converted to string (e.g. `aod-paths` not set) | Guard with `(if (== sym "sym") ...)` or ensure def runs first |
| Wrong TOF nσ (e.g. always 0 or always fail) | `tof-c` in wrong units (ps instead of ns) | Use `tof-c = 29.9792458` cm/ns; fTrackTime is in nanoseconds |
| `m-dca` defined but not in mask | Forgot to include `m-dca` in the mask chain | Verify every quality mask is AND'd into the final mask |
| SIGSEGV from concurrent TTree load | `TTree::Streamer` touches global StreamerInfo registry | Fixed in rut_column.cxx — tree metadata dispatched to main thread |
| Cling errors from method chains | `(.GetXaxis h .GetXmin)` is wrong syntax | Two separate calls: `(. GetXmin (. GetXaxis h))` |
