---
description: Light nuclei (He3, He4, deuteron) PID and selection for ALICE analyses. Use when working with O2nucleitable, anti-helium searches, ITS cluster size, TOF mass², or nuclei-specific track quality. Covers physics pitfalls, selection strategy, and rooture implementation.
---

# alice-nuclei

Physics analysis for light nuclei (³He, ⁴He and anti-particles) in ALICE Run 3 AO2D.

## Data format: O2nucleitable

Pre-filtered flat table produced by O2Physics `NucleiTask`. One tree per TF, tree name `O2nucleitable`.

| Branch | Type | Description |
|---|---|---|
| `fPt` | `/F` | Signed tracking pT (GeV/c); **fPt < 0 → anti-particle** |
| `fEta`, `fPhi` | `/F` | Kinematics |
| `fTPCInnerParam` | `/F` | Rigidity at inner TPC wall = p/\|Z\| (always positive) |
| `fTPCsignal` | `/F` | TPC dE/dx (a.u.) |
| `fBeta` | `/F` | TOF β = v/c; **sentinel 0.0001 = no TOF hit** |
| `fZvertex` | `/F` | Primary vertex Z (cm) |
| `fNContrib` | `/I` | **int32** — must `col-cast-f32` before passing to float jit-fns |
| `fDCAxy`, `fDCAz` | `/F` | Pre-computed DCA to PV (cm) |
| `fTPCchi2` | `/F` | TPC chi²/cluster |
| `fTPCfindableCls` | `/b` | uint8 — must `col-cast-f32` |
| `fTPCcrossedRows` | `/b` | uint8 — must `col-cast-f32` |
| `fTPCnCls` | `/b` | uint8 — must `col-cast-f32` |
| `fITSclusterSizes` | `/i` | uint32 packed nibbles (4 bits × 7 ITS layers) |
| `fITSchi2` | `/F` | ITS chi²/cluster |

**Critical**: `fNContrib` is int32, uint8 branches must be cast. Forgetting `col-cast-f32` on `fNContrib` sets the event-quality mask to all-zero (silent analysis failure).

## Physics of Z=2 nuclei in the TPC/ITS

### Tracker convention: rigidity, not momentum
The ALICE tracker reconstructs the **magnetic rigidity** R = p/|Z|. For He3/He4 (Z=2), `fTPCInnerParam` = p_rig = p_actual/2. The actual momentum is p_actual = 2 × p_rig.

### TPC signal: dedicated He3 BB parametrisation

**Do NOT use the universal proton BB (from CCDB `Analysis/PID/TPC/BetheBloch`) for He3.**
That parametrisation uses βγ_eff = p_rig/m_nucleus with Z²=4 scaling and overestimates the He3 signal by ~10-15%, giving nσ_He3 ≈ −2 to −3 for genuine He3.

Instead use the **dedicated He3 BB from O2Physics NucleiTask** (implemented in `pid_nuclei.rut`):

```cpp
// C++ reference:
double bb(double bg, double kp1, double kp2, double kp3, double kp4, double kp5) {
  double beta = bg / sqrt(1 + bg*bg);
  double aa   = pow(beta, kp4);
  double lnt  = log(kp3 + pow(1.0/bg, kp5));
  return (kp2 - aa - lnt) * kp1 / aa;
}
float bbHe3(float mom) {
  return bb(mom/2.80839, -321.34, 0.6539, 1.591, 0.8225, 2.363);
}
```

Key difference: input is the **actual He3 momentum** `mom = 2 × fTPCInnerParam` (Z × rigidity), NOT the rigidity. No explicit Z² factor — it is absorbed into kp1. This gives ~260 a.u. at minimum ionising.

Result: with dedicated BB, nσ_He3 mean ≈ −0.5 for genuine He3 (residual from bb-res; a He3-specific resolution ~0.06 instead of CCDB proton value 0.046 would center it at 0).

This formula is implemented in `examples/pid_nuclei.rut` as `nsig-tpc-he3-fn`, `pid-he3-fn`, and `bb-he3-fn` (standalone expected signal for BB curve overlays).

### Low-momentum Bragg peak background
Z=1 particles (π, K, p) at low rigidity (p_rig < ~0.8 GeV/c) are in their **Bragg peak** region with very high dE/dx (hundreds to thousands of a.u.). These tracks:
- Pass a wide He3 TPC nσ window
- Have large ITS cluster sizes due to delta-ray production and high charge deposition
- Are the dominant background before a minimum momentum cut

**Always apply a minimum rigidity cut: p_rig > 1.0 GeV/c** for He3 searches. This puts Z=1 particles firmly in the minimum-ionising region while He3 at p_rig ≈ 1 GeV/c is still on the rising BB curve with 4× higher signal.

### ITS cluster size as Z² discriminator
The ITS mean cluster size × cos(λ) (correcting for track inclination) scales approximately as Z²:
- Z=1 MIP tracks: mean ≈ 2.5 (peak of distribution)
- Z=2 He3/He4: mean ≈ 3.5–6 (shifted higher, with momentum dependence)

The discriminating power is real but imperfect: the MIP tail overlaps the He3 distribution below ~4.5. A **momentum-dependent cut** is more powerful than a flat cut:

```scheme
;;; ITS cls cut: tighter at low p (more background), looser at high p
;;; Approximate: threshold = 3.5 + 1.0/p_rig
(def {its-mom-cut-ptr}
  (jitfn-ptr (jit-fn float (\{{float clsz} {float p}}
    {(if (> clsz (+ 3.5f (/ 1.0f p)))
      {1.0f} {0.0f})}))))
```

To calibrate the cut: first get a **gold sample** via TPC + TOF (see below), plot ITS cls×cosλ vs p_rig for those tracks, and fit the lower edge.

### TOF mass²: gold sample strategy

TOF m² for Z=2 nuclei:
```
m² = (Z × p_rig)² × (1/β² − 1) = 4 × p_rig² × (1/β² − 1)
```

Expected values:
- ³He: m² ≈ 7.89 (GeV/c²)²
- ⁴He: m² ≈ 13.89 (GeV/c²)²

**Gold sample selection** (pure He3 for efficiency/calibration studies):
```
|nσ_He3_TPC| < 3  AND  4 < m² < 13 (GeV/c²)²
```
With CCDB-calibrated (but Z=1) BB params, shift TPC window to: −5 < nσ < 1.

**TOF sentinel**: `fBeta = 0.0001` means no TOF hit. Apply `beta < 0.01 → m² = −999` and filter these out with `keep-m2-fn`:
```scheme
(def {mass2-beta-fn}
  (jitfn-ptr (jit-fn float (\{{float pinner} {float beta}}
    {(if (< beta 0.01f)
      {(- 0.0f 999.0f)}
      {(* (* 4.0f (* pinner pinner))
          (- (/ 1.0f (* beta beta)) 1.0f))})}))))
```

### ITS cluster size decode from uint32

`fITSclusterSizes` encodes 7 × 4-bit nibbles. Layer L occupies bits [4L .. 4L+3]. Decoded via `examples/its_clsize.rut` which provides `col-its-cls-size` and `col-its-inner-cls-size` (inner 3 layers only, strongest discriminant).

```scheme
(load examples/its_clsize.rut)
(= {tgl-c}  (col-map-ptr sinh-fn eta-c))   ;; tgl = sinh(η)
(= {cls-c}  (col-its-cls-size its-c tgl-c))
```

## Recommended selection pipeline

```
1. Base quality (apply col-cast-f32 to int/uint branches!)
   |η| < 0.8
   TPC nCls ≥ 80, chi²/cls < 3.5
   crossed rows ≥ 70, crossed/findable ≥ 0.8
   |dcaXY| < 0.1 cm, |dcaZ| < 1.0 cm  (primary nuclei; no pT parametrisation)
   |Zvtx| < 10 cm, nContrib ≥ 1

2. Anti-particle flag: fPt < 0

3. Minimum rigidity: p_rig > 1.0 GeV/c  (removes Bragg-peak Z=1 background)

4. TPC He3 window: |nσ_He3| < 3
   (with CCDB Z=1 BB params, signal peaks at nσ ≈ −2.5; consider −5 < nσ < 1)

5. TOF confirmation (when available): 4 < m² < 13 (GeV/c²)²
   → gold sample: ~15k He3 per O2nucleitable file (LHC22o-level luminosity)

6. ITS cluster size: col-its-cls-size > 3.5 + 1.0/p_rig
   (calibrate from gold sample: plot ITS cls vs p_rig, fit lower edge)
```

## QA plots to always make

1. **dE/dx vs p_rig** (2D, log-z): shows He3/He4 BB bands
2. **nσ_He3 before/after ITS cut**: checks BB calibration offset
3. **TOF m² of TPC candidates**: dominant at m²≈0, He3 bump at 7.89
4. **TOF m² of gold sample**: confirms He3 identity (should peak at ~7.89)
5. **ITS cls×cosλ vs p_rig for gold sample**: defines the momentum-dependent cut

## rooture implementation notes

- Use `nuclei_filtered.rut` as the starting point for O2nucleitable files
- Use `pid_nuclei.rut` for He3/He4 nσ and TOF expected-time functions
- `its_clsize.rut` provides the ITS cluster size decode
- `tracksel.rut` provides shared quality cuts (dca-nuc-ptr, eta-ok-ptr, etc.)
- CCDB BB params: set `ccdb-pid-ts` to any valid Run 3 timestamp before loading `pid_nuclei.rut`
