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

## Iterating over directories

```scheme
(def {f}    (new TFile "AO2D.root" "READ"))
(def {keys} (.GetListOfKeys f))
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

To also access `O2trackextra_002` (row-aligned with `O2track_iu`), add it as a friend:

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

`new ROOT::RDataFrame` requires a `TTree&`; rooture's deref-retry in `builtin_new`
handles the pointer-to-reference conversion automatically (first Cling attempt fails
visibly, second attempt with `*chain` succeeds silently).

```scheme
(def {df}
  (-> (new ROOT::RDataFrame chain)
    {.Range 500000}                           ; optional: sample first N tracks
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

Derived quantities:
```
pt  = 1 / |fSigned1Pt|
eta = TMath::ASinH(fTgl)
phi = fAlpha + TMath::ASin(fSnp)
```

## Key branches in `O2trackextra_002`

| Branch | Description |
|--------|-------------|
| `fTPCNClsFindable` | TPC findable clusters |
| `fTPCNClsFindableMinusFound` | findable − found (so found = findable − this) |
| `fTPCNClsFindableMinusCrossedRows` | for crossed-rows quality cut |
| `fTPCNClsShared` | shared TPC clusters |
| `fITSClusterSizes` | ITS cluster sizes (packed) |
| `fTPCSignal` | TPC dE/dx signal |
| `fTPCChi2NCl` | TPC χ²/ndf |
| `fITSChi2NCl` | ITS χ²/ndf |

TPC clusters found: `int(fTPCNClsFindable) - int(fTPCNClsFindableMinusFound)`

---

## pT spectrum example

```scheme
(def {hpt}
  (.Histo1D df
    (new ROOT::RDF::TH1DModel "hpt" "Track p_{T};p_{T} (GeV/c);Tracks" 200 0. 10.)
    "pt"))

(def {c} (new TCanvas "c" "pT" 800 600))
(.SetLogy c 1)
(.DrawClone hpt)
(.Update c)
```

---

## TH3 with interactive cluster-cut slider

See `examples/tracks_3d_gui.rut` for a full example that:
1. Fills a TH3D (η × φ × N_TPC cls) from 500k sampled tracks.
2. Shows a `TGHSlider` window; dragging it calls `TH3::Project3D("yx")` with a
   Z-axis range cut and redraws the COLZ canvas live.

Key pattern for range-filtered projection:
```scheme
(.SetRange (.GetZaxis h3raw) (+ minCls 1) 162)   ; bins, not values
(def {hp} (.Project3D h3raw "yx"))                ; returns TH2D
(.SetRange (.GetZaxis h3raw) 0 162)               ; reset
```

---

## Gotchas

- `GetName` / `GetClassName` on `TKey` objects return `const char*` — rooture handles
  these as `LVAL_STR` via the `kString` branch of `TMethodCall`.
- The Cling probe errors printed for `new ROOT::RDataFrame`, `Histo1D`, `Histo3D`
  etc. are **harmless** — they are from the first (failing) deref probe; the second
  attempt succeeds silently.
- All trees in the same `DF_*` dir are row-aligned; do not mix entries across
  different `DF_*` directories when using friends.
- `O2track_iu` uses **inner-update** (IU) helix parameters, not propagated to a fixed
  radius. Use `O2trackcov_iu` for the covariance matrix if needed.
