---
description: Reading ALICE Run 3 AO2D data in rooture. Use when the user asks about AO2D file structure, TChain setup, branch names, TDirectoryFile iteration, or RDataFrame from O2 data.
---

# alice-aod

Use this skill when the user wants to read or analyse ALICE Run 3 AO2D data in rooture.

---

## File structure

An AO2D.root file contains one `TMap` named `metaData` and many `TDirectoryFile` entries
named `DF_<timestamp>` — one per time frame.  Each `DF_*` directory holds a set of
`TTree` objects, one per O2 data table.

Key trees inside each `DF_*`:

| Tree | Contents |
|------|----------|
| `O2track_iu` | Track parameters at inner update (IU) |
| `O2trackcov_iu` | Track covariance at IU |
| `O2trackextra_002` | TPC/ITS cluster counts, chi2, PID signals |
| `O2collision_001` | Primary vertex per collision |
| `O2bc_001` | Bunch-crossing info |
| `O2fwdtrack` | Muon / forward tracks |
| `O2mfttrack_001` | MFT tracks |

Trees in the same `DF_*` directory are **row-aligned**: entry *i* of `O2track_iu` and
entry *i* of `O2trackextra_002` belong to the same track.

---

## Preferred approach: native column API

**Always prefer `load-branch` + `col-*` over TChain/RDataFrame** for reading AO2D data in rooture. The native column API:
- Reads branches as flat in-memory float32/int arrays
- Composes with `col-map-ptr`, `col-zip-ptr`, `col-gather`, `col-mask`, `col-fill-h1` without Cling JIT
- Is safe inside `pmap` workers (no JIT, no ROOT method calls)
- Is significantly faster for the typical per-species, per-timeframe analysis pattern

```scheme
;;; preferred — load-branch + col-* operations
(= {q1pt-c} (load-branch aod-path (concat tf "/" "O2track_iu") "fSigned1Pt"))
(= {tgl-c}  (load-branch aod-path (concat tf "/" "O2track_iu") "fTgl"))
(= {pt-c}   (col-map-ptr pt-ptr q1pt-c))   ; fast, no Cling

;;; avoid in hot paths — triggers Cling JIT, not pmap-safe
;; (-> (new ROOT::RDataFrame chain) {.Define "pt" "1.f/..."} ...)
```

TChain/RDataFrame are still useful for quick interactive exploration or one-off plots,
but for production analysis loops use the column API. See `rooture-analysis-basics`
for the full parallel analysis pattern.

### Reach for the col framework first, for *any* per-row task

Beyond reading, prefer the `col-*` framework over hand-rolled `TTree`/`TBranch`/`TLeaf`
walking whenever a task is fundamentally per-row — comparing two files, counting,
filtering, accumulating, deriving quantities. It is both faster and simpler:

- **Row count** comes from `col-length` — never call `TTree::GetEntries` (which forces
  a full tree deserialization) just to size or skip a branch. An empty branch loads as
  a valid 0-length column.
- **Element access** via `col-ref`; whole-column transforms via `col-map-ptr`; pairwise
  comparison/combination via `col-zip-ptr`; folds via `col-reduce-ptr`.
- Avoiding the `.Get`/`.GetEntries` structure walk removes the dominant cost (rooture
  serializes every tree deserialization on the main thread).

⚠️ **`col-zip-ptr` output dtype**: when the jit-fn's *return* type differs from the
*input* column dtype (e.g. an `(Int_t,Int_t)->float` equality test), pass the output
type explicitly as the trailing arg — `(col-zip-ptr fp a b "float")`. Without it the
output defaults to the input dtype and the float return is mis-read as an int (a silent
wrong result). The comparison itself still happens in the input type, so equality stays
exact. See `examples/validate_aod_df.rut` for the file-comparison pattern.

---

## Iterating over directories

```scheme
(def {keys} (.GetListOfKeys (::Open TFile "AO2D.root")))
(def {n}    (.GetEntries keys))

(dotimes {i} n {
  do
  (= {key} (.At keys i))
  (print (.GetClassName key) " " (.GetName key))
})
; → TDirectoryFile DF_2403356384352128
; → TDirectoryFile DF_2403356384413376
; → ...
; → TMap           metaData
```

---

## Building a TChain across all time frames

```scheme
(def {fpath} "AO2D.root")
(def {chain} (new TChain "O2track_iu"))

(dotimes {i} n {
  do
  (= {key} (.At keys i))
  (if (== (.GetClassName key) "TDirectoryFile")
    {(.Add chain (concat fpath "?#" (.GetName key) "/O2track_iu"))}
    {})
})

(.GetEntries chain)   ; → total track count across all TFs
```

To also access `O2trackextra_002` (row-aligned), add it as a friend:

```scheme
(def {extra-chain} (new TChain "O2trackextra_002"))

(dotimes {i} n {
  do
  (= {key} (.At keys i))
  (if (== (.GetClassName key) "TDirectoryFile")
    {do
      (= {dn} (.GetName key))
      (.Add chain       (concat fpath "?#" dn "/O2track_iu"))
      (.Add extra-chain (concat fpath "?#" dn "/O2trackextra_002"))}
    {})
})

(.AddFriend chain extra-chain)
```

---

## RDataFrame from the chain

```scheme
(def {df}
  (-> (new ROOT::RDataFrame chain)
    {.Define "eta"  "TMath::ASinH(fTgl)"}
    {.Define "phi"  "fAlpha + TMath::ASin(fSnp)"}
    {.Define "pt"   "1.f / std::abs(fSigned1Pt)"}
    {.Define "nCls" "int(fTPCNClsFindable) - int(fTPCNClsFindableMinusFound)"}))
```

---

## Key track parameter branches (`O2track_iu`)

| Branch | Type | Description |
|--------|------|-------------|
| `fAlpha` | float | Sector rotation angle (rad) |
| `fX` | float | Local x at reference surface |
| `fY`, `fZ` | float | Local y, z |
| `fSnp` | float | sin(φ_local) — local bending angle |
| `fTgl` | float | tan(λ) = p_z / p_T |
| `fSigned1Pt` | float | q / p_T (signed) |
| `fIndexCollisions` | int | collision index for this track |

Derived:
```
pt  = 1 / |fSigned1Pt|
eta = asinh(fTgl)
phi = fAlpha + asin(fSnp)
```

## Key branches in `O2trackextra_002`

| Branch | Description |
|--------|-------------|
| `fTPCNClsFindable` | TPC findable clusters |
| `fTPCNClsFindableMinusFound` | findable − found |
| `fTPCNClsFindableMinusCrossedRows` | for crossed-rows quality cut |
| `fITSClusterMap` / `fITSClusterSizes` | ITS hit pattern (nonzero = has hit) |
| `fTPCSignal` | TPC dE/dx signal |
| `fTPCInnerParam` | Momentum at inner TPC wall (for BB evaluation) |
| `fTPCChi2NCl` | TPC χ²/cluster |
| `fITSChi2NCl` | ITS χ²/cluster |
| `fTOFChi2` | TOF χ² (< 0 = no TOF hit) |
| `fTOFExpMom` | Momentum at TOF surface |
| `fLength` | Track length (cm) |
| `fTrackTime` | Measured TOF time (ns) |
| `fTrackTimeRes` | TOF time resolution (ns) |

TPC clusters found: `int(fTPCNClsFindable) - int(fTPCNClsFindableMinusFound)`

---

## Gotchas

- Trees in the same `DF_*` dir are row-aligned; do not mix entries across
  different `DF_*` directories when using friends.
- `O2track_iu` uses **inner-update** (IU) helix parameters, not propagated to a fixed
  radius. Use `propagate-to-dca` (from `spectra_tpc.rut`) for DCA computation.
- `fITSClusterMap` (Run2/local) vs `fITSClusterSizes` (Open Data/newer): both work
  with `> itsmap 0.5f` as the ITS hit check.
- For full analysis with event selection, track cuts, and PID, see the
  `rooture-analysis-basics` skill.
