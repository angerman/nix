// ════════════════════════════════════════════════════════════════════════════
//  autoresearch-loop.workflow.js — the agent-driven outer loop for v3-autoresearch.
//
//  Run via the Workflow tool ONLY when the working tree is CLEAN (no uncommitted
//  engine edits) and you have explicitly opted into multi-agent orchestration:
//      Workflow({ scriptPath: "src/libexpr-v3/research/autoresearch-loop.workflow.js",
//                 args: { row: "foldl", metric: "cpu", width: 4 } })
//
//  It automates the tiered loop from lode/AUTORESEARCH_V3_DESIGN_2026-06-16.md:
//    Scout    — one agent reads research/program.md + do-not-repropose.tsv + the
//               target area and proposes `width` distinct, localized, fenced-off-
//               respecting lever ideas for the objective.
//    Explore  — each idea is implemented IN ITS OWN git worktree (isolation), then
//               graded by research/autoresearch-cycle.sh (rebuild → gate → verdict).
//               This is the autoresearch "train 5 min → keep/revert" analog, gate-
//               wrapped so correctness is a HARD constraint, not part of the reward.
//    Verify   — every KEEP-CANDIDATE is handed to an ADVERSARIAL verifier whose job
//               is to REFUTE the win (engaged? prod path? byte-identical on MORE
//               rows? benchmark special-casing? error-masking?).  This is the
//               anti-reward-hacking layer §3B — mandatory, because the engine is an
//               adversarially-gameable metric.
//    Synthesize — report confirmed keepers.  Does NOT auto-merge or default-flip:
//               a keep is PROVISIONAL until a periodic full nixpkgs flip-soak +
//               a human confirm it (the gen-major rollout discipline).
//
//  Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
//  Input Output Group.  SPDX-License-Identifier: Apache-2.0
// ════════════════════════════════════════════════════════════════════════════

export const meta = {
  name: 'v3-autoresearch-loop',
  description: 'Autonomous gate-wrapped keep/revert loop for the v3 VM (scout → explore-in-worktrees → adversarial-verify → synthesize)',
  whenToUse: 'After committing in-flight work, to autonomously explore low-risk v3 perf/mem levers (emit fusions, opt passes, GC knobs) under the byte-identity gate.',
  phases: [
    { title: 'Scout',      detail: 'propose N localized lever ideas from program.md' },
    { title: 'Explore',    detail: 'implement each in a worktree; grade via autoresearch-cycle.sh' },
    { title: 'Verify',     detail: 'adversarially refute each KEEP-CANDIDATE' },
    { title: 'Synthesize', detail: 'report confirmed (still provisional) keepers' },
  ],
}

const ROW    = (args && args.row)    || 'foldl'
const METRIC = (args && args.metric) || 'cpu'
const WIDTH  = (args && args.width)  || 4

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
          hypothesis: { type: 'string', description: 'what this changes and why it should improve the objective' },
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
    verdict: { type: 'string', enum: ['BUILD-FAIL','ABORT-NOT-ENGAGED','REVERT-DIVERGENT','REVERT-REGRESS','NEUTRAL','KEEP-CANDIDATE'] },
    ratio:   { type: 'string' },
    arena:   { type: 'string' },
    summary: { type: 'string', description: 'one line: what was changed + the verdict reason' },
    diff:    { type: 'string', description: 'the unified diff of the change (empty if reverted)' },
  },
  required: ['verdict', 'summary'],
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
log(`v3-autoresearch: objective row=${ROW} metric=${METRIC}, width=${WIDTH}`)
const scouted = await agent(
  `You are scouting localized optimization ideas for the v3 bytecode VM.
   READ first: src/libexpr-v3/research/program.md (objective, HARD constraints, keep/revert
   bars, FENCED-OFF areas) and src/libexpr-v3/research/do-not-repropose.tsv (already-killed
   levers — do NOT re-propose any of them).
   Objective: lower v3 ${METRIC} on the '${ROW}' row.
   Propose exactly ${WIDTH} DISTINCT, LOCALIZED candidate levers, each touching a small
   translation unit (prefer opt_*.cc / emit.cc / a GC tuning knob — avoid header edits that
   rebuild the world, and NEVER touch the fenced-off areas: derivationStrict, FFI, the
   writeback/cell machinery). Each must be byte-identity-preserving by construction.`,
  { schema: IDEAS_SCHEMA, phase: 'Scout' })

const ideas = (scouted && scouted.ideas) ? scouted.ideas.slice(0, WIDTH) : []
log(`scouted ${ideas.length} ideas: ${ideas.map(i => i.id).join(', ')}`)

const graded = await pipeline(ideas,
  // Stage 1 — implement in an isolated worktree, grade via the deterministic harness.
  (idea) => agent(
    `Implement ONE v3 lever in THIS git worktree and grade it.
     Idea: [${idea.id}] area=${idea.area} — ${idea.hypothesis}
     Rules (from research/program.md): byte-identity-preserving; no fenced-off edits; no
     benchmark special-casing; no new ungated env var. Make the minimal change, then run:
        bash src/libexpr-v3/research/autoresearch-cycle.sh --row ${ROW} --metric ${METRIC}
     (it rebuilds + measures v3 vs TW + emits a VERDICT line). Report the VERDICT fields and
     the unified diff. If the verdict is not KEEP-CANDIDATE, leave the diff in 'diff' for the
     record but note it will be reverted.`,
    { schema: VERDICT_SCHEMA, isolation: 'worktree', label: `explore:${idea.id}`, phase: 'Explore' }),
  // Stage 2 — adversarially verify only the KEEP-CANDIDATEs.
  (verdict, idea) => (verdict && verdict.verdict === 'KEEP-CANDIDATE')
    ? agent(
        `ADVERSARIALLY REFUTE this claimed v3 win — default to refuted=true unless every check passes.
         Idea [${idea.id}]: ${idea.hypothesis}
         Verdict: ${verdict.summary}
         Diff:\n${verdict.diff || '(none provided)'}
         Check, citing evidence: (1) was v3 actually ENGAGED (v3-direct stats line)? (2) is this the
         PROD path (not nix-instantiate / not a fallback)? (3) is it byte-identical to TW on MORE rows
         than just '${ROW}' (spot-check hello + git)? (4) does the diff SPECIAL-CASE the benchmark
         expression/shape/attr-name? (5) does it remove a correctness check or MASK an error to "win"
         (the fake-store class)? Any "no" to 1-3 or any "yes" to 4-5 ⇒ refuted=true.`,
        { schema: REFUTE_SCHEMA, label: `verify:${idea.id}`, phase: 'Verify' })
        .then(refute => ({ idea, verdict, refute }))
    : ({ idea, verdict, refute: null }))

phase('Synthesize')
const all = graded.filter(Boolean)
const confirmed = all.filter(r => r.verdict && r.verdict.verdict === 'KEEP-CANDIDATE' && r.refute && !r.refute.refuted)
const refuted   = all.filter(r => r.verdict && r.verdict.verdict === 'KEEP-CANDIDATE' && r.refute && r.refute.refuted)
log(`confirmed ${confirmed.length}/${all.length} (refuted-keeps: ${refuted.length}). All keeps are PROVISIONAL — full flip-soak + human confirm before any default-flip.`)
return {
  objective: { row: ROW, metric: METRIC },
  confirmed: confirmed.map(r => ({ id: r.idea.id, summary: r.verdict.summary, ratio: r.verdict.ratio, arena: r.verdict.arena })),
  refuted:   refuted.map(r => ({ id: r.idea.id, reason: r.refute.reason })),
  rejected:  all.filter(r => !r.verdict || r.verdict.verdict !== 'KEEP-CANDIDATE').map(r => ({ id: r.idea && r.idea.id, verdict: r.verdict && r.verdict.verdict, summary: r.verdict && r.verdict.summary })),
  note: 'Keeps are PROVISIONAL. Append every revert to research/do-not-repropose.tsv. Do NOT default-flip a GC/representation change without a full nixpkgs flip-soak + human sign-off.',
}
