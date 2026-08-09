# WS-C Topology-B zygote — patch-draft for nix-eval-jobs (DRAFT ONLY)

**Status:** DRAFT. No files modified, nothing built, nothing pushed, nothing run.
This is a review artifact. Every hunk is fork-safety-critical GC code; a human
reviews + applies + gates each line.

**Stamped at:**
- nix worktree `angerman/2.35-eval-profiling-v2` @ `2138ee9a1` (v3 engine + `cli/v3-eval.cc` fork prototype, `gc.cc`, `nursery.hh`, `aot_cache.cc`).
- nix-eval-jobs `2.34.1-v3` @ `4d4c0b4` (`src/nix-eval-jobs.cc`, `src/worker.cc`).

**Cross-branch caveat (load-bearing):** the v3 accessor surface `worker.cc`
links — `<v3/eval_jobs_api.hh>` providing `nix::v3::evalFlakeRoot` /
`evalExprRoot` / `descendAttrPath` / `isAttrs` / `isDerivation` /
`jobNeedsTreeWalker` / `childAttrNames` / `EvalJobsHandle{,Ptr}` — is **NOT in
the 2.35 worktree**; it lives on `input-output-hk/nix@angerman/2.34-v3`
(accessor commits `dbbb0abd8`, `847123ca6`, `7e5325fe4`, `4ae029f1e`). All
`nix::v3::*` handle calls below are quoted from `worker.cc` usage, not from a
header I could read. Signatures marked **[XB-VERIFY]** must be confirmed against
that branch before applying.

---

## 0. API facts established from source (with anchors)

| fact | anchor |
|---|---|
| `GC_DONT_GC=1` set first thing in nej main → Boehm never collects/moves/frees | `nix-eval-jobs.cc:592` |
| nej main: `initNix(); initGC(); flakeSettings.configureEvalSettings(...)` | `nix-eval-jobs.cc:610-612` |
| nej main spawns N **collector threads** (multithreaded from here on) | `nix-eval-jobs.cc:659-665` |
| `Proc` ctor forks a worker via `startProcess(..., {.allowVfork=false})` (plain fork), no GC_atfork bracket | `nix-eval-jobs.cc:120-156` |
| collector lazily creates its worker `Proc(worker)` **inside the thread** | `nix-eval-jobs.cc:460-461` |
| WorkerDied → respawn / tree-walker retry forks (`Proc(treeWalkerOnlyWorker)`) **inside the thread** | `nix-eval-jobs.cc:531` |
| `worker()` builds its **own** `EvalState` per process | `worker.cc:598-600` |
| worker engages v3: `setFlakeSettings` + `evaluateFlakeV3` / `evalExprRoot` | `worker.cc:655-683` |
| worker's per-job loop: `while (processJobRequest(...)) {}` | `worker.cc:733-736` |
| descent forces per-attr: `descendAttrPath(*v3Handle, path)` then `isAttrs`/`processDerivationV3` | `worker.cc:494-504` |
| `ensureTwRoot` / `rebuildV3` closures | `worker.cc:699-729` |
| v3-eval proven fork server: `GC_set_handle_fork(-1)` before `GC_INIT` | `cli/v3-eval.cc:376` (comment `:367-377`) |
| v3-eval brackets: `GC_atfork_prepare()` pre-fork; child calls `GC_atfork_child()` first; parent `GC_atfork_parent()` | `cli/v3-eval.cc:703`, `:709`, `:733`; fork-fail path also releases `:745` |
| v3-eval warms ONE parent then forks per request; child `_exit()`s, never re-enters parent loop | `cli/v3-eval.cc:634-645` (warm), `:705-729` (fork/child) |
| v3-eval keeps parent **single-threaded** for the fork (heap-trace sampler NOT started under fork modes) | `cli/v3-eval.cc:390-394` |
| nursery drain is a **member**: `bool Nursery::forceScavenge(VMState&)` — NOT a free `nix::v3::forceScavenge()` | `include/v3/nursery.hh:264`; impl `gc.cc:1595-1605` |
| `threadNursery()` returns `Nursery&` (per-thread) | `include/v3/nursery.hh:427` |
| `forceScavenge` returns false unless `enabled && base` (nursery must have been touched) | `gc.cc:1602` |
| `forceScavenge` preconditions: called at `exitDepth==0`; `activeVMStack` holds exactly the caller's `vm` | `include/v3/nursery.hh:254-258` |
| scavenge resets the bump buffer (memset + `next=base`) → inherited nursery is EMPTY post-drain | `include/v3/nursery.hh:268-289` |
| scavenge walks **global roots** (so a `frames.empty()` VMState is sufficient to keep the base alive): builtins, import-cache, applied-cache, call-flake, deep-force, standalone-cells, Phase-D dirty set | `gc.cc:766-808`; explicit `frames.empty()` support noted `gc.cc:783-792` |
| scavenge also walks `vm.frames` of the passed VMState | `gc.cc:640, 728, 1161` |
| Lever-1 AOT: `aot_cache::init()` reads `NIX_V3_AOT_CACHE_FILE`, mmap `MAP_PRIVATE PROT_READ`, runs eagerly at top of `runRootExprFromString` | `aot_cache.cc:102`, `:221-222` (+ plan §2.1) |

**Consequence for the drain:** because the warm base graph is reachable from
the process-global roots the scavenger already walks (`walkImportCacheRoots`
`gc.cc:772`, plus the handle's rooted slot — see [XB-VERIFY-GCROOT] below), a
freshly-constructed empty-frames `VMState` is a *sufficient* argument to
`forceScavenge`: it forwards every young base survivor into non-moving tenured
and leaves the nursery empty. This is the same shape as the post-eval-print
scavenge the CLI already relies on (`gc.cc:783`).

**[XB-VERIFY-GCROOT]:** the `EvalJobsHandle` roots its root value via a `GcRoot`
registry (`worker.cc:713` references `GcRoot::~GcRoot` erasing its own entry).
The 2026-07-20 deploy review's memory note records a **fixed** deploy-killer:
"GcRoot registry invisible to minor scavenge (gc.cc walk)". Confirm on the
2.34-v3 branch that `walkGlobalV3Roots`/`scavengeNursery` walks the GcRoot
registry — otherwise the warm base held only by the handle would be dropped by
the pre-fork `forceScavenge`. This walk is not present in the 2.35 worktree's
`gc.cc` because the handle surface is not here.

---

## 1. Shape of the change

Split `worker.cc:worker()` into three pieces and hoist the warm state into the
parent:

1. `warmParentV3(WarmCtx&, MyArgs&)` — **runs ONCE in the single-threaded
   parent.** Body = current `worker.cc:598-729` (build `EvalState`, engage v3,
   set up `ensureTwRoot`/`rebuildV3`) **minus** the `processJobRequest` loop,
   **plus** (a) force the handle root to WHNF (do NOT descend per-job), and (b)
   `threadNursery().forceScavenge(vm)` to drain the nursery.
2. `workerChildLoop(WarmCtx&, MyArgs&, toFd, fromFd)` — **runs in each pre-forked
   child.** Body = current `worker.cc:731-740` (the `LineReader` + descent loop),
   operating on the **inherited (CoW) warm `WarmCtx`** — it does NOT rebuild
   `EvalState` or re-engage v3.
3. `worker()` (unchanged, cold) — retained ONLY for the tree-walker-only respawn
   worker (`nix-eval-jobs.cc:442-447`), which must build its own cold state.

`main()` gains a warm-then-prefork phase **before** the collector-thread spawn.

`WarmCtx` is a single process-global the parent fills and every forked child
inherits by CoW:

```c++
// worker.hh (new) — or a detail header shared by main + worker TU.
struct WarmCtx {
    nix::ref<nix::EvalState>     state;      // built once in the parent
    nix::Bindings *              autoArgs = nullptr;
    nix::v3::EvalJobsHandlePtr   v3Handle;   // warm, WHNF-forced root  [XB-VERIFY]
    nix::Value *                 vRoot = nullptr;  // lazy TW parity root
};
```

---

## 2. Diff — `src/nix-eval-jobs.cc`

### 2a. Fork-safety init: `GC_set_handle_fork(-1)` BEFORE `initGC()`

Anchor: top of `main()`, around the existing `GC_DONT_GC` setenv (`:590-611`).

```diff
 auto main(int argc, char **argv) -> int {
     /* We are doing the garbage collection by killing forks */
     setenv("GC_DONT_GC", "1", 1); // NOLINT(concurrency-mt-unsafe)
+
+#if NIX_USE_BOEHMGC
+    // [GC-SAFETY 1] Port of cli/v3-eval.cc:376. MUST precede GC_INIT (which
+    // nix::initGC() runs at :611) or it has no effect. -1 = do NOT auto-install
+    // Boehm's pthread_atfork handlers; we bracket forks manually with
+    // GC_atfork_prepare/child/parent instead.
+    //
+    // [GC-SAFETY 1a — REVIEW DECISION, see checklist C1] -1 is PROCESS-GLOBAL:
+    // it disables Boehm's atfork for EVERY fork in nej, not just the pre-fork.
+    // nej still forks LAZILY from collector threads on respawn (:461) and
+    // tree-walker retry (:531). With -1, those forks are no longer
+    // Boehm-atfork-protected unless ALSO bracketed. Two acceptable resolutions:
+    //   (A) keep -1 and bracket EVERY fork site (this diff brackets the pre-fork;
+    //       the respawn/tree-walker sites in Proc must be bracketed too — 2c);
+    //   (B) use GC_set_handle_fork(1) so Boehm auto-brackets ALL forks (pre-fork
+    //       AND lazy respawn). On the LINUX deploy target (1) is safe and is the
+    //       lower-risk choice given nej's multiple fork sites; v3-eval chose -1
+    //       only because Darwin can abort when INSTALLING the handlers.
+    // Recommendation: (B) on Linux. Draft below shows (A) as the literal port
+    // requested; the human picks. Whichever is chosen, it is one line here.
+    GC_set_handle_fork(-1);
+#endif

     curl_global_init(CURL_GLOBAL_ALL);
     ...
     return nix::handleExceptions(args[0], [&]() -> void {
         nix::initNix();
         nix::initGC();               // GC_INIT — after the set_handle_fork above
         nix::flakeSettings.configureEvalSettings(nix::evalSettings);
```

`#include <gc/gc.h>` (guarded by `NIX_USE_BOEHMGC`) must be added to the include
block — nej does not currently include it directly.

### 2b. Warm the parent + drain the nursery, BEFORE spawning threads

Anchor: insert between the eval-store pre-open (`:654-656`) and the collector
thread loop (`:658-665`).

```diff
         if (myArgs.evalStoreUrl.has_value()) {
             nix_eval_jobs::openStore(myArgs.evalStoreUrl);
         }
+
+        // ============================================================
+        // [WS-C Topology B] Single-threaded warm-parent + pre-fork.
+        // The parent is STILL SINGLE-THREADED here (collector threads are
+        // spawned below). This is the invariant that makes the fork safe.
+        // ============================================================
+        static WarmCtx warm{ /* state built inside warmParentV3 */ };
+        const bool zygote = envFlagEnabled("NIX_V3_DIRECT_EVAL")
+                         && !envFlagEnabled("NIX_EVAL_JOBS_NO_ZYGOTE"); // kill-switch
+
+        std::vector<std::unique_ptr<Proc>> preforked;   // one per worker
+        if (zygote) {
+            // 1+2. Build+engage+WHNF-warm the shared EvalState, then drain the
+            //      nursery into non-moving tenured (stable addresses across fork).
+            //      Runs in THIS single thread. See worker.cc warmParentV3.
+            warmParentV3(warm, myArgs);
+
+            // 4. Pre-fork the N worker processes HERE (single-threaded parent).
+            //    Each inherits `warm` (EvalState + warm v3 handle + tenured base
+            //    graph + empty nursery) copy-on-write and REUSES it — no per-worker
+            //    base re-eval / re-parse / re-lower. Each Proc brackets its fork
+            //    with GC_atfork_* (see 2c).
+            preforked.reserve(myArgs.nrWorkers);
+            for (size_t i = 0; i < myArgs.nrWorkers; i++) {
+                preforked.emplace_back(std::make_unique<Proc>(
+                    // Processor: child runs the loop against the inherited warm ctx.
+                    [](MyArgs &a, nix::AutoCloseFD &toFd, nix::AutoCloseFD &fromFd) {
+                        workerChildLoop(warm, a, toFd, fromFd);
+                    }));
+            }
+        }
```

### 2c. Bracket the fork inside `Proc` (belt for resolution A)

Anchor: `Proc` ctor `:120-156`. Add GC_atfork brackets around `startProcess`
and make the child call `GC_atfork_child()` FIRST.

```diff
     explicit Proc(const Processor &proc) {
         nix::Pipe toPipe;
         nix::Pipe fromPipe;
         toPipe.create();
         fromPipe.create();
+#if NIX_USE_BOEHMGC
+        // [GC-SAFETY 2] Port of cli/v3-eval.cc:703-733. Acquire the GC/alloc
+        // lock BEFORE the fork inside startProcess. Because GC_set_handle_fork(-1)
+        // disabled the auto handlers, we do this by hand. NOTE: startProcess
+        // forks INTERNALLY, so this prepare brackets the fork only approximately
+        // (startProcess may run a little code between here and the actual fork,
+        // and in the child before our lambda). Under GC_DONT_GC + single-threaded
+        // pre-fork this is safe (no sibling holds the lock; no marker threads).
+        // For the LAZY respawn/tree-walker forks (multithreaded caller) this
+        // approximation is the C1 risk — see checklist. If tighter bracketing is
+        // required, hand-roll the fork in main() like v3-eval instead of
+        // startProcess (alternative in RISKS §R1).
+        GC_atfork_prepare();
+#endif
         auto childPid = startProcess(
             [&,
              toFd{...}, fromFd{...}]() -> void {
+#if NIX_USE_BOEHMGC
+                GC_atfork_child();   // FIRST thing in the child, before any alloc
+#endif
                 nix::logger->log(nix::lvlDebug, ...);
                 try { proc(myArgs, *toFd, *fromFd); }
                 catch (nix::Error &e) { ... }
             },
             nix::ProcessOptions{.allowVfork = false});
+#if NIX_USE_BOEHMGC
+        GC_atfork_parent();          // release in the parent, on every path
+#endif

         to = std::move(toPipe.writeSide);
         from = std::move(fromPipe.readSide);
         pid = childPid;
     }
```

> **[GC-SAFETY 2 caveat]** `startProcess`'s internal fork means the prepare/child
> brackets are not razor-tight the way v3-eval's are (v3-eval calls `fork()`
> directly at `:705`). The single-threaded pre-fork makes this a non-issue for
> the zygote workers. The pre-existing multithreaded respawn/tree-walker forks
> are the only place it matters; resolution (B) `GC_set_handle_fork(1)` sidesteps
> the whole concern by keeping Boehm's own (correct, tight) atfork handlers.

### 2d. Drive the pre-forked pool from the collector threads

Anchor: the thread-spawn loop `:659-665` and `collector()` `:449-561`.

The existing collector protocol (`checkWorkerStatus`/`getNextJob`/
`processWorkerResponse`/`updateJobQueue`) is **unchanged**. Only worker
*ownership* changes: instead of each collector lazily building
`proc_ = std::make_unique<Proc>(worker)` (`:460-461`), it is **handed** its
pre-forked `Proc*`.

```diff
         std::vector<Thread> threads;
         std::condition_variable wakeup;
         threads.reserve(myArgs.nrWorkers);
         for (size_t i = 0; i < myArgs.nrWorkers; i++) {
-            threads.emplace_back(
-                [&state_, &wakeup] -> void { collector(state_, wakeup); });
+            Proc * pf = zygote ? preforked[i].get() : nullptr;
+            threads.emplace_back(
+                [&state_, &wakeup, pf] -> void { collector(state_, wakeup, pf); });
         }
```

`collector()` change (`:449-461`): take the pre-forked `Proc*`; adopt it instead
of forking.

```diff
-void collector(nix::Sync<State> &state_, std::condition_variable &wakeup) {
+void collector(nix::Sync<State> &state_, std::condition_variable &wakeup,
+               Proc *prefork /* nullptr = legacy lazy-fork path */) {
     try {
         std::optional<std::unique_ptr<Proc>> proc_;
+        // Non-owning handle to a pre-forked zygote worker. When set, the
+        // collector drives it and does NOT own its lifetime (owned by main's
+        // `preforked` vector). proc_ stays empty in that case; only the
+        // respawn/tree-walker paths below allocate an owned Proc.
+        Proc * activeProc = prefork;
         std::optional<std::unique_ptr<LineReader>> fromReader_;
         int idleDeaths = 0;
         while (true) {
-            if (!proc_.has_value()) {
+            if (activeProc == nullptr && !proc_.has_value()) {
                 proc_ = std::make_unique<Proc>(worker);   // legacy cold path
+                activeProc = proc_->get();
             }
             if (!fromReader_.has_value()) {
-                fromReader_ =
-                    std::make_unique<LineReader>(proc_.value()->from.release());
+                fromReader_ =
+                    std::make_unique<LineReader>(activeProc->from.release());
             }
             ...
```

Every subsequent `proc_.value().get()` in `collector` (`:470`, `:497`, `:505`,
`:509`, `:513`) becomes `activeProc`. The two RESET points — `line=="restart"`
(`:490-493`) and `WorkerDied` (`:528-529`) — need Topology-B semantics:

```diff
             if (line == "restart") {
-                proc_ = std::nullopt;
-                fromReader_ = std::nullopt;
+                // [ZYGOTE] shouldRestart (worker.cc:588,shouldRestart) fires on
+                // RSS. A pre-forked worker cannot be cheaply re-warmed (re-fork
+                // must come from main, single-threaded). Draft policy: on a
+                // zygote worker, treat restart as end-of-worker (its share of the
+                // queue drains via siblings) OR fall back to a cold lazy re-fork
+                // (multithreaded — C1 risk). MUST be decided (RISK R3).
+                fromReader_ = std::nullopt;
+                if (prefork) { activeProc = nullptr; return; /* draft: retire */ }
+                proc_ = std::nullopt; activeProc = nullptr;
                 continue;
             }
```

The `WorkerDied` retry (`:515-552`) already forks a cold
`Proc(treeWalkerOnlyWorker)` from the collector thread — this is a
**multithreaded fork** and is a pre-existing property of nej, now interacting
with `GC_set_handle_fork(-1)` (C1). It is unchanged in behavior but MUST be
bracketed (2c already brackets all `Proc` forks) or covered by resolution (B).

> **Simplification note (recommended):** if per-worker RSS-restart and lazy
> respawn are dropped for zygote workers (a dead zygote worker's attrPaths retry
> on the cold tree-walker path, then that worker stays down), Topology B needs
> **zero** multithreaded forks except the rare tree-walker retry. That is the
> cleanest safety posture. Flagged as a design choice for the human, not baked in.

---

## 3. Diff — `src/worker.cc`

### 3a. Extract `warmParentV3` (parent-only) from `worker()`

`worker()` today: `:593-741`. Split so `:598-729` become `warmParentV3`, adding
the WHNF force + drain. Skeleton:

```c++
// [WS-C] Runs ONCE in the single-threaded parent. Builds the shared EvalState,
// engages v3, forces the flake root to WHNF (NOT per-job), and drains the
// nursery so the inherited image is fully tenured (stable addresses) + empty
// nursery. Populates `warm` for the forked children to inherit CoW.
void warmParentV3(WarmCtx &warm, MyArgs &args) {
    auto evalStore = nix_eval_jobs::openStore(args.evalStoreUrl);
    warm.state = nix::make_ref<nix::EvalState>(               // was worker.cc:599
        args.lookupPath, evalStore, nix::fetchSettings, nix::evalSettings);
    warm.autoArgs = args.getAutoArgs(*warm.state);            // was :601

    const bool v3Direct  = envFlagEnabled("NIX_V3_DIRECT_EVAL");   // :611
    const bool v3Require = envFlagEnabled("NIX_V3_REQUIRE");       // :612
    // ... lockFlagsV3Compatible (:623-627), v3ExprShape (:645), v3ApplyOk (:653)
    //     copied verbatim ...

    if (v3Direct && v3ApplyOk && ((args.flake && lockFlagsV3Compatible)
                                  || v3ExprShape)) {
        nix::v3::setFlakeSettings(&nix::flakeSettings);           // :661
        try {
            warm.v3Handle = args.flake
                ? evaluateFlakeV3(warm.state, args.releaseExpr,
                                  args.lockFlags, args.selectExpr)  // :664
                : nix::v3::evalExprRoot(*warm.state, args.releaseExpr,
                                        args.selectExpr);            // :669  [XB-VERIFY]
        } catch (const std::exception &e) {
            if (v3Require) throw;                                    // :675
            warm.v3Handle.reset();
        }
    }
    if (v3Require && !warm.v3Handle)
        throw nix::Error("NIX_V3_REQUIRE=1 but v3-direct did not engage");  // :685-690
    if (!warm.v3Handle)
        warm.vRoot = initializeRootValue(warm.state, *warm.autoArgs, args);  // :693

    // ---- [WS-C step 1] Force the flake root to WHNF, but DO NOT descend. ----
    // Each child will descendAttrPath into ONLY its own jobs, forcing only its
    // own per-job thunks. Forcing the top-level attrset to WHNF in the parent is
    // what makes the *shared* base (attrset spine + import-cache value graph +
    // unforced per-job thunks + their captured upvalues) live BEFORE the drain.
    if (warm.v3Handle) {
        // [XB-VERIFY] the exact "force root to WHNF" call. `isAttrs(*v3Handle)`
        // forces the handle root (worker.cc:498 uses it after descent); confirm
        // it forces the ROOT (pre-descent) on the 2.34-v3 branch. Do NOT call
        // descendAttrPath here.
        (void) nix::v3::isAttrs(*warm.v3Handle);
    }

    // ---- [WS-C step 2] Drain the nursery into non-moving tenured. ----
    // forceScavenge is a Nursery MEMBER needing a VMState. An empty-frames
    // VMState suffices: the warm base is reachable from process-global roots the
    // scavenger walks (gc.cc:766-808), incl. import-cache (gc.cc:772) and the
    // handle's GcRoot slot ([XB-VERIFY-GCROOT]). Preconditions (nursery.hh:254):
    // exitDepth==0 and activeVMStack holds exactly this vm — true here (quiescent,
    // single-threaded, no nested VMState). Returns false (no-op) if the nursery
    // was never touched (enabled && base, gc.cc:1602) — harmless.
    nix::v3::VMState drainVm;                        // empty frames
    // [XB-VERIFY] ensure drainVm is the sole entry on tlActiveVMStack here (push
    // if the API requires it; the CLI's forceValue paths push a synthetic frame).
    nix::v3::threadNursery().forceScavenge(drainVm);
}
```

### 3b. Extract `workerChildLoop` (child-only) from `worker()`

`:731-740` become the child body. It reconstructs the two parity closures over
the inherited `warm` (cheap; each child gets private CoW copies) and enters the
existing loop unchanged.

```c++
// [WS-C] Runs in each pre-forked child. Uses the INHERITED (CoW) warm ctx —
// does NOT rebuild EvalState or re-engage v3. Byte-identical descent to the
// pre-zygote worker() loop, just against a pre-warmed handle.
void workerChildLoop(WarmCtx &warm, MyArgs &args,
                     nix::AutoCloseFD &toParent, nix::AutoCloseFD &fromParent) {
    auto ensureTwRoot = [&]() -> nix::Value * {          // was worker.cc:699-706
        if (warm.vRoot == nullptr)
            warm.vRoot = initializeRootValue(warm.state, *warm.autoArgs, args);
        return warm.vRoot;
    };
    auto rebuildV3 = [&]() noexcept {                    // was worker.cc:716-729
        try {
            warm.v3Handle.reset();
            warm.v3Handle = args.flake
                ? evaluateFlakeV3(warm.state, args.releaseExpr, args.lockFlags,
                                  args.selectExpr)
                : nix::v3::evalExprRoot(*warm.state, args.releaseExpr,
                                        args.selectExpr);
        } catch (...) { warm.v3Handle.reset(); }
    };
    LineReader fromReader(fromParent.release());          // :731
    while (processJobRequest(*warm.state, fromReader, toParent, *warm.autoArgs,
                             ensureTwRoot, warm.v3Handle, rebuildV3, args)) {}  // :733
    (void) tryWriteLine(toParent.get(), "restart");       // :738
}
```

> **[GC-SAFETY 3]** `rebuildV3` in a child mutates only the child's CoW-private
> `warm.v3Handle` + arena (thunk memoisation) — never the parent's. `_exit()`
> is not used here because the child runs a persistent loop (unlike v3-eval's
> per-request child); instead the child never returns into `main`'s pre-fork
> site (it entered via the `Proc` Processor lambda and exits the process at loop
> end). Confirm the Processor lambda path exits the child process rather than
> returning to `main` (it does: `startProcess` children do not return).

### 3c. `worker()` retained cold

Keep the original `worker()` (`:593-741`) intact for `treeWalkerOnlyWorker`
(`nix-eval-jobs.cc:442-447`) and the legacy non-zygote path (`zygote==false`).

---

## 4. Lever-1 (AOT) plug-in — NOTE ONLY

No nej code change. The warm parent calls `evalFlakeRoot`/`evalExprRoot` →
`runRootExprFromString`, at whose top `aot_cache::init()` runs eagerly
(`aot_cache.cc:221-222`) and reads `NIX_V3_AOT_CACHE_FILE` (`aot_cache.cc:102`),
mmap-ing the base bytecode `MAP_PRIVATE PROT_READ`. Set on the workers'
environment (systemd unit): `NIX_V3_AOT_CACHE_FILE=/var/cache/hydra/v3-aot/current.aot`
(plan §2.1). It composes with the zygote: the parent maps the AOT once
(file-backed `Shared_Clean`, inherited read-only by every child); the fork then
shares the *evaluated* graph on top via CoW. **[XB-VERIFY-Q3]** confirm the nej
`evalFlakeRoot`/`evalExprRoot` route actually reaches `runRootExprFromString`
(plan open-Q3) — Lever-1's engagement on the nej path depends on it. If not,
`aot_cache::init()` must be invoked explicitly once in `warmParentV3` before the
first eval.

---

## 5. GC-SAFETY CHECKLIST (human MUST verify each)

- **[C1] `GC_set_handle_fork` mode vs nej's multiple fork sites.** `-1`
  (`nix-eval-jobs.cc` 2a) disables Boehm atfork **process-wide**, incl. the lazy
  respawn fork (`:461`) and tree-walker-retry fork (`:531`) that run from
  collector THREADS. Verify EITHER every fork site is bracketed (2c brackets all
  `Proc` forks) OR switch to `GC_set_handle_fork(1)`. On the Linux deploy target,
  (1) is the lower-risk choice. **This is the single most important decision.**
- **[C2] `set_handle_fork` precedes `GC_INIT`.** It must be called before
  `nix::initGC()` (`:611`) or it is a silent no-op (mirrors `cli/v3-eval.cc:374`
  rationale). Confirm ordering in `main`.
- **[C3] Nursery drained before the pool fork.** `warmParentV3` calls
  `threadNursery().forceScavenge(drainVm)` AFTER the WHNF force and BEFORE any
  `Proc` fork. Verify (a) it returns true (nursery was touched → base actually
  tenured; if false, confirm base never used the nursery), and (b) no allocation
  happens between the drain and the fork (would re-populate the nursery →
  young/movable object crosses the fork). Draft ordering satisfies this: drain is
  the last statement of `warmParentV3`; the fork loop is the next statement in
  `main`.
- **[C4] `forceScavenge` preconditions hold at the drain.** `exitDepth==0` and
  `tlActiveVMStack` holds exactly `drainVm` (nursery.hh:254-258). Parent is
  single-threaded + quiescent post-warm. Confirm `drainVm` is pushed as the sole
  active VMState if the API requires it. **[XB-VERIFY]**
- **[C5] The warm base survives the drain.** Confirm the scavenger walks the
  `EvalJobsHandle` GcRoot registry ([XB-VERIFY-GCROOT]); otherwise the base held
  only by the handle is dropped. `walkImportCacheRoots` (`gc.cc:772`) covers the
  import-cache value graph; the handle root is the gap to verify.
- **[C6] Tenured is non-moving / stable-address after fork.** Guaranteed by the
  arena design (plan §3.1.2; `alloc.hh` calloc/mmap, no compaction) + `GC_DONT_GC`
  (`:592`). No code path moves a tenured object post-fork. Confirm no gen-major
  *compaction* is reachable (there is none: mark-sweep tenured is non-moving).
- **[C7] No movable/young object crosses the fork.** Follows from C3+C6: after
  the drain the nursery is empty (`nursery.hh:287-288` memset+reset) and every
  live base object is tenured. Each child's own allocations go to its private
  (CoW) nursery/arena.
- **[C8] Children never mutate the shared image in a CoW-breaking-but-incorrect
  way.** Legal mutations are thunk memoisation (Blackhole→Evaluated) + IC updates
  + Phase-D dirty-set writes — each CoW-copies its page **private** to the child
  (plan §3.2). Verify no child writes are expected to be *observed* by the parent
  or a sibling (they are not: independent per-job descent). Verify `rebuildV3` in
  a child only touches child-private state (3b [GC-SAFETY 3]).
- **[C9] Collector threads spawn strictly AFTER the fork.** In `main`,
  `warmParentV3` + the `preforked` loop complete before the
  `threads.emplace_back(collector...)` loop. Verify no thread (incl. any nix/curl
  background thread) is live at pre-fork time. Note `curl_global_init` (`:605`)
  runs in the parent before fork (intended); confirm it spawns no persistent
  thread. Confirm the v3 heap-trace sampler is NOT started (nej never starts it;
  contrast `cli/v3-eval.cc:390-394`).
- **[C10] Phase-D remembered set / thread_local roots are per-process.**
  `dirtyContainers`, `standaloneCellRoots`, `singletonClosureRegistry` are
  `thread_local` (plan §3.1.5); a fork gives each child private CoW copies.
  Verify the pre-fork parent's dirty set is empty/consistent (post-drain the
  scavenger drained it, `gc.cc:797-808`).
- **[C11] Boehm inert in both parent and child.** `GC_DONT_GC=1` (`:592`) → no
  collection, move, or free anywhere in the tree. Confirm nothing in the zygote
  path re-enables GC (no `GC_enable`, no `GC_gcollect`).
- **[C12] Fork-fail path releases the GC lock.** In `Proc` (2c) confirm
  `GC_atfork_parent()` runs on the fork-failure path too (mirrors
  `cli/v3-eval.cc:745`). `startProcess` throws on failure → the `#if
  NIX_USE_BOEHMGC GC_atfork_parent()` after it would be skipped; wrap in a
  try/catch or RAII guard so the lock is always released.

---

## 6. RISKS + OPEN QUESTIONS

- **[R1] `startProcess` internal fork vs tight bracketing.** nej forks via
  nix's `startProcess`, not a direct `fork()`, so the GC_atfork brackets (2c) are
  approximate (code runs between prepare and the real fork, and in the child
  before `GC_atfork_child`). Single-threaded pre-fork makes this safe for zygote
  workers; the multithreaded respawn/tree-walker forks are the exposure.
  **Alternative:** hand-roll the pre-fork in `main` exactly like
  `cli/v3-eval.cc:688-705` (explicit `pipe()` + `GC_atfork_prepare` + `fork()` +
  child `GC_atfork_child` first) and build a `Proc`-equivalent by hand — tighter,
  but loses `startProcess`'s process plumbing (signal reset etc.). Trade-off for
  the human.
- **[R2] Does nej's flake eval produce ONE warmable base, or per-attr?**
  hydra passes `--expr 'let flake = builtins.getFlake "<locked-url>"; in
  flake.hydraJobs or flake.checks or (throw …)'` (worker.cc:635-645). That single
  top-level attrset IS the common base every job descends — so YES, one warmable
  base. K1/K2 (plan §"WS-C BUILD SPEC", base = 34-46% of warm wall; per-child
  private well under 70%) already sized this on a nixpkgs proxy. Confirm the
  haskell.nix shape (cabalProject machinery) warms to the same single WHNF root.
- **[R3] Does reusing ONE EvalState across pre-forked workers need per-child
  re-init?** The `EvalState` is created once in the parent and inherited CoW.
  Open: (a) any `EvalState` field that is a live FD / socket / RNG / cache handle
  that must NOT be shared post-fork (store connection: nej pre-opens the eval
  store `:654-656` deliberately so children share the SQLite schema — but the
  *connection* may need reopening per child; the pre-zygote worker opened its own
  store at `worker.cc:598`). **Verify whether `EvalState`'s store connection is
  fork-safe to share or must be reopened in each `workerChildLoop`.** (b)
  per-worker RSS accounting (`shouldRestart`, `worker.cc:588`) is per-process
  already — fine.
- **[R4] RSS-restart / respawn semantics for a pre-forked worker.** A zygote
  worker cannot cheaply re-warm itself (re-fork must originate single-threaded
  from `main`). Draft retires the worker on restart (2d). Confirm the queue
  drains correctly when a worker retires mid-run (siblings pick up its `todo`;
  its `active` attrPath is the concern — mirror the WorkerDied retry).
- **[R5] Darwin vs Linux fork semantics.** The correctness gate must be on
  LINUX (deploy platform; real CoW smaps + `startProcess` fork). `-1` was a
  Darwin workaround; on Linux `1` (auto-handlers) is available and safer (C1).
- **[R6] `NIX_EVAL_JOBS_NO_ZYGOTE` kill-switch** (2b) — add with an inline
  retirement criterion (v3 §"Critical constraints" rule 4: no env gate without a
  retirement criterion in the comment at first read). Draft: "retire once the
  zygote pool is the sole path and gated green on the farm."

---

## 7. GATE RECIPE (mandatory; faithful = Linux)

1. **Correctness — full `--brute` UNDER the forked pool.** The v3 gate
   (`src/libexpr-v3/CLAUDE.md` pre-merge): `nix develop -c bash
   src/libexpr-v3/test/all-v3-tests.sh --brute` — expect `22/22 ALL GREEN` (1 MB
   nursery + `V3_DBG_NURSERY_AUDIT=1 V3_DBG_NURSERY_BRUTE=1`). This exercises the
   v3 engine; additionally run a nej-level brute that drives the *zygote pool*
   (fork N workers, run the firefox/hello/git/gcc jobset through the pipe
   protocol) so the fork path itself is under the moving-GC stress.
2. **drvPath byte-identity vs TW, under the forked pool.** Run the same jobset
   through (a) stock tree-walker nej (`NIX_V3_DIRECT_EVAL=0`) and (b) the
   Topology-B zygote pool; diff every emitted `drvPath` (and the full response
   JSON). Byte-identical or KILL (K3). Use a pinned nixpkgs
   (`test/nixpkgs-pin.sh`) so the golden is stable.
3. **Per-child density (Linux smaps).** Confirm each worker's `Private_Dirty`
   (from `/proc/self/smaps_rollup`) is materially below a cold worker's RSS and
   `Shared_Clean` reflects the shared base (K2 < 70% of fresh). Reuse the
   `--cow-fork` smaps harness pattern (`cli/v3-eval.cc:124-149, 545-595`).
4. **No worker OOM-restart storm** across a full cardano-node haskell.nix
   jobset (plan Phase-2 gate).
5. **Kill-switch parity:** `NIX_EVAL_JOBS_NO_ZYGOTE=1` (or `NIX_V3_DIRECT_EVAL=0`)
   reproduces the pre-zygote pool byte-for-byte.

**KILL (K3):** fork drvPath divergence that is not a fixable missed-root/barrier
bug → fall back to Lever-1 (AOT) only + optional robustness-only per-worker
fork-server (Topology A).

**Discipline:** nej repo + deploy infra → needs explicit human OK before any
push; the fork-pool gate wants the Linux farm.


Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
