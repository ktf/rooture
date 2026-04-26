# TMethodCall memoization — performance diary

This page documents the design, measurement, and implementation of
`TMethodCall` result caching in rooture's method-dispatch hot path
(`builtin_member` in `rooture.cxx`).

---

## Motivation

The `dotimes` chi²-over-bins loop in `examples/fit_gui.rut` hung rooture
(several seconds for 100 iterations × 3 method calls each).
The root cause: every call to `builtin_member` constructs a fresh
`TMethodCall(TClass*, method_name, args_string)`, which does a linear
overload-resolution search in Cling — typically 5–15 ms each.

In a tight loop calling the same method with the same argument types,
all that work is identical on every iteration.  The fix is to cache the
resolved `TMethodCall` object keyed on `(class, method, arg-type-signature)`
and reuse it with typed `ResetParam` / `SetParam` / `Execute` on subsequent
calls.

---

## Design (Opus plan, summarised)

- **Cache key**: `"ClassName::MethodName(L,D,S,O,...)"` where
  `L` = Long (LVAL_NUM), `D` = Double (LVAL_FLOAT), `S` = String, `O` = Object pointer.
  Built from lval types *before* `lval_to_cpp_arg` serialises them to strings.

- **Cache value** (`CachedMethodCall`):
  - `TClass* cached_cls` — validated on hit to guard against class reuse
  - `unique_ptr<TMethodCall> mc` — the resolved, reusable method object
  - `EReturnType ret_type` — drives the `Execute` variant
  - `bool is_ptr_return`, `TClass* ptr_ret_cls` — for kLong pointer returns
  - `vector<EDataType> param_types` — **critical**: expected C++ type per
    parameter, populated from `TMethod::GetListOfMethodArgs()` at miss time

- **Type coercion (option 2)**: On a cache hit, for each `LVAL_NUM` arg the
  expected `EDataType` is checked; if it is a floating-point type
  (`kFloat_t`, `kDouble_t`, `kDouble32_t`, `kFloat16_t`), `SetParam(Double_t)`
  is used instead of `SetParam(Long_t)`.  This correctly handles calls like
  `(.SetRangeUser axis 1 10)` where the integers must reach a `Double_t` parameter.

- **Not cached**:
  - Calls with callable args (lambdas) — already go through Cling
  - Smart-pointer classes (those with `operator->`) — obj_cls may change
  - Non-void `kOther` returns — require Cling typeid path
  - kLong pointer returns where `TClass::GetClass(bare)` fails (dynamic type
    recovery via Cling typeid; result is pointer-instance-specific)

---

## Benchmark

`examples/bench_method_dispatch.rut` measures three representative call patterns:

| Pattern | Args | Return | Cache key |
|---------|------|--------|-----------|
| `GetBinContent(1)` | LVAL_NUM | kDouble | `TH1F::GetBinContent(L)` |
| `GetRandom()` | none | kDouble | `TH1F::GetRandom()` |
| `SetBinContent(1, 0.5)` | LVAL_NUM + LVAL_FLOAT | void | `TH1F::SetBinContent(L,D)` |

Each pattern runs 500 iterations.

### Baseline (before caching)

| Pattern | Time (500 calls) | Per-call |
|---------|-----------------|---------|
| `GetBinContent(1)` | 0.0055 s | 0.011 ms |
| `GetRandom()` | 0.0045 s | 0.009 ms |
| `SetBinContent(1, 0.5)` | 1.90 s | **3.8 ms** |

The mixed LVAL_NUM + LVAL_FLOAT case is 350× slower than the integer-only case.
The arg string `"1, 0.500000"` causes Cling to compile a float-conversion wrapper
on every `TMethodCall` construction.  Integer-only args (`"1"`) appear to hit a
faster Cling path.

### After caching

| Pattern | Time (500 calls) | Per-call | Speedup |
|---------|-----------------|---------|---------|
| `GetBinContent(1)` | 0.004 s | 0.008 ms | ~1.4× |
| `GetRandom()` | 0.003 s | 0.005 ms | ~1.8× |
| `SetBinContent(1, 0.5)` | 0.004 s | 0.009 ms | **433×** |

The `exec` cost on cache hits is ~0.05 µs/call (measured via `clock_gettime`).
The large speedup for `SetBinContent` is because the mixed int+float args string
`"1, 0.500000"` caused Cling to compile a float-conversion wrapper on every fresh
`TMethodCall` construction.  With caching that cost is paid once.

---

## Debugging session — why SetBinContent never hit the cache

After the initial caching implementation, `GetBinContent` and `GetRandom`
became fast, but `SetBinContent(1, 0.5)` still ran at ~3.8 ms/call — the
same as before caching.  This section documents the full investigation.

### Step 1 — establish a native C++ baseline

Before profiling rooture, we needed to know the floor: how fast are these
calls in native C++?  A plain ROOT macro
([`examples/bench_method_dispatch.C`](../examples/bench_method_dispatch.C))
runs the same three patterns using direct C++ calls:

```
500x GetBinContent(1)    : 0.018 us/call
500x GetRandom()         : 0.034 us/call
500x SetBinContent(1,0.5): 0.006 us/call
```

All three are sub-microsecond in native C++.  Rooture's cache-hit cost of
~0.05 µs/call (measured later) is within 2–8× of the native baseline —
the overhead is from the `builtin_member` dispatch machinery, not from the
method execution itself.

### Step 2 — profile with macOS `sample`

With rooture running as an MCP server (PID known via `pgrep rooture`), load
the benchmark from a separate terminal and simultaneously attach the sampler:

```sh
# Terminal 1 — trigger the benchmark via MCP eval
# Terminal 2 — profile while it runs:
sample <PID> 10 -file /tmp/rooture_profile.txt
open /tmp/rooture_profile.txt
```

The relevant section of the profile:

```
1390 builtin_member(lenv*, lval*)  (in rooture)
  +  371 $_3::operator()()         (in rooture)   ← probe() lambda
  |    TCling::ProcessLine(...)
  |      LLVM JIT compilation
  +  641 builtin_member + 8220     ← no symbol: JIT wrapper frame
```

Two things stand out:

1. **371 samples in `$_3::operator()()`** — this is the `probe()` lambda
   inside `cling_new_auto_typed`.  It calls `ProcessLine` to compile a
   `using __rut_tN = decltype(expr);` alias for each call.  371 ms of JIT
   compilation means `cling_new_auto_typed` is being called on most iterations.

2. **641 samples in `builtin_member` with no callee symbol** — JIT-compiled
   wrapper frames are not in the macOS symbol table.  Time spent inside the
   Cling-generated wrapper is attributed back to the calling C++ frame.
   This is not a bug; it means the method is actually executing but the
   profiler can't resolve the frame.

The profile told us: **the Cling fallback path is active for SetBinContent**,
even though the cache should be handling it.

### Step 3 — add timing instrumentation to the cache hit path

To confirm cache hits are fast (and distinguish "cache hits are slow" from
"there are no cache hits"), we wrapped the hit path's `Execute` calls with
`clock_gettime(CLOCK_MONOTONIC)` and printed a rolling average every 100 hits:

```cpp
static thread_local long long g_hit_count = 0;
static thread_local double    g_pad_ns    = 0, g_exec_ns = 0;
auto t0 = ns_now();
if (pad_snapshot) { pad_snapshot->cd(); gROOT->SetSelectedPad(pad_snapshot); }
auto t1 = ns_now();
g_pad_ns += t1 - t0;
cached.mc->Execute(obj_ptr, arg_ptrs, nargs, &ret);
auto t2 = ns_now();
g_exec_ns += t2 - t1;
if (++g_hit_count % 100 == 0)
    fprintf(stderr, "[cache-timing] hits=%lld pad_cd=%.2fus exec=%.2fus\n", ...);
```

Result: after running the benchmark, the timing log showed exactly **1000 hits**
(500 for `GetBinContent` + 500 for `GetRandom`) and **0 hits for SetBinContent**.
The hit path itself was fast — `exec=0.05µs` — so the problem was that
SetBinContent *never reached the hit path*, not that the hit path was slow.

> **Lesson:** instrument the *count* first, not just the cost.  A zero hit
> count is a different bug from a slow hit path.

### Step 4 — diagnose `mc.IsValid()` failure

The most obvious way for a method to bypass the cache: `mc.IsValid()` returns
false, which clears `cache_key` and falls through to `cling_new_auto_typed`.
We added a one-line print at that branch:

```cpp
if (!mc.IsValid()) {
    fprintf(stderr, "[cache-debug] mc.IsValid() FAILED: %s::%s args='%s'\n",
            class_name.c_str(), method_name.c_str(), args.c_str());
    cache_key = "";
    ...
```

Result: **no output** — `mc.IsValid()` succeeded for SetBinContent.  The
TMethodCall was constructed and resolved correctly; the problem was downstream.

> **Lesson:** rule out the obvious exits first with a one-shot print.

### Step 5 — trace the return-type switch branch

With `mc.IsValid()` confirmed OK, the next suspect was the `switch (ret_type)`
in the miss path.  The `kOther` branch has an early `return` that bypasses
cache population when `retname != "void"`:

```cpp
case TMethodCall::kOther: {
    TMethod* m = obj_cls->GetMethodAny(method_name.c_str());
    std::string retname = m ? m->GetReturnTypeName() : "";
    if (retname == "void") { ... break; }   // ← caches
    cache_this = false;
    return cling_new_auto_typed(...);        // ← never caches, early return
}
```

We added one-shot diagnostics using a `static unordered_set<string>` to
print only on the *first* visit per cache key (avoiding hot-path flooding):

```cpp
{ static std::unordered_set<std::string> dbg_set;
  if (dbg_set.insert(cache_key).second)
      fprintf(stderr, "[cache-debug] kOther: %s::%s retname='%s' m=%p\n",
              class_name.c_str(), method_name.c_str(), retname.c_str(), (void*)m); }
```

And similarly for the `default: // kNone` branch and for the cache population
block (`if (cache_this)`).

> **Lesson on hot-path diagnostics:** `fprintf` to stderr in a 500-iteration
> loop can take ~1 ms/call (synchronous I/O), turning a 5 ms benchmark into a
> 500 ms hang.  Always gate hot-path prints with a `static set` or a flag so
> each unique key prints at most once.

### Step 6 — the smoking gun

Benchmark output with the branch diagnostics:

```
[cache-debug] kOther: TStopwatch::Start  retname='void' m=0x71e613020
[cache-debug] populating: key='TStopwatch::Start(L)' ret_type=3
[cache-debug] populating: key='TH1F::GetBinContent(L)' ret_type=1
[cache-debug] populating: key='TH1F::GetRandom()' ret_type=1
[cache-debug] kOther: TH1F::SetBinContent retname='' m=0x0
```

The last line is decisive: `m=0x0` — `GetMethodAny("SetBinContent")` returned
**null** for `TH1F`.  With `retname = ""`, the `if (retname == "void")` guard
fails, `cache_this` is set to false, and the function returns early via
`cling_new_auto_typed` on every single call.

Compare `TStopwatch::Start`: `GetMethodAny` succeeded (`m=0x71e613020`),
`retname='void'`, so it broke out of the switch and was cached.

### Step 7 — root cause and fix

`TClass::GetMethodAny` searches the class's own method list.  `SetBinContent`
is declared on `TH1`, not on `TH1F`, so the lookup on the `TH1F` `TClass`
returns null even though `TMethodCall` (which uses Cling's full overload
resolution) finds it without trouble.

The fix: `TMethodCall` already holds the resolved `TFunction*` — use
`mc.GetMethod()` instead of repeating a fallible `GetMethodAny` lookup:

```cpp
// Before (broken for inherited methods):
TMethod* m = obj_cls->GetMethodAny(method_name.c_str());
std::string retname = m ? m->GetReturnTypeName() : "";

// After (uses what TMethodCall already resolved):
TFunction* mf = mc.GetMethod();
std::string retname = mf ? mf->GetReturnTypeName() : "";
```

The same replacement was applied in the `kLong` pointer-return check and in
the cache-population block where `GetMethodAny` was used to fill `param_kinds`.

After rebuilding, all three benchmark methods cached correctly:

```
[cache-timing] hits=1500 ...   ← all 500×3 iterations cached
500x SetBinContent(1,0.5): 0.004 s   (was 1.7 s — 433× speedup)
```

---

## Implementation notes

### `lval_to_cpp_arg` is destructive

`lval_to_cpp_arg(e, a, 0)` walks `a->cell[i]` and may call
`register_rooture_callable` which frees cells.  The cache-arg collection
must happen *before* this call, while the lval list is intact.

### Collected args (`CacheArg`) hold value snapshots

Each `CacheArg` stores a copy of the value (`long num`, `double flt`,
`std::string str`, `void* obj`) so that the original lval can be freed
normally.  For LVAL_TOBJ only the pointer is needed since TMethodCall
passes it as a `Long_t` cast.

### TMethodCall is not copyable; use `unique_ptr`

`std::unique_ptr<TMethodCall>` in `CachedMethodCall` lets the struct be
move-constructed into the `unordered_map` without requiring TMethodCall
to be copyable.  On a cache miss the new `TMethodCall` is constructed
*after* execution (using the same `args` string that was used to construct
the local `mc`), so only one construction happens per call, amortised.

### Smart-pointer and Cling-fallback paths skip the cache

When `mc.IsValid()` fails and we enter the `operator->` dereference or
Cling fallback branches, `cache_key` is cleared so neither path pollutes
the cache.  The non-void `kOther` return branch also clears `cache_key`
before delegating to `cling_new_auto_typed`.

---

## Key takeaways

- **The bottleneck was `TMethodCall` construction, not execution.**  Each
  construction triggers Cling overload resolution (5–15 ms for pure-integer
  args, up to ~4 ms for mixed int+float args that force a float-conversion
  wrapper).  Caching the resolved `TMethodCall` and calling
  `Execute(obj, args[], nargs, ret)` directly drops per-call cost to ~0.05 µs.

- **Use `mc.GetMethod()` not `GetMethodAny()` for return-type inspection.**
  `TClass::GetMethodAny` fails (returns null) for methods inherited from base
  classes on some ROOT class hierarchies (e.g. `TH1F::SetBinContent` defined
  on `TH1`).  The `TMethodCall` object has already resolved the method at
  construction time; `mc.GetMethod()` retrieves it directly.  Relying on
  `GetMethodAny` silently fell through to the non-void `kOther` path
  (`cache_this = false`) and prevented all void-returning inherited methods
  from ever entering the cache.

- **`kOther` + void is a legitimate cached return type.**  ROOT's
  `TMethodCall::ReturnType()` returns `kOther` (not `kNone`) for some
  void methods.  The `retname == "void"` guard inside the `kOther` branch
  correctly identifies these and caches them; the hit path uses the
  `Execute(void*, args[], nargs, nullptr)` form.

- **Cache key uses lval types, not arg values.**  The key
  `"ClassName::Method(L,D,O,...)"` is built from `LVAL_NUM`/`LVAL_FLOAT`/
  `LVAL_TOBJ` type tags before `lval_to_cpp_arg` serialises them.  This
  means the same method with the same argument-type signature always hits
  the same cache entry regardless of the actual numeric values.

- **For performance debugging: instrument counts before costs.**  Knowing
  that the cache hit count was 0 (not "cache hits are slow") immediately
  pointed to the miss path, not the hit path.  The timing instrumentation
  confirmed this split: `exec=0.05µs` per hit meant any remaining slowness
  was from calls that never reached the hit path.

- **JIT frames are invisible to `sample`.**  Cling-generated wrapper
  functions appear as unlabelled offsets in `builtin_member`.  Time spent
  inside JIT code is attributed to the calling C++ frame.  Use `fprintf`
  timing or `clock_gettime` within the C++ code to measure JIT-path cost
  directly; the system profiler alone cannot distinguish "slow hit" from
  "no hit, falling through to Cling every time".

- **Hot-path `fprintf` can dominate the benchmark.**  Writing to stderr
  inside a 500-iteration loop adds ~1 ms/call (synchronous I/O on macOS),
  turning a 5 ms benchmark into a 500 ms hang.  Gate hot-path diagnostic
  prints behind a `static std::unordered_set<std::string>` so each unique
  call signature prints at most once per process run.

---

## GUI debugging session — show/hide rows on combo selection

This section records the investigation that followed the introduction of
the degree-slider and control-points-slider rows in `fit_gui.rut`.

### Symptom

After adding a degree slider for polynomial fits and keeping the control-points
slider for spline mode, both slider rows were always visible regardless of which
fit function was selected in the combo box.

### Step 1 — add print markers

Added `(print "step N: ...")` between each major operation in the callback and at
the startup initialisation code. This quickly confirmed which functions were and
weren't being reached.

### Step 2 — read back the running definition

Used MCP eval with `(print on-combo-change)` to print the lambda actually bound
to the symbol at runtime.  `lval_print` renders lambdas as `(\ formals body)`,
so this shows exactly what rooture is running, not just what the source file says.
This is invaluable when a script has been partially reloaded or a def was silently
skipped.

### Step 3 — discover HideFrame timing issue

`(.HideFrame win row)` before `MapSubwindows` silently has no effect:
`TGCompositeFrame::HideFrame` calls `f->UnmapWindow()`, but windows that have
never been mapped have nothing to unmap, so `MapSubwindows` later maps them all
anyway.

**Fix**: call `HideFrame` (or equivalently, call the combo-change callback) *after*
`MapSubwindows` + `MapRaised`.  Wrapping the initial show/hide state in a zero-arg
function call after `MapRaised` is the correct pattern:

```scheme
(doto win {MapSubwindows} {Resize ...} {MapRaised})
(on-combo-change)   ; ← apply initial state now that windows are mapped
```

### Step 4 — discover signal argument not forwarded

After fixing the timing issue, the slider rows were hidden correctly on startup but
still did not appear when the user changed the combo selection.

The rooture `connect` shim is declared in Cling as `static void fire() { ... }` —
a slot with **no parameters**.  ROOT's signal/slot mechanism allows connecting a
signal with parameters (`Selected(Int_t)`) to a slot with fewer parameters; it
simply drops the trailing arguments.  The Int_t item-id never reaches rooture.

A lambda with formals `(\{id} ...)` bound to a zero-arg signal call undergoes
**partial application** — the body is never evaluated because not all formals are
bound.  The callback silently does nothing.

**Fix**: use a **0-formal lambda** for all `connect` callbacks; read widget state
from the widget itself rather than from the signal argument:

```scheme
; Wrong — id is never bound, body never runs:
(def {on-combo-change} (\{id} {do (if (== id 1.) ...) ...}))

; Correct — 0 formals, read state directly:
(def {on-combo-change} (\{} {do
  (= {sel} (.GetSelected combo))
  ...}))
```

This mirrors how `do-fit` reads the selection: `(= {sel} (.GetSelected combo))`.

> **Lesson**: signal parameters are not forwarded to rooture callbacks.  Always
> write callbacks as `(\{} {do ...})` and query widget state explicitly.

### Step 5 — simplify nested if

Earlier attempts used a nested `(if ... {do ... (if ...)})` pattern inside the
callback body.  While syntactically valid, nested `if`/`do` combinations are
difficult to read and error-prone.  Replacing with a flat sequence — always hide
both rows, then conditionally show the relevant one — made the logic robust:

```scheme
(def {on-combo-change} (\{} {do
  (.HideFrame win deg-row)
  (.HideFrame win npts-row)
  (if (== sel 1.) {.ShowFrame win deg-row}  {})
  (if (== sel 2.) {.ShowFrame win npts-row} {})
  (.Layout win)}))
```

---

## Interpreter bug: use-after-free in `builtin_ord` (LVAL_FLOAT case) caused a segfault
  inside `dotimes`.  The LVAL_FLOAT branch of the comparison operators
  (`>`, `<`, `>=`, `<=`) called `lval_del(a)` *before* reading
  `a->cell[0]->floating` and `a->cell[1]->floating`.  On macOS the freed
  memory usually survived intact in the system allocator — the comparisons
  returned correct results and the bug lay dormant.  It only manifested as a
  segfault inside a tight `dotimes` loop, where the allocator reused the freed
  cells quickly enough to corrupt the values before the access.

  The diagnosis sequence:
  1. Added step-by-step `(print "step N")` markers to `do-spline` in
     `fit_gui.rut`.  The crash consistently appeared after `"step6: nbins= 100"`,
     i.e., inside the chi² `dotimes` loop, not in spline construction.
  2. The ROOT signal handler printed a backtrace pointing to
     `builtin_ord` line 1717 — the floating comparison after the premature free.
  3. Fix: move `lval_del(a)` to *after* the `if/strcmp` comparisons, matching
     the LVAL_NUM branch pattern.

  The bug had no impact on purely integer comparisons (`LVAL_NUM` branch was
  correct), and the latent float case only triggered once the caching work
  made `(> data 0.)` inside a long `dotimes` loop feasible without excessive
  Cling overhead.

