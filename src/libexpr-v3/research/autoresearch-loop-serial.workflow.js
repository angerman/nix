// ════════════════════════════════════════════════════════════════════════════
//  autoresearch-loop-serial.workflow.js — the SERIAL, MAIN-TREE variant of the
//  v3-autoresearch loop, tuned for a host WITHOUT ccache and WITHOUT pre-built
//  worktree `build/` dirs (e.g. the 8-core laptop).
//
//  WHY serial-in-main-tree instead of the parallel-worktree
//  autoresearch-loop.workflow.js:
//    * `build/` is gitignored, so a fresh `git worktree` has NO meson build dir
//      → each arm would need `meson setup` + a FULL COLD build.
//    * No ccache is configured (no ccache token in build.ninja) → that cold
//      build is minutes, uncached.
//    * On an 8-core laptop, K parallel cold builds thrash → hours / timeouts.
//  In the main tree, `build/` already exists, so `ninja` for a single `.cc`
//  edit is an INCREMENTAL relink (~1-3 min) — fast and reliable. The price is
//  no parallelism and a mutated working tree, so every arm MUST restore the
//  tree to clean before the next arm runs (the script cannot run git between
//  agents, so the restore is the agent's contract + a defensive force-clean at
//  the start of each arm).
//
//  Use the parallel-worktree variant instead once the substrate is ready
//  (ccache enabled OR darwin-4 with warm per-worktree build dirs).
//
//  Run (working tree MUST be clean of tracked-source edits first):
//      Workflow({ scriptPath: "src/libexpr-v3/research/autoresearch-loop-serial.workflow.js",
//                 args: { row: "git", metric: "cpu", width: 3 } })
//
//  Same four-tier contract as the parallel variant (Scout → Explore → Verify →
//  Synthesize); keeps are PROVISIONAL (no auto-merge / no default-flip).
//
//  Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
//  Input Output Group.  SPDX-License-Identifier: Apache-2.0
// ════════════════════════════════════════════════════════════════════════════

export const meta = {
  name: 'v3-autoresearch-loop-serial',
  description: 'SERIAL main-tree keep/revert loop for the v3 VM (no-ccache hosts): scout → explore-in-main-tree-one-at-a-time → adversarial-verify → synthesize',
  whenToUse: 'On a host without ccache / warm worktree build dirs (e.g. the 8-core laptop), to autonomously explore low-risk v3 perf/mem levers under the byte-identity gate, one arm at a time with incremental builds.',
  phases: [
    { title: 'Scout',      detail: 'propose N localized lever ideas from program.md' },
    { title: 'Explore',    detail: 'implement each in the MAIN tree, one at a time; grade via autoresearch-cycle.sh; restore' },
    { title: 'Verify',     detail: 'adversarially refute each KEEP-CANDIDATE' },
    { title: 'Synthesize', detail: 'report confirmed (still provisional) keepers' },
  ],
}

const ROW    = (args && args.row)    || 'git'
const METRIC = (args && args.metric) || 'cpu'
const WIDTH  = (args && args.width)  || 3

const IDEAS_SCHEMA = {
  type: 'object',
  properties: {
    ideas: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          id:         { type: 'string' },
          area:       { type: 'string', description: 'file/pass, e.g. opt_stream_fusion.cc or a GC knob' },
          hypothesis: { type: 'string', description: 'what this changes and why it should improve the objective on the REAL row' },
        },
        required: ['id', 'area', 'hypothesis'],
      },
    },
  },
  required: ['ideas'],
}

const VERDICT_SCHEMA = {
  type: 'object',
  properties: {
    verdict:      { type: 'string', enum: ['BUILD-FAIL','ABORT-NOT-ENGAGED','REVERT-DIVERGENT','REVERT-REGRESS','NEUTRAL','KEEP-CANDIDATE'] },
    ratio:        { type: 'string' },
    arena:        { type: 'string' },
    summary:      { type: 'string', description: 'one line: what was changed + the verdict reason' },
    diff:         { type: 'string', description: 'the unified diff of the change (for the record; the tree is then restored)' },
    extraByteId:  { type: 'string', description: 'for a KEEP-CANDIDATE: byte-identity result on hello + git checked BEFORE reverting (e.g. "hello ok, git ok"); empty otherwise' },
    treeRestored: { type: 'boolean', description: 'true iff `git status --porcelain -- src/` is clean after the arm (edit reverted)' },
  },
  required: ['verdict', 'summary', 'treeRestored'],
}

const REFUTE_SCHEMA = {
  type: 'object',
  properties: {
    refuted: { type: 'boolean', description: 'true if the win does NOT hold up under scrutiny' },
    reason:  { type: 'string' },
    checks:  { type: 'string', description: 'engaged / prod-path / byte-identity-breadth / special-casing / error-masking findings' },
  },
  required: ['refuted', 'reason'],
}

phase('Scout')
log(`v3-autoresearch (SERIAL/main-tree): objective row=${ROW} metric=${METRIC}, width=${WIDTH}`)
const scouted = await agent(
  `You are scouting localized optimization ideas for the v3 bytecode VM.
   READ first: src/libexpr-v3/research/program.md (objective, the REAL-WORLD-GAINS RULE,
   HARD constraints, keep/revert bars, FENCED-OFF areas) and
   src/libexpr-v3/research/do-not-repropose.tsv (already-killed levers — do NOT re-propose any).
   Objective: lower v3 ${METRIC} on the REAL '${ROW}' row (a nixpkgs drvPath eval).
   The real hot path (per the git.drvPath sample) is forceValue → callClosure → dispatchLoop,
   driven by primFoldl/primFilter running nixpkgs lib.foldl'/filter/map — i.e. PER-ELEMENT
   DISPATCH, not record-construction. Favor ideas that cut dispatch/alloc on that path.
   Propose exactly ${WIDTH} DISTINCT, LOCALIZED candidate levers, each touching a SMALL
   translation unit (prefer opt_*.cc / emit.cc / a GC or IC tuning knob — avoid header edits
   that rebuild the world, and NEVER touch the fenced-off areas: derivationStrict, FFI, the
   writeback/cell machinery). Each must be byte-identity-preserving BY CONSTRUCTION, and must
   not be benchmark-specific (it has to help REAL evals, not just one row's shape).`,
  { schema: IDEAS_SCHEMA, phase: 'Scout' })

const ideas = (scouted && scouted.ideas) ? scouted.ideas.slice(0, WIDTH) : []
log(`scouted ${ideas.length} ideas: ${ideas.map(i => i.id).join(', ')}`)

// ── Explore SERIALLY in the main tree (NO worktree). One arm at a time so the
//    arms never stomp each other's edits or builds. Each arm restores the tree. ──
const graded = []
for (let i = 0; i < ideas.length; i++) {
  const idea = ideas[i]
  const verdict = await agent(
    `Implement ONE v3 lever in the MAIN working tree (this is the SERIAL variant — there is
     NO worktree; you are editing the live tree, so discipline matters).

     STEP 0 (defensive): run \`git -C /Users/angerman/Projects/iohk/nix status --porcelain -- src/\`.
     If any TRACKED source under src/ is already modified, restore it first
     (\`git -C /Users/angerman/Projects/iohk/nix restore --staged --worktree -- src/\`) so you
     start from a clean committed baseline. (Untracked build artifacts are fine; leave them.)

     Idea: [${idea.id}] area=${idea.area} — ${idea.hypothesis}

     Rules (from research/program.md): byte-identity-preserving BY CONSTRUCTION; no fenced-off
     edits (derivationStrict / FFI / writeback-cell machinery); no benchmark special-casing;
     no new ungated env var; prefer a single small .cc edit (NOT a header that rebuilds the world).

     Make the minimal change, then GRADE it:
        bash src/libexpr-v3/research/autoresearch-cycle.sh --row ${ROW} --metric ${METRIC}
     It rebuilds (incremental ninja), measures v3 vs TW on the '${ROW}' row, and emits a single
     VERDICT line. Report the VERDICT fields (verdict / ratio / arena) and the unified diff.

     IF the verdict is KEEP-CANDIDATE: BEFORE reverting, also check byte-identity on a SECOND
     real row so a single-row win isn't masking a divergence — eval hello.drvPath under both
     arms and compare:
        NIX_V3_DIRECT_EVAL=1 NIX_VM_STATS=1 <nix> eval --impure --expr '(import <nixpkgs> {}).hello.drvPath'
        <nix>                                <nix> eval --impure --expr '(import <nixpkgs> {}).hello.drvPath'
     (the <nix> binary is build/src/nix/nix). Put "hello ok/DIVERGENT, git ok" in extraByteId.

     STEP FINAL (MANDATORY, regardless of verdict): RESTORE the tree —
        git -C /Users/angerman/Projects/iohk/nix restore --staged --worktree -- src/
     then confirm \`git status --porcelain -- src/\` shows NO modified tracked sources, and set
     treeRestored=true. The committed engine MUST be left byte-for-byte unchanged; keeps are
     PROVISIONAL and recorded in the diff field only — do NOT git-commit anything.`,
    { schema: VERDICT_SCHEMA, label: `explore:${idea.id}`, phase: 'Explore' })
  graded.push({ idea, verdict })
  log(`[${idea.id}] ${verdict ? verdict.verdict : 'NULL'}${verdict && !verdict.treeRestored ? '  ⚠ TREE NOT RESTORED' : ''}`)
}

// ── Verify: adversarially refute each KEEP-CANDIDATE (static — the diff was reverted). ──
phase('Verify')
const verified = await parallel(graded.map(g => () => {
  const v = g.verdict
  if (!v || v.verdict !== 'KEEP-CANDIDATE')
    return Promise.resolve({ ...g, refute: null })
  return agent(
    `ADVERSARIALLY REFUTE this claimed v3 win — default to refuted=true unless every check passes.
     Idea [${g.idea.id}]: ${g.idea.hypothesis}
     Verdict: ${v.summary}   (ratio=${v.ratio || '?'}, arena=${v.arena || '?'})
     Second-row byte-id check from explore: ${v.extraByteId || '(none reported)'}
     Diff (now reverted; analyze statically):\n${v.diff || '(none provided)'}
     Check, citing evidence from the diff/summary: (1) was v3 actually ENGAGED (the harness
     aborts if not — but confirm the summary shows a real v3-direct measurement, not TW)?
     (2) is this the PROD path (not nix-instantiate / not a fallback)? (3) is it byte-identical
     to TW on MORE than just '${ROW}' — does extraByteId show hello ok too, or is it untested?
     (4) does the diff SPECIAL-CASE the benchmark expression / shape / attr-name, or only help
     this row (a real-world-neutral micro-opt, the foldl-lever trap)? (5) does it remove a
     correctness check or MASK an error to "win" (the fake-store class)?
     Any "no" to 1-3 or any "yes" to 4-5 ⇒ refuted=true.`,
    { schema: REFUTE_SCHEMA, label: `verify:${g.idea.id}`, phase: 'Verify' })
    .then(refute => ({ ...g, refute }))
}))

phase('Synthesize')
const all = verified.filter(Boolean)
const confirmed = all.filter(r => r.verdict && r.verdict.verdict === 'KEEP-CANDIDATE' && r.refute && !r.refute.refuted)
const refuted   = all.filter(r => r.verdict && r.verdict.verdict === 'KEEP-CANDIDATE' && r.refute && r.refute.refuted)
const dirty     = all.filter(r => r.verdict && r.verdict.treeRestored === false)
log(`confirmed ${confirmed.length}/${all.length} (refuted-keeps: ${refuted.length}; arms that left the tree dirty: ${dirty.length}). All keeps are PROVISIONAL — full flip-soak + darwin-4 + human confirm before any default-flip.`)
return {
  objective: { row: ROW, metric: METRIC, variant: 'serial-main-tree' },
  confirmed: confirmed.map(r => ({ id: r.idea.id, summary: r.verdict.summary, ratio: r.verdict.ratio, arena: r.verdict.arena, diff: r.verdict.diff, extraByteId: r.verdict.extraByteId })),
  refuted:   refuted.map(r => ({ id: r.idea.id, reason: r.refute.reason })),
  rejected:  all.filter(r => !r.verdict || r.verdict.verdict !== 'KEEP-CANDIDATE').map(r => ({ id: r.idea && r.idea.id, verdict: r.verdict && r.verdict.verdict, summary: r.verdict && r.verdict.summary })),
  treeDirtyArms: dirty.map(r => r.idea && r.idea.id),
  note: 'SERIAL main-tree run. Keeps are PROVISIONAL (diff included for manual review). Append every revert to research/do-not-repropose.tsv. Re-measure any keeper on darwin-4 + a full nixpkgs flip-soak before committing/default-flipping. If treeDirtyArms is non-empty, manually `git restore -- src/` before trusting later results.',
}
