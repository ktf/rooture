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
| Fetch calibration/PID objects from CCDB via credential proxy | `ccdb-proxy` |
| Light nuclei (He3/He4) PID, O2nucleitable, ITS cluster size, TOF m² | `alice-nuclei` |

## Key conventions

- **Species order** in all lists: `{El Mu Pi Ka Pr De}` (index 0–5)
- **Tree names (Run 3)**: `O2track_iu`, `O2trackextra_002`, `O2collision_001`, `O2bc_001`
- **Tree names (Run 2 converted)**: `O2track`, `O2trackextra`, `O2collision`, `O2bc`
- **Solenoid field**: `(def {bz} 0.5)` before loading `spectra_tpc.rut`
- **CCDB BB params**: set `ccdb-pid-ts` to Unix timestamp in ms before loading

## Standard analysis flow

All library scripts use the **provide system**: loading is side-effect-free; call `(run)`
to execute. Use the `load` MCP tool to load scripts — it returns the exported symbol manifest.

```
load stdlib.rut
→ guard aod-paths
→ set defaults (trk-tree, extra-tree, coll-tree, bc-tree, bz)
→ set event selection params (bcSOR, nBCsPerTF, rofLength, tvx-bit, pvz-cut)
→ load examples/eventsel.rut    → exports: {compute-evsel-tf evsel-and evsel-sel8}
→ load examples/pid_tpc.rut     → exports: {pid-masks bb0 bb1 bb2 bb3 bb4 bb-res ...}
→ load examples/spectra_tpc.rut → exports: {run tf-pairs propagate-to-dca h-pt h-p ...}
→ (run)    ← triggers: enumerate TFs, pmap over TF indices, merge, draw
  → load-branch for all needed columns
  → compute-evsel-tf → evsel-and → col-gather to tracks
  → propagate-to-dca
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
