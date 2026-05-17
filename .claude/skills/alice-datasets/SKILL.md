---
description: Tree names, file paths, branch names, and reference files for known local ALICE datasets. Read before running any example that loads AO2D data.
---

# alice-datasets

## Tree name configuration for spectra_tpc.rut

`spectra_tpc.rut` defaults to Run 3 tree names. Always set all five variables with
`def` (not `default`) before loading — `default` will not override a value already
bound from a previous session.

```scheme
;;; Run 2 converted (LHC15o, LHC15n, …):
(def {trk-tree}       "O2track")
(def {extra-tree}     "O2trackextra")
(def {coll-tree}      "O2collision")
(def {bc-tree}        "O2bc")
(def {its-map-branch} "fITSClusterMap")   ;;; uint8 bitmask, 1 bit per ITS layer

;;; Run 3 AO2D (e.g. /Users/ktf/Downloads/AO2D.root):
(def {trk-tree}       "O2track_iu")
(def {extra-tree}     "O2trackextra_002")
(def {coll-tree}      "O2collision_001")
(def {bc-tree}        "O2bc_001")
(def {its-map-branch} "fITSClusterSizes") ;;; packed nibbles, 4 bits per ITS layer
```

## LHC15o — Run 2 converted OpenData (run 245066)

**Base path:** `/Users/ktf/src/data/2015/LHC15o/000245066/pass2/PWGZZ/Run3_Conversion/320_20220701-0813/AOD/`

**Files (8 total):** `009/AO2D.root`, `012/AO2D.root`, `018/AO2D.root`, `019/AO2D.root`,
`021/AO2D.root`, `030/AO2D.root`, `034/AO2D.root`, `049/AO2D.root`

**Stats:** 127 timeframes, ~23M selected tracks (flat DCA), ~20M (pT-parameterised DCA)

**Tree names:** `O2track`, `O2trackextra`, `O2collision`, `O2bc` (no numeric suffixes)

**ITS branch:** `fITSClusterMap`

**Loading snippet:**
```scheme
(def {aod-paths} (list
  "/Users/ktf/src/data/2015/LHC15o/000245066/pass2/PWGZZ/Run3_Conversion/320_20220701-0813/AOD/009/AO2D.root"
  "/Users/ktf/src/data/2015/LHC15o/000245066/pass2/PWGZZ/Run3_Conversion/320_20220701-0813/AOD/012/AO2D.root"
  "/Users/ktf/src/data/2015/LHC15o/000245066/pass2/PWGZZ/Run3_Conversion/320_20220701-0813/AOD/018/AO2D.root"
  "/Users/ktf/src/data/2015/LHC15o/000245066/pass2/PWGZZ/Run3_Conversion/320_20220701-0813/AOD/019/AO2D.root"
  "/Users/ktf/src/data/2015/LHC15o/000245066/pass2/PWGZZ/Run3_Conversion/320_20220701-0813/AOD/021/AO2D.root"
  "/Users/ktf/src/data/2015/LHC15o/000245066/pass2/PWGZZ/Run3_Conversion/320_20220701-0813/AOD/030/AO2D.root"
  "/Users/ktf/src/data/2015/LHC15o/000245066/pass2/PWGZZ/Run3_Conversion/320_20220701-0813/AOD/034/AO2D.root"
  "/Users/ktf/src/data/2015/LHC15o/000245066/pass2/PWGZZ/Run3_Conversion/320_20220701-0813/AOD/049/AO2D.root"))
(def {trk-tree}       "O2track")
(def {extra-tree}     "O2trackextra")
(def {coll-tree}      "O2collision")
(def {bc-tree}        "O2bc")
(def {its-map-branch} "fITSClusterMap")
(load "examples/spectra_tpc.rut")
```

## Run 3 AO2D — single test file

**Path:** `/Users/ktf/Downloads/AO2D.root`

**Stats:** 3 timeframes, ~37–42k selected tracks (depending on DCA cut)

**Tree names:** `O2track_iu`, `O2trackextra_002`, `O2collision_001`, `O2bc_001`

**ITS branch:** `fITSClusterSizes`

## O2Physics reference file

**Path:** `/Users/ktf/Downloads/AnalysisResults.root`

**Task:** `tpc-spectra-tiny`

**Navigation** (use `TFile::Get` with slash path):
```scheme
(= {f}   (new TFile "/Users/ktf/Downloads/AnalysisResults.root" "READ"))
(= {hpi} (. Get f "tpc-spectra-tiny/pt/Pi"))
(= {hka} (. Get f "tpc-spectra-tiny/pt/Ka"))
(= {hpr} (. Get f "tpc-spectra-tiny/pt/Pr"))
```

The `comparison.rut` skill script hardcodes `/tmp/AnalysisResults.root` — always
pass the real path in the eval, do not rely on the skill default.
