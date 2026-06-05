#!/usr/bin/env python3
"""analyze-operands.py — OPERAND-SENSITIVE static scan of v3 bytecode.

Companion to analyze-bytecode.py.  That tool is deliberately opcode-only
(it discards operands), so its n-grams cannot distinguish e.g. "load the
SAME local twice" (a redundant-load / DUP candidate) from "load two
DIFFERENT locals" (a normal two-arg push).  This tool parses the
`; resolved` operand annotations the high-quality disassembler now emits
(`v3-eval --emit-bytecode`, disasm.cc::disassembleModule) and reports
optimisation candidates that ONLY become visible with operands.

Input: a NIX_V3_EMIT_BYTECODE dump (same format analyze-bytecode.py reads).

Detectors (each a HYPOTHESIS to triage against NIX_VM_OPCOUNTS/BIGRAMS —
static counts are a generator, not decision-grade):

  D1  same-slot adjacent  GET_LOCAL n ; GET_LOCAL n
        → the local is pushed twice in a row; the 2nd load is a DUP
          (cheaper than re-reading the frame).  Opcode n-grams lump these
          in with the different-slot case; D1 splits them so the DUP
          ceiling is the SAME-slot count, not the raw bigram count.

  D2  same-slot  SET_LOCAL n ; GET_LOCAL n  NOT already SET_LOCAL_KEEP
        → SET_LOCAL_KEEP (#shipped) fuses the adjacent same-slot store+load.
          Any residual same-slot SET;GET that the emitter left unfused is a
          MISS by that pass (worth auditing the lowering site).

  D3  primop reached via generic OP_CALL  (LIT_PRIMOP <p> … OP_CALL)
        → a builtin loaded as a value and called through the generic
          closure-call path instead of OP_CALL_PRIMOP.  Counts per primop.

Boundaries: runs break at OP_RETURN / OP_HALT, CU delimiters, AND label
leaders (`L<off>:`) — a label is a jump target, so the linear predecessor
is not the guaranteed dynamic one; adjacency detectors must not span it.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
"""

import argparse
import re
import sys
from collections import Counter

CU_RE    = re.compile(r"^=== v3-bytecode CU functions=(\d+) code=(\d+) ===")
LABEL_RE = re.compile(r"^L(\d+):\s*$")
# Instruction with operand + optional resolved suffix, e.g.
#   "  [54] OP_LIT_PRIMOP            operand=2   ; primop foldl'"
INSN_RE  = re.compile(
    r"^\s*\[(\d+)\]\s+(OP_[A-Z0-9_]+)\s+operand=(-?\d+)(?:\s+data=\[[^\]]*\])?"
    r"(?:\s*;\s*(.*))?$")

RUN_BREAK = {"OP_RETURN", "OP_HALT"}


class Insn:
    __slots__ = ("ip", "op", "operand", "resolved")
    def __init__(self, ip, op, operand, resolved):
        self.ip = ip; self.op = op; self.operand = operand
        self.resolved = resolved or ""


def parse_runs(path):
    """Yield runs (lists of Insn) broken at RETURN/HALT/CU/label."""
    runs, cur = [], []
    def flush():
        nonlocal cur
        if cur:
            runs.append(cur); cur = []
    with open(path, errors="replace") as f:
        for line in f:
            if CU_RE.match(line) or LABEL_RE.match(line):
                flush(); continue
            m = INSN_RE.match(line)
            if not m:
                continue
            ip, op, operand, resolved = m.groups()
            cur.append(Insn(int(ip), op, int(operand), resolved))
            if op in RUN_BREAK:
                flush()
    flush()
    return runs


def d1_same_slot_get_get(runs):
    """GET_LOCAL n ; GET_LOCAL n  — same vs different slot."""
    same = diff = 0
    for run in runs:
        for a, b in zip(run, run[1:]):
            if a.op == "OP_GET_LOCAL" and b.op == "OP_GET_LOCAL":
                if a.operand == b.operand:
                    same += 1
                else:
                    diff += 1
    return same, diff


def d2_set_then_get(runs):
    """SET_LOCAL n ; GET_LOCAL n  — same-slot residual not caught by KEEP."""
    same = diff = 0
    for run in runs:
        for a, b in zip(run, run[1:]):
            if a.op == "OP_SET_LOCAL" and b.op == "OP_GET_LOCAL":
                if a.operand == b.operand:
                    same += 1
                else:
                    diff += 1
    return same, diff


def d3_primop_via_generic_call(runs):
    """Attribute each generic OP_CALL to the nearest preceding LIT_PRIMOP in
    the same run (reset on OP_CALL/OP_CALL_PRIMOP, since that consumes the
    callee).  This is a heuristic proxy for "this generic call dispatches
    this builtin"; it is NOT precise data-flow (args intervene), but it is
    far tighter than run-level co-occurrence."""
    by_primop = Counter()
    generic_calls = attributed = 0
    for run in runs:
        last_primop = None
        for i in run:
            if i.op == "OP_LIT_PRIMOP":
                last_primop = i.resolved.replace("primop ", "")
            elif i.op == "OP_CALL":
                generic_calls += 1
                if last_primop is not None:
                    by_primop[last_primop] += 1
                    attributed += 1
                last_primop = None      # callee consumed
            elif i.op == "OP_CALL_PRIMOP":
                last_primop = None
    return by_primop, generic_calls, attributed


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="NIX_V3_EMIT_BYTECODE dump file")
    ap.add_argument("--top", type=int, default=15)
    args = ap.parse_args()

    runs = parse_runs(args.dump)
    total = sum(len(r) for r in runs)
    if total == 0:
        print("analyze-operands: no operand-bearing instructions parsed "
              f"from {args.dump}", file=sys.stderr)
        sys.exit(1)

    print("=" * 70)
    print(f"v3 OPERAND-SENSITIVE scan — {args.dump}")
    print(f"  runs: {len(runs)}   operand-bearing instructions: {total}")
    print("=" * 70)

    s, d = d1_same_slot_get_get(runs)
    tot = s + d
    print("\n## D1  GET_LOCAL n ; GET_LOCAL n   (same vs different slot)")
    print(f"  same-slot (DUP candidate): {s}")
    print(f"  different-slot (normal)  : {d}")
    if tot:
        print(f"  → {100.0*s/tot:.1f}% of adjacent GET;GET pairs are the SAME"
              f" slot; opcode-only n-grams count all {tot} together.")

    s, d = d2_set_then_get(runs)
    tot = s + d
    print("\n## D2  SET_LOCAL n ; GET_LOCAL n   (SET_LOCAL_KEEP residual)")
    print(f"  same-slot (KEEP MISS): {s}")
    print(f"  different-slot       : {d}")
    if s == 0:
        print("  → 0 same-slot residual: SET_LOCAL_KEEP caught the adjacent"
              " cases (expected).")
    else:
        print(f"  → {s} adjacent same-slot SET;GET the KEEP pass left unfused"
              " — audit the emit site.")

    by_primop, gcalls, attr = d3_primop_via_generic_call(runs)
    print("\n## D3  generic OP_CALL attributed to nearest preceding LIT_PRIMOP")
    print(f"  total generic OP_CALL: {gcalls}   "
          f"attributed to a primop: {attr}")
    print(f"  {'primop (likely callee)':<24}{'generic-CALL sites':>20}")
    for nm, c in by_primop.most_common(args.top):
        print(f"  {nm:<24}{c:>20}")

    print("\n## Caveat")
    print("  STATIC. Confirm any candidate execution-weighted (NIX_VM_OPCOUNTS")
    print("  / NIX_VM_BIGRAMS) before implementing — a hot-loop pattern can")
    print("  dwarf its static count, and a frequent static pattern can be")
    print("  dynamically cold (the SET_LOCAL_KEEP lesson).")


if __name__ == "__main__":
    main()
