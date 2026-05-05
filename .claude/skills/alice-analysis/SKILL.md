---
description: ALICE physics analysis in rooture. Use when the user is working with ALICE data, AO2D files, spectra, PID, track propagation, or any O2Physics-equivalent analysis. Delegates to focused sub-skills listed below.
---

# alice-analysis

Umbrella skill for ALICE analyses in rooture. Sub-skills to load:

| Task | Skill |
|------|-------|
| Read AO2D files, understand data layout, use TChain / RDataFrame | `alice-aod` |
| Full analysis setup: event sel, track cuts, DCA, PID, pmap | `rooture-analysis-basics` |
| Run `spectra_tpc.rut`, compare to `AnalysisResults.root`, PID plot | `spectra-comparison` |
| Species colors, line widths, axis conventions for any plot | `plot-conventions` |

## Key conventions

- **Species order** in all lists: `{El Mu Pi Ka Pr De}` (index 0–5)
- **Tree names (Run 3)**: `O2track_iu`, `O2trackextra_002`, `O2collision_001`, `O2bc_001`
- **Tree names (Run 2 converted)**: `O2track`, `O2trackextra`, `O2collision`, `O2bc`
- **Solenoid field**: `(def {bz} 0.5)` before loading `spectra_tpc.rut`
- **CCDB BB params**: set `ccdb-pid-ts` to Unix timestamp in ms before loading

## Standard analysis flow

```
load stdlib.rut
→ guard aod-paths
→ set defaults (trk-tree, extra-tree, coll-tree, bc-tree, bz)
→ set event selection params (bcSOR, nBCsPerTF, rofLength, tvx-bit, pvz-cut)
→ load examples/eventsel.rut
→ load examples/pid_tpc.rut
→ enumerate timeframes (TDirectoryFile keys)
→ define jit-fns + jitfn-ptr (before pmap)
→ pre-create histograms on main thread
→ pmap over TF indices
  → load-branch for all needed columns
  → compute-evsel-tf → evsel-and → col-gather to tracks
  → propagate-to-dca (from spectra_tpc.rut)
  → build quality mask (m-ev, m-eta, m-cls, m-crows, m-tpcq, m-its, m-dca)
  → pid-masks → combine with quality mask
  → col-fill-h1
→ merge per-TF histograms
→ draw
```

## Critical rules

- **Never call `new` or ROOT methods inside `pmap` workers** — Cling JIT is not thread-safe concurrently. Create all ROOT objects before pmap.
- **DCA cut must be pT-parameterized**: `|dcaXY| < 0.0105 + 0.035/pT^1.1` (flat cuts pass Λ→pπ secondaries).
- **TOF units**: `fTrackTime` and `fTrackTimeRes` are in **nanoseconds** → `tof-c = 29.9792458 cm/ns`.
- **m-dca must be in the mask**: define `m-dca` and AND it into the main mask; omitting it silently passes all secondaries.
