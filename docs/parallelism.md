# Parallelism in ROOTure

ROOTure exposes Clojure-style concurrency primitives — `atom`, `future`,
`pmap`, `pfilter` — adapted to the constraints of a ROOT/Cling environment
where the interpreter itself is single-threaded.

---

## Primitives and Clojure comparison

| ROOTure | Clojure | Notes |
|---------|---------|-------|
| `(atom val)` | `(atom val)` | identical semantics |
| `(deref a)` | `@a` or `(deref a)` | `@` is taken by ROOT object lookup in rooture |
| `(reset! a v)` | `(reset! a v)` | identical |
| `(swap! a f args...)` | `(swap! a f args...)` | identical |
| `(future {body})` | `(future body)` | Q-expression instead of implicit do-body |
| `(realized? f)` | `(realized? f)` | identical |
| `(deref f ms default)` | `(deref f ms default)` | identical |
| `(pmap f l)` | `(pmap f coll)` | returns a fully-realised list, not a lazy seq |
| `(pfilter f l)` | — | not in Clojure stdlib; added for symmetry |
| `(parallelism)` | — | query thread-pool size |
| `(set-parallelism! n)` | — | resize pool |

The main semantic difference from Clojure is that `pmap` and `pfilter` are
**strict**: they block until all futures are realised before returning.
Clojure's `pmap` returns a lazy sequence that realises in chunks.  The strict
behaviour is simpler to implement correctly and is appropriate for batch
analysis workloads where you want all results before proceeding.

---

## The Cling thread-safety constraint

ROOT's interpreter (Cling) is **not thread-safe**.  All calls to
`gInterpreter->Calc`, `ProcessLine`, and `Declare` must happen on the **main
thread** — the thread that owns `gSystem->Run()` and the ROOT event loop.
Calling Cling from a background thread causes silent data corruption or
crashes.

This constraint touches almost every rooture builtin: constructing objects
(`new`), calling methods (`.Method`), static calls (`::Method`), jit-fn
compilation, and ROOT callback connection all go through Cling.

### Solution: main-thread trampoline

Every builtin that touches Cling has a 5-line dispatch guard at the top:

```cpp
if (g_in_future) {
  lval* ac = lval_copy(a);
  lval* res = nullptr;
  rut_dispatch_work([&]{ res = builtin_X(e, ac); });
  lval_del(a);
  return res;
}
```

`g_in_future` is a `thread_local bool` set to `true` in every worker thread.
`rut_dispatch_work` posts a `ClingWork` item (a `std::function` +
`std::promise<void>`) to a lock-protected queue, writes a byte to
`g_cling_pipe` to wake the ROOT event loop, then blocks on the promise's
future until the main thread has executed the work.

On the main thread, a `TFileHandler` (`ClingPipeHandler`) registered with
`gSystem` drains the queue whenever the pipe becomes readable:

```
future thread                         main thread
─────────────────────────────────     ──────────────────────────────────
rut_dispatch_work(fn):                gSystem->Run() event loop:
  enqueue ClingWork{fn, promise}  ──►   ClingPipeHandler::ReadNotify()
  write 'C' to g_cling_pipe               drain g_cling_queue:
  future.get()  ◄────────────────           work.fn()          // Cling call
                                            work.done.set_value()
```

The waiting `builtin_deref` also pumps `rut_drain_cling_queue()` in its
1 ms sleep loop so it can service Cling requests from the future it is
waiting on, preventing deadlock.

### Consequence: futures compose safely with ROOT

A future body can freely call any rooture builtin — including `new`,
`.Method`, `::`, `jit-fn` — because the dispatch guard transparently routes
those calls to the main thread.  From the user's perspective, futures look
just like Clojure futures; the Cling constraint is invisible.

---

## Environment capture: `lenv_snapshot`

Rooture's interpreter is derived from the BYOL (Build Your Own Lisp) design,
where each lambda **re-uses its own `env` field** as the call environment:
formals are bound into `f->env` and `f->env->par` is set to the calling
environment at call time.  This is neither purely lexical nor purely dynamic
scoping — it is a mutation-based approach that works correctly for sequential
single-threaded execution.

### The dangling-pointer problem

When `(future {body})` is called inside a nested lambda (as happens inside
`pmap`), the naive `lenv_copy(e)` only copies the immediate environment level
and stores a raw pointer to the parent:

```
future_env = { x: 1,  par: inner_lambda->env  ←── raw pointer }
```

After `builtin_future` returns, `lval_eval_sexpr` calls `lval_del(f)` on the
inner lambda, freeing `inner_lambda->env`.  The future thread then crashes
dereferencing the dangling `par` pointer.

### Fix: `lenv_snapshot`

`builtin_future` uses `lenv_snapshot(e)` instead of `lenv_copy(e)`.
`lenv_snapshot` walks the **entire environment chain** from innermost to
outermost and copies all bindings into a single flat environment with no
parent pointer.  Outermost bindings are written first so inner bindings
(closer scope) win on collision.

```
chain:  inner_lambda->env → map->env → pmap->env → global_env
                                        ↑
                                 fn = user_lambda  (pmap's binding)

snapshot (flat):  { *, +, map, fn=user_lambda, f=inner_lambda, x=1, ... }
                                    ↑ pmap's fn wins over nothing
```

The future thread holds an entirely self-contained environment.  No raw
pointers into any lambda environment that might be freed.

### The name-shadowing constraint

Because the snapshot is flat, any symbol name used as a parameter in an
**intermediate** stdlib function (between the future creation site and the
user's binding) will shadow the user's binding.

`map` uses `f` as its callback parameter.  If `pmap` also used `f`, map's
binding (`f = inner_lambda`) would overwrite pmap's binding (`f = user_fn`)
in the snapshot, and every future would recursively spawn more futures instead
of calling the user's function.

**Convention:** parallel higher-order functions (`pmap`, `pfilter`) use `fn`
as their callback parameter name, which `map` does not use, avoiding the
collision.

---

## Thread pool

Each `(future ...)` call submits work to a fixed-size `RutThreadPool` instead
of spawning a raw `std::thread`.  The pool is sized to
`std::thread::hardware_concurrency()` at startup.

```
(pmap f {1 2 3 ... 1000})
```

Without a pool this would spawn 1000 threads simultaneously.  With the pool,
at most N tasks run concurrently (where N = logical CPU count); the rest queue
and are picked up as workers become free.

Worker threads set `g_in_future = true` **once** at startup (not per task),
since the thread-local flag is stable for the lifetime of each worker thread.

**Known limitation:** if all pool workers are blocked in `deref` waiting for
other futures (nested blocking deref), the pool deadlocks.  The workaround is
to avoid blocking deref inside a future body, or to use `timeout-deref` as a
safety valve.

---

## Summary of files changed

| File | What changed |
|------|-------------|
| `rooture.h` | `LVAL_ATOM`, `LVAL_FUTURE` type tags; `ClingWork`; `g_cling_pipe`; pool declarations |
| `rut_lang.cxx` | `RutAtom`, `RutFuture`, `lenv_snapshot`; all atom/future builtins; `lenv_snapshot` in `builtin_future` |
| `rut_root.cxx` | `rut_dispatch_work`, `rut_drain_cling_queue`, `rut_calc/process_line/declare` wrappers; `ClingPipeHandler`; `RutThreadPool`; dispatch guards in all Cling-touching builtins |
| `rut_jitfn.cxx` | `gInterpreter->Declare` → `rut_declare` |
| `rut_repl.cxx` | `g_cling_pipe` setup; pool creation/destruction |
| `stdlib.rut` | `pmap`, `pfilter`; `take`/`drop` nil-guard |
| `rut_repl.cxx` (parser) | `?` added to MPC symbol character class so `realized?` parses |
