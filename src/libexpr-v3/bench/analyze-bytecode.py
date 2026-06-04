#!/usr/bin/env python3
"""analyze-bytecode.py — static opcode + n-gram statistics for v3 bytecode.

Consumes the disassembly dump emitted by the VM under
`NIX_V3_EMIT_BYTECODE=1` (see emit.cc::compile), e.g.:

    NIX_V3_EMIT_BYTECODE=1 NIX_V3_EMIT_BYTECODE_OUT=/tmp/bc.txt \\
    NIX_V3_NO_DISK_CACHE=1 NIX_V3_DIRECT_EVAL=1 \\
      ./build/src/nix/nix eval --impure --expr '(import <nixpkgs> {}).hello.drvPath'
    ./analyze-bytecode.py /tmp/bc.txt

Reports, over the STATIC compiled bytecode (what the emitter produced for
every CU — top-level + each imported module):
  - opcode frequency histogram
  - bigram / trigram / 4-gram frequency (within function runs)
  - superinstruction candidates ranked by DISPATCH SAVINGS = (L-1)·count
    (fusing an L-gram into one superinstruction removes L-1 dispatches
    per static occurrence)

CAVEATS (printed in the report too):
  * STATIC, not execution-weighted.  A sequence inside a hot loop matters
    far more than its static count suggests.  The execution-weighted truth
    for bigrams already ships as `NIX_VM_BIGRAMS` (dynamic counter, #782);
    static is the cheaper "what does the compiler emit" proxy.
  * n-grams are taken over the LINEAR instruction stream, broken at
    function boundaries (OP_RETURN / OP_HALT) and CU boundaries.  Branches
    (OP_JUMP / OP_BRANCH_* / OP_*_BRANCH) mean the linear successor is not
    always the dynamic successor — a static-adjacency approximation, which
    is exactly what an emit-time superinstruction fuser keys on.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
"""

import argparse
import re
import sys
from collections import Counter

# Instruction line from disasm.cc: "  [<ip>] OP_NAME operand=..."
INSN_RE = re.compile(r"^\s*\[(\d+)\]\s+(OP_[A-Z0-9_]+)")
UNKNOWN_RE = re.compile(r"^\s*\[(\d+)\]\s+<unknown")
CU_RE = re.compile(r"^=== v3-bytecode CU functions=(\d+) code=(\d+) ===")

# Opcodes that end a linear run for n-gram purposes (function boundaries).
RUN_BREAK = {"OP_RETURN", "OP_HALT"}

# Opcodes already implemented as fused superinstructions — flagged so we
# don't "discover" them as new candidates.
ALREADY_FUSED = {"OP_GET_LOCAL_FORCE", "OP_GET_UPVALUE_FORCE", "OP_TAIL_CALL"}

# The local-stack-motion family (#778: 48.9% of *dynamic* dispatch).
STACK_MOTION = {"OP_GET_LOCAL", "OP_SET_LOCAL", "OP_GET_UPVALUE", "OP_FORCE",
                "OP_GET_LOCAL_FORCE", "OP_GET_UPVALUE_FORCE"}


def parse(path):
    """Return (list-of-runs, n_cus, n_unknown).  Each run is a list of
    opcode-name strings (no operands), broken at function/CU boundaries."""
    runs = []
    cur = []
    n_cus = 0
    n_unknown = 0

    def flush():
        nonlocal cur
        if cur:
            runs.append(cur)
            cur = []

    with open(path, errors="replace") as f:
        for line in f:
            if CU_RE.match(line):
                flush()
                n_cus += 1
                continue
            if UNKNOWN_RE.match(line):
                n_unknown += 1
                flush()  # alignment lost — don't span it
                continue
            m = INSN_RE.match(line)
            if not m:
                continue
            op = m.group(2)
            cur.append(op)
            if op in RUN_BREAK:
                flush()
    flush()
    return runs, n_cus, n_unknown


def ngrams(runs, n):
    c = Counter()
    for run in runs:
        for i in range(len(run) - n + 1):
            c[tuple(run[i:i + n])] += 1
    return c


def pct(x, total):
    return (100.0 * x / total) if total else 0.0


def bar(frac, width=24):
    filled = int(round(frac * width))
    return "█" * filled + "·" * (width - filled)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="path to NIX_V3_EMIT_BYTECODE dump file")
    ap.add_argument("--top", type=int, default=20, help="rows per table")
    ap.add_argument("--max-n", type=int, default=4, choices=[2, 3, 4],
                    help="largest n-gram length (default 4)")
    args = ap.parse_args()

    runs, n_cus, n_unknown = parse(args.dump)
    total_insns = sum(len(r) for r in runs)
    if total_insns == 0:
        print(f"analyze-bytecode: no instructions parsed from {args.dump} "
              f"(is NIX_V3_EMIT_BYTECODE output present?)", file=sys.stderr)
        sys.exit(1)

    op_hist = Counter()
    for r in runs:
        op_hist.update(r)

    print("=" * 70)
    print(f"v3 STATIC bytecode analysis — {args.dump}")
    print(f"  CUs: {n_cus}   functions/runs: {len(runs)}   "
          f"instructions: {total_insns}"
          + (f"   UNKNOWN-opcode lines: {n_unknown} (alignment risk!)"
             if n_unknown else ""))
    print("=" * 70)

    # ---- opcode histogram ---------------------------------------------
    print(f"\n## Opcode frequency (top {args.top} of {len(op_hist)})\n")
    sm_total = sum(op_hist[o] for o in STACK_MOTION)
    print(f"{'opcode':<26}{'count':>10}{'%':>8}  distribution")
    for op, c in op_hist.most_common(args.top):
        f = pct(c, total_insns)
        tag = "  ← stack-motion" if op in STACK_MOTION else ""
        print(f"{op:<26}{c:>10}{f:>7.2f}%  {bar(c/total_insns)}{tag}")
    print(f"\n  local-stack-motion family total: {sm_total} "
          f"({pct(sm_total, total_insns):.1f}% of static instructions)")

    # ---- n-gram tables -------------------------------------------------
    grams = {}
    for n in range(2, args.max_n + 1):
        grams[n] = ngrams(runs, n)
        total = sum(grams[n].values())
        topsum = sum(c for _, c in grams[n].most_common(args.top))
        label = {2: "bigrams", 3: "TRIGRAMS", 4: "4-grams"}[n]
        print(f"\n## {label} (top {args.top}; {len(grams[n])} distinct, "
              f"{total} total; top-{args.top} = {pct(topsum, total):.1f}%)\n")
        print(f"{'sequence':<52}{'count':>9}{'%':>7}")
        for seq, c in grams[n].most_common(args.top):
            short = " ".join(s[3:] for s in seq)  # strip "OP_"
            print(f"{short:<52}{c:>9}{pct(c, total):>6.1f}%")

    # ---- superinstruction candidate ranking ---------------------------
    # Savings = (len-1) * count: fusing an L-gram removes L-1 dispatches
    # per static occurrence.  Rank 2..max-n grams together.  Skip grams
    # that span a run break (already excluded) or are already fused.
    print(f"\n## Superinstruction candidates — ranked by dispatch savings"
          f" = (len-1)·count\n")
    cand = []
    for n in range(2, args.max_n + 1):
        for seq, c in grams[n].items():
            if any(s in ALREADY_FUSED for s in seq):
                continue
            cand.append(((n - 1) * c, c, n, seq))
    cand.sort(reverse=True)
    total_dispatch = total_insns  # static dispatch count ≈ instruction count
    print(f"{'fused sequence':<46}{'occ':>7}{'saved':>9}{'%disp':>7}")
    best = cand[0][0] if cand else 0
    for saved, c, n, seq in cand[:args.top]:
        short = "+".join(s[3:] for s in seq)
        flag = ""
        if all(s in STACK_MOTION for s in seq):
            flag = "  ← all stack-motion"
        print(f"{short:<46}{c:>7}{saved:>9}{pct(saved, total_dispatch):>6.1f}%{flag}")
    # NOTE: these candidates OVERLAP (one GET_LOCAL is counted in its
    # bigram, trigram and 4-gram), so per-row savings are NOT additive —
    # summing them double-counts.  Each %disp is the individual ceiling
    # for fusing that one sequence; the single best is the headline.
    print(f"\n  per-row savings are individual ceilings and OVERLAP (not")
    print(f"  additive); best single candidate removes {best} dispatches "
          f"({pct(best, total_dispatch):.1f}% of {total_dispatch}).")

    # ---- guidance ------------------------------------------------------
    print("\n## Reading this for register-VM / superinstruction decisions")
    print("  * STATIC counts; the execution-weighted signal (NIX_VM_BIGRAMS,")
    print("    dynamic) is decision-grade — a hot-loop trigram dwarfs its")
    print("    static count.  Use both; static = 'what the emitter emits'.")
    print("  * High top-K n-gram concentration ⇒ a few superinstructions")
    print("    capture most fusable dispatch.  Low/flat ⇒ a register VM")
    print("    (eliminating the GET/SET_LOCAL traffic structurally) beats")
    print("    piecemeal fusion.")
    print("  * 'all stack-motion' candidates are the register-VM target;")
    print("    OP_GET_LOCAL_FORCE / OP_GET_UPVALUE_FORCE are already fused")
    print("    (excluded above) — see what remains around them.")


if __name__ == "__main__":
    main()
