---
description: Fetching objects from the ALICE CCDB via the local credential proxy. Use when the user wants to retrieve calibration or PID parameters (e.g. TPC Bethe-Bloch) from CCDB inside a rooture script.
---

# ccdb-proxy

The ALICE CCDB lives behind a local reverse proxy at `http://localhost:8888` that adds
CERN credentials when forwarding requests.

**If you do not know the bearer token, ask the user before attempting any fetch.**

## Basic fetch pattern

```scheme
(= {hdrs} {"Authorization: Bearer <token>"})
(= {f}    (deref (fetch-url "http://localhost:8888/<path>/<timestamp-ms>" hdrs)))
(= {obj}  (tfile-get f "ccdb_object"))
```

`fetch-url` signature: `(fetch-url url [headers-qexpr] [proxy-string])`
- `headers` is a Q-expression of `"Header: value"` strings.
- The proxy at 8888 is a **reverse proxy** (not HTTP CONNECT) — pass the full URL directly, do not use the third `proxy` argument.
- 303 redirect URLs are rewritten automatically to stay on the same proxy base.

## TPC Bethe-Bloch parameters

- **Path**: `Analysis/PID/TPC/BetheBloch`
- **ObjectType**: `o2::pid::tpc::BetheBloch`
- **Validity**: 0 – 4108971600000 ms (covers all Run 2 + Run 3; one global set of params)
- **Timestamp**: use `1` to get the single available object, or any valid ms timestamp

```scheme
(= {hdrs} {"Authorization: Bearer <token>"})
(= {f}    (deref (fetch-url "http://localhost:8888/Analysis/PID/TPC/BetheBloch/1" hdrs)))
(= {obj}  (tfile-get f "ccdb_object"))
(= {par}  (tobj-member obj "mPar"))
;;; mPar = {kp1 kp2 kp3 kp4 kp5 mMIP resolution_abs}   (7 elements)
;;; bb0  = mMIP * kp1 = par[5] * par[0]   (pid_tpc.rut absorbs mMIP into bb0)
;;; bb1–bb4 = par[1]–par[4]
;;; bb-res = par[6] / par[5]               (absolute resolution / MIP → fractional)
```

Confirmed values (2025 LHC Run 3 pp):
`mPar = {0.0320981, 19.9768, 2.52666e-16, 2.72123, 6.08092, 50, 2.3}`
→ `bb0=1.60491`, `bb1–bb4` as above, `bb-res=0.046`

## Using with spectra_tpc.rut

`spectra_tpc.rut` fetches and sets `bb0`–`bb4` + `bb-res` automatically when
`ccdb-pid-ts > 0`:

```scheme
(def {ccdb-pid-ts}      1448150400000)
(def {ccdb-pid-url}     "http://localhost:8888")
(def {ccdb-pid-headers} {"Authorization: Bearer <token>"})
(load "examples/spectra_tpc.rut")
```
