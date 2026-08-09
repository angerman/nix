# WS-IFD-SOLVE — patch draft + A/B recipe (2026-08-09)

Scouting + draft only. No build, no farm, no push. Baseline to beat: **24.6 s cold
marginal solve** for cardano-node's 2051-pkg plan (`IFD_SOLVE_FINDINGS_2026-08-09.md:24,30`).

Correctness bar (both variants): the **resolved plan** (plan.json → downstream package
build derivations) must be **byte-identical drvPath**. A faster solve that changes the
plan is a KILL.

---

## 1. What `truncate-index` actually does — and variant (a) verdict

Source: `haskell.nix/nix-tools/nix-tools/truncate-index/Main.hs:22-30`:

```haskell
filterHaskellIndex orig indexState out =
  BS.writeFile out . nukeHeaderOS . GZip.compress . Tar.write . f . Tar.read . GZip.decompress =<< BS.readFile orig
  where f = filter ((<= ts) . Tar.entryTime) . toList
        ts = toEpochTime indexState
```

CLI (`--help`, and `args` parser Main.hs:46-50): **only three flags** — `-o/--output`,
`-i/--input`, `-s/--indexState` (`YYYY-MM-DDTHH:MM:SSZ`).

**So `truncate-index` does exactly ONE thing:** read the hackage `01-index.tar.gz`, drop
every tar entry whose `entryTime > indexState`, re-gzip. It prunes **purely by
index-state timestamp**. It has:
- NO package-set input, NO dependency-closure input, NO project awareness.
- NO notion of a "reachable candidate cone" — it cannot be handed the project's deps.

**It is ALREADY wired into the pipeline.** `overlays/haskell.nix:258` (inside `dotCabal`,
the function `call-cabal-project-to-nix.nix:692` calls to build `CABAL_DIR`):

```nix
postFetch = "${nix-tools}/bin/truncate-index -o $out -i $downloadedFile -s ${index-state}";
```

The index handed to `make-install-plan` is therefore **already** truncated to
`cached-index-state`, and the solver is **further** bounded by
`make-install-plan --index-state=${index-state}` (`call-cabal-project-to-nix.nix:700-707`).

### Variant (a) verdict: **KILL (WEAK — factually cannot do what the workstream asked)**

- The finding-doc premise ("`truncate-index` is NOT wired in", `IFD_SOLVE_FINDINGS:40`) is
  **incorrect**. It IS wired in (via `dotCabal`), but only as an **index-state truncator** —
  which is exactly what index-state pinning already is. It was conflated with a
  package-set pruner it never was.
- Truncating "to a reachable/smaller candidate set BEFORE make-install-plan" is **not a
  capability this tool has**. It can only truncate by timestamp, and that truncation is
  already applied. Running it again with the same `-s` is a no-op on the candidate space.
- **Chicken-and-egg** for the intended reachable-cone prune: to know the reachable
  package cone you must first run the solve — the very thing you're trying to cheapen.
- No pipeline patch is drafted for (a): there is nothing sound to wire that isn't already
  wired. (A hypothetical *new* tool that prunes by a pre-computed dep cone is a moonshot,
  not a `truncate-index` wiring, and would risk changing the plan if the cone is wrong.)

Conclusion: variant (a) = **KILL**. `truncate-index` prunes no more than index-state
pinning already does.

---

## 2. Variant (b) freeze-fed — the mechanism already exists

A cabal `cabal.project.freeze` is a full set of `constraints: pkg ==ver` (+ flag pins) for
the **entire transitive closure**. Fed to the solver, every version is pinned by `==`, so
the Modular solver's search collapses to ~one candidate per package → the solve becomes
**validation** (still runs, still errors if the pins are unsatisfiable → catches drift).
This matches `IFD_SOLVE_FINDINGS:59` ("if provided, the solve is ~trivial").

The end-to-end plumbing is **already present** — no new mechanism needed:

1. **Input**: `cabalProjectFreeze` arg (`call-cabal-project-to-nix.nix:15`), whose module
   default already **auto-reads a committed freeze**:
   `modules/cabal-project.nix:67` →
   `default = readIfExists config.evalSrc "${config.cabalProjectFileName}.freeze";`
   i.e. a `cabal.project.freeze` committed next to `cabal.project` is picked up for free.
2. **Feed**: if non-null it is copied into the build dir *before* the solve
   (`call-cabal-project-to-nix.nix:679-682`):
   ```nix
   ${pkgs.lib.optionalString (cabalProjectFreeze != null) ''
     cp ${evalPackages.writeText "cabal.project.freeze" cabalProjectFreeze} cabal.project.freeze
     chmod +w cabal.project.freeze
   ''}
   ```
3. **Output**: the *generated* freeze from a solve is exported as the second output
   `$freeze` (declared `:629-635`, written `:721 cp cabal.project.freeze $freeze`), surfaced
   as `passthru.freeze` (comment `:632`).

### Where the freeze is stored + keyed

The **sound key** is `(project source, index-state, ghc / compiler-nix-name, cabal.project
+ cabal.project.local content)` — everything the solver consumes. Two storage options:

- **(b-recommended) In-repo, committed.** Persist the generated freeze as
  `cabal.project.freeze` in the project source tree. Keying is then *implicit* in the repo
  content + the repo's pinned `index-state`. **Zero pipeline change** — the module default
  at `cabal-project.nix:67` already picks it up. This is the only form that speeds up a
  **cold** solve (the freeze is present *ahead of* the eval).
- **(b-auto) Out-of-band store cache keyed by the tuple above → re-fed as
  `cabalProjectFreeze`.** This is **chicken-and-egg / no cold win**: to produce the freeze
  you must run the full solve once; and once you've run it, the plan-nix **output is itself
  store-cached** (the warm 632× collapse already noted in project memory), so an automatic
  freeze cache buys **nothing** on repeat that the existing plan-nix store cache doesn't
  already buy. It cannot help the first cold solve because the freeze doesn't exist yet.

### Variant (b) verdict: **VIABLE as an ahead-of-time (committed) artifact; KILL as an auto-cache**

- Committing the freeze collapses the cold solve to validation and is fully supported today.
- It is the same soundness/ergonomics class as materialization (which was REJECTED), but
  **lighter**: one reviewable text file, and it *still runs the validating solve* so it
  catches index-state/dep drift instead of silently serving a stale plan.
- No genuine new pipeline patch is required for the win. The only optional pipeline
  nicety is a helper to *extract* the `freeze` output into the repo (see §3 patch).

---

## 3. Concrete patch drafts

### Variant (a): NO PATCH (KILL — see §1). `truncate-index` cannot prune beyond index-state, which is already applied at `overlays/haskell.nix:258` + `call-cabal-project-to-nix.nix:700-707`.

### Variant (b): the win needs NO code patch — it is a workflow + one committed file

Recommended, zero-diff path (per project, e.g. cardano-node):

```
# 1. Produce the freeze from ONE solve (the generated second output):
nix build .#<project>.plan-nix^freeze --no-link --print-out-paths
#    -> $freeze contains the full `constraints:` closure

# 2. Copy it into the project source, next to cabal.project, and commit:
cp <freeze-store-path> ./cabal.project.freeze
git add cabal.project.freeze

# 3. Next eval: modules/cabal-project.nix:67 readIfExists auto-feeds it.
#    make-install-plan (call-cabal-project-to-nix.nix:679-682) copies it in
#    before the solve -> solve collapses to validation.
```

Optional convenience patch (does NOT change eval semantics; only makes the freeze output
easier to grab). Against `call-cabal-project-to-nix.nix`, at the passthru block `:772-780`:

```diff
 in {
   projectNix = plan-json;
   inherit index-state-max src;
   inherit (fixedProject) sourceRepos extra-hackages;
+  # Generated cabal.project.freeze (the second output of plan-json). Feed this back
+  # via `cabalProjectFreeze` (or commit it as ./cabal.project.freeze — see
+  # modules/cabal-project.nix:67) to collapse the next cold solve to validation.
+  # WS-IFD-SOLVE variant (b). Sound key: (src, index-state, compiler-nix-name,
+  # cabal.project[.local]). drvPath-invariant iff the pins reproduce the free solve.
+  freeze = plan-json.freeze;
   rawCabalProjectContext = builtins.substring 0 0 rawCabalProject;
 }
```

(`plan-json` is the `materialize`-wrapped multi-output derivation `:608-615`; `.freeze`
selects the second output declared at `:634`. This only re-exports an existing output.)

**No auto-cache patch is drafted** — §2 (b-auto) shows it is chicken-and-egg with no cold
win over the existing plan-nix store cache.

---

## 4. A/B measurement recipe (against the 24.6 s baseline)

The plan-nix derivation is `projectNix` = the `plan-to-nix-pkgs` runCommand
(`call-cabal-project-to-nix.nix:615`, output name `*-plan-to-nix-pkgs`). Resolve the attr
to that drv (e.g. `<project>.plan-nix` in a haskell.nix project).

### Baseline (no freeze) — reproduce the 24.6 s number:
```
# warm all inputs first (build once), then force a cold re-solve of just this drv:
PLAN_DRV=$(nix path-to-derivation .#<project>.plan-nix 2>/dev/null || \
           nix eval --raw .#<project>.plan-nix.drvPath)
/usr/bin/time -l nix build "$PLAN_DRV^out" --rebuild --no-link -L
#   -L surfaces "Resolving dependencies..."; --rebuild forces the solve with inputs warm.
```

### Variant (b) arm — WITH a committed/fed freeze:
Because feeding a freeze adds a build input (the `writeText` at `:680`), the plan-nix
**.drv itself is DIFFERENT** — you cannot `--rebuild` the *same* drv across arms. Instead:
```
# with ./cabal.project.freeze committed (or cabalProjectFreeze passed):
PLAN_DRV_B=$(nix eval --raw .#<project>.plan-nix.drvPath)   # a NEW drv
/usr/bin/time -l nix build "$PLAN_DRV_B^out" --rebuild --no-link -L
```
Compare wall of the "Resolving dependencies..." phase (baseline ~24.6 s vs freeze arm).
Expect the freeze arm to collapse toward validation time. Keep-bar: material (>15%).

### drvPath-invariance check (the correctness gate) — MUST pass or KILL:
The plan-nix `.drv` differing is EXPECTED and harmless (it's the IFD input, and the freeze
is `rm`'d at `:723` + all non-`.nix` deleted at `:757`, so `$out` content is unaffected).
The bar is the **downstream resolved plan**. Verify byte-identity of the final build graph:
```
# same attr, both arms; compare the actual package build derivations:
nix eval --raw .#<project>.components.exes.<exe>.drvPath        # baseline vs (b)
# and/or diff the resolved package set in plan.json:
diff <(jq -S '."install-plan"|map({id,"pkg-name","pkg-version",flags})' plan_baseline.json) \
     <(jq -S '."install-plan"|map({id,"pkg-name","pkg-version",flags})' plan_freeze.json)
```
Downstream drvPaths depend on plan.json **content**, not on the projectNix store path:
`load-cabal-plan.nix:7` reads `readFile (projectNix + "/plan.json")`, and `:117` uses
`unsafeDiscardStringContext ... projectNix.outPath` (discards the path from the dep graph).
So a changed projectNix path does NOT perturb downstream drvPaths — only a changed
*resolved plan* would. Byte-identical exe drvPath + identical plan package set ⇒
invariance holds ⇒ the freeze is sound.

---

## 5. Risks

| Risk | Variant | Assessment |
|---|---|---|
| Changes the resolved plan / downstream drvPath | (b) | Only if the freeze pins differ from the free solve. At matching (index-state, ghc, cabal.project) the freeze *reproduces* the solve by construction → invariant. Enforced by the §4 check; fail-closed. |
| Stale freeze (index-state bumped / cabal.project edited) | (b) | Either the validating solve **errors** (over-constrained → visible failure, good) or it picks different versions → plan changes → **KILL** for that eval. Mitigation: key on (src, index-state, ghc, cabal.project[.local]); regenerate the freeze on any bump. Never silently serve a mismatched freeze. |
| Over-pruning breaks the solve | (a) | N/A — variant (a) is KILL; `truncate-index` only re-applies the already-applied index-state truncation, so no additional pruning happens. A *hypothetical* cone-pruner WOULD risk dropping a package the solver needs → unsatisfiable or altered plan. |
| plan-nix .drv path churn | (b) | Expected, benign — freeze is a new input but is removed before `$out` is packaged (`:723,:757`); downstream is content-keyed (`load-cabal-plan.nix:7,117`). |
| No cold-solve win from auto-cache | (b-auto) | Chicken-and-egg; the plan-nix store cache already serves repeats. Only the *committed* freeze helps a cold solve. |

---

## Files/lines read
- `src/libexpr-v3/lode/IFD_SOLVE_FINDINGS_2026-08-09.md:24,30,40,59`
- `haskell.nix/lib/call-cabal-project-to-nix.nix:15,608-635,679-683,692-707,721-723,746,757,772-780`
- `haskell.nix/nix-tools/nix-tools/truncate-index/Main.hs:22-30,46-53` (+ `--help`)
- `haskell.nix/overlays/haskell.nix:232-261` (dotCabal, `truncate-index` postFetch :258)
- `haskell.nix/modules/cabal-project.nix:4,55-68` (readIfExists + cabalProjectFreeze default :67)
- `haskell.nix/lib/load-cabal-plan.nix:7,117` (plan.json content read; projectNix context discarded)

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
