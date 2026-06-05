#!/usr/bin/env python3
"""analyze-operands.py — OPERAND- and STRUCTURE-sensitive static scan of
v3 bytecode.

Companion to analyze-bytecode.py, which is deliberately opcode-only (it
discards operands AND function structure, so its n-grams cannot tell
"load the SAME local twice" from "load two DIFFERENT locals", nor reason
about per-function shape or control flow).  This tool parses the
per-function framing + `; resolved` operand annotations + `L<off>:` jump
labels the high-quality disassembler now emits (`v3-eval --emit-bytecode`,
disasm.cc::disassembleModule) and runs three families of detector:

  OPERAND adjacency (D-series)
    D1  same-slot adjacent  GET_LOCAL n ; GET_LOCAL n   (DUP candidate)
    D2  same-slot  SET_LOCAL n ; GET_LOCAL n            (KEEP residual)
    D3  generic OP_CALL attributed to nearest LIT_PRIMOP (callee guess)

  PER-FUNCTION structural (S-series)
    S1  single-reference functions     (one MAKE_CLOSURE/THUNK site → inline)
    S2  slot slack                     (declared nLocals ≫ max slot touched)
    S3  leaf functions                 (no call-family op; +MAKE_THUNK sub)

  BRANCH structure (B-series)
    B1  branch-to-fallthrough          (target == next ip → dead/degenerate)
    B2  branch-to-jump                 (target is itself a JUMP → threadable)
    B0  (integrity) branch target that matches no instruction ip

Every result is a HYPOTHESIS to triage against the execution-weighted
counters (NIX_VM_OPCOUNTS / NIX_VM_BIGRAMS) — static counts generate
candidates, they don't decide (the SET_LOCAL_KEEP measure-twice lesson).

Input: a NIX_V3_EMIT_BYTECODE / `--emit-bytecode` dump.

NOTE on scope: `v3-eval` runs installAllBytecodePrimops at startup, so a
whole-process dump is DOMINATED by the fixed bytecode-primop wrappers
(derivation/foldl'/…) — those CUs are identical across user expressions
(empirically ~96% of a small-expr dump).  Use `--cu N` / `--cu last` to
isolate one CU (the user expression is typically the last `; module`).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0
"""

import argparse
import re
import sys
from collections import Counter

# --- line shapes emitted by disasm.cc --------------------------------------
MODULE_RE = re.compile(r"^;\s*module\s+functions=(\d+)")
FUNC_RE = re.compile(
    r'^;\s*func\s+(\d+)(?:\s+"([^"]*)")?\s+arity=(\d+)\s+nUp=(\d+)'
    r"\s+nLocals=(\d+)\s+entry=(\d+)")
LABEL_RE = re.compile(r"^L(\d+):\s*$")
INSN_RE = re.compile(
    r"^\s*\[(\d+)\]\s+(OP_[A-Z0-9_]+)\s+operand=(-?\d+)"
    r"(?:\s+data=\[[^\]]*\])?(?:\s*;\s*(.*))?$")

# --- opcode families (names exactly as opName() emits them) ----------------
BRANCH_OPS = {"OP_JUMP", "OP_BRANCH_FALSE", "OP_BRANCH_TRUE",
              "OP_AND_BRANCH", "OP_OR_BRANCH", "OP_IMPL_BRANCH"}
CALL_OPS = {"OP_CALL", "OP_CALL_N", "OP_CALL_PRIMOP", "OP_TAIL_CALL",
            "OP_TAIL_CALL_N", "OP_APPLY_OVERRIDES"}
SLOT_OPS = {"OP_GET_LOCAL", "OP_SET_LOCAL", "OP_SET_LOCAL_KEEP",
            "OP_GET_LOCAL_FORCE"}
CLOSURE_REF_OPS = {"OP_MAKE_CLOSURE", "OP_MAKE_THUNK"}  # operand = lambda id
RUN_BREAK = {"OP_RETURN", "OP_HALT"}


class Insn:
    __slots__ = ("ip", "op", "operand", "target", "resolved")

    def __init__(self, ip, op, operand, resolved):
        self.ip = ip
        self.op = op
        self.operand = operand
        # For branch ops the operand IS the absolute target offset
        # (disasm.cc: isBranchOp ⇒ isTarget[operand]).
        self.target = operand if op in BRANCH_OPS else None
        self.resolved = resolved or ""


class Func:
    __slots__ = ("cu", "fid", "name", "arity", "nUp", "nLocals",
                 "entry", "insns", "labels")

    def __init__(self, cu, fid, name, arity, nUp, nLocals, entry):
        self.cu = cu
        self.fid = fid
        self.name = name or ""
        self.arity = arity
        self.nUp = nUp
        self.nLocals = nLocals
        self.entry = entry
        self.insns = []          # list[Insn] in ip order
        self.labels = set()      # set[int] of ips that are jump targets

    def blocks(self):
        """Split insns into basic blocks at label leaders and RUN_BREAK so
        adjacency detectors don't span a jump target or a function exit."""
        block = []
        for ins in self.insns:
            if ins.ip in self.labels and block:
                yield block
                block = []
            block.append(ins)
            if ins.op in RUN_BREAK:
                yield block
                block = []
        if block:
            yield block


def parse_module(path):
    """Return (funcs, refs) where funcs is list[Func] and refs is a
    Counter keyed (cu_index, lambda_id) of MAKE_CLOSURE/MAKE_THUNK sites."""
    funcs = []
    refs = Counter()
    cu = -1
    cur = None
    pending_label = None  # an L<ip>: seen; attach to the next insn's func
    with open(path, errors="replace") as f:
        for line in f:
            if MODULE_RE.match(line):
                cu += 1
                cur = None
                continue
            fm = FUNC_RE.match(line)
            if fm:
                fid, name, arity, nUp, nLocals, entry = fm.groups()
                cur = Func(cu, int(fid), name, int(arity), int(nUp),
                           int(nLocals), int(entry))
                funcs.append(cur)
                continue
            lm = LABEL_RE.match(line)
            if lm:
                pending_label = int(lm.group(1))
                continue
            im = INSN_RE.match(line)
            if not im or cur is None:
                continue
            ip, op, operand, resolved = im.groups()
            ins = Insn(int(ip), op, int(operand), resolved)
            if pending_label is not None:
                cur.labels.add(ins.ip)
                pending_label = None
            cur.insns.append(ins)
            if op in CLOSURE_REF_OPS:
                refs[(cur.cu, ins.operand)] += 1
    return funcs, refs


# ============================ D-series ====================================
def d_adjacent(funcs, op_a, op_b):
    """Count same- vs different-operand adjacent (op_a, op_b) pairs,
    within basic blocks only."""
    same = diff = 0
    for fn in funcs:
        for block in fn.blocks():
            for a, b in zip(block, block[1:]):
                if a.op == op_a and b.op == op_b:
                    if a.operand == b.operand:
                        same += 1
                    else:
                        diff += 1
    return same, diff


def d3_primop_via_generic_call(funcs):
    by_primop = Counter()
    gcalls = attributed = 0
    for fn in funcs:
        for block in fn.blocks():
            last_primop = None
            for i in block:
                if i.op == "OP_LIT_PRIMOP":
                    last_primop = i.resolved.replace("primop ", "")
                elif i.op == "OP_CALL":
                    gcalls += 1
                    if last_primop is not None:
                        by_primop[last_primop] += 1
                        attributed += 1
                    last_primop = None
                elif i.op == "OP_CALL_PRIMOP":
                    last_primop = None
    return by_primop, gcalls, attributed


# ============================ C-series ====================================
# Constant spill-and-reload (NEXT_STEPS_2026-06-05 §2(a)).  A-normal-form
# lowering names every literal operand, so the emitter materializes a pure
# constant into a slot — `LIT_* k ; SET_LOCAL s ; … ; GET_LOCAL s` — instead
# of pushing it where used.  For a write-once slot s holding a constant,
# rematerializing the LIT at each use site is order-independent and saves
# exactly 2 ops per constant (the early LIT + the SET; each GET→LIT is the
# same width).  This detector counts those candidates and the ops saveable.
LIT_OPS = {"OP_LIT_INT", "OP_LIT_FLOAT", "OP_LIT_BOOL", "OP_LIT_NULL",
           "OP_LIT_TRUE", "OP_LIT_FALSE"}


def c1_const_spill(funcs):
    """Per function, find `LIT_* ; SET_LOCAL s` where slot s is written
    exactly once (so the SET defines a single constant binding) and read at
    least once.  Returns (n_candidates, ops_saveable, by_litop, rows)."""
    n = 0
    saveable = 0
    by_litop = Counter()
    rows = []  # (fn, count_in_fn)
    for fn in funcs:
        # write/read tallies per slot within this function
        writes = Counter()
        reads = Counter()
        for ins in fn.insns:
            if ins.op == "OP_SET_LOCAL" or ins.op == "OP_SET_LOCAL_KEEP":
                writes[ins.operand] += 1
            elif ins.op == "OP_GET_LOCAL" or ins.op == "OP_GET_LOCAL_FORCE":
                reads[ins.operand] += 1
        fn_count = 0
        for a, b in zip(fn.insns, fn.insns[1:]):
            if a.op in LIT_OPS and b.op == "OP_SET_LOCAL":
                s = b.operand
                if writes[s] == 1 and reads[s] >= 1:
                    n += 1
                    saveable += 2          # early LIT + SET removed
                    by_litop[a.op] += 1
                    fn_count += 1
        if fn_count:
            rows.append((fn_count, fn))
    rows.sort(key=lambda r: r[0], reverse=True)
    return n, saveable, by_litop, rows


# Lever 1A residual (WALL_OPTIMIZATION_PLAN §4 Phase 1A): generalises C1 from
# literal sources to ANY single-use spill.  A slot written EXACTLY once
# (plain SET_LOCAL, not KEEP) and read EXACTLY once is a single-use
# intermediate the emitter spilled-and-reloaded — the #542 defer mechanism +
# SET_LOCAL_KEEP already elide the cases where the GET is the adjacent next
# op (D2 == 0), so what this counts is the RESIDUAL the defer pass missed
# (flushed across an intervening op / branch / block boundary).  Each is a
# SET+GET pair (2 ops) a stack-scheduler could remove.  Reports the residual
# as a fraction of total SET_LOCAL+GET_LOCAL — the Lever-1A headroom for the
# pre-committed >=10% gate.
def c2_single_use_spill(funcs):
    cand = 0
    set_get_total = 0
    adjacent = 0          # SET n immediately followed by GET n (defer/KEEP should have caught)
    for fn in funcs:
        writes = Counter()
        reads = Counter()
        keep = Counter()
        for ins in fn.insns:
            if ins.op == "OP_SET_LOCAL":
                writes[ins.operand] += 1; set_get_total += 1
            elif ins.op == "OP_SET_LOCAL_KEEP":
                keep[ins.operand] += 1; set_get_total += 1
            elif ins.op in ("OP_GET_LOCAL", "OP_GET_LOCAL_FORCE"):
                reads[ins.operand] += 1; set_get_total += 1
        for a, b in zip(fn.insns, fn.insns[1:]):
            if a.op == "OP_SET_LOCAL":
                s = a.operand
                if writes[s] == 1 and keep[s] == 0 and reads[s] == 1:
                    cand += 1
                    if b.op in ("OP_GET_LOCAL", "OP_GET_LOCAL_FORCE") \
                       and b.operand == s:
                        adjacent += 1
    return cand, set_get_total, adjacent


# ============================ S-series ====================================
def s1_single_reference(funcs, refs):
    """Non-top-level functions created at exactly one MAKE_CLOSURE/THUNK
    site → single call-site → inline / lambda-lift-back candidates."""
    out = []
    for fn in funcs:
        if fn.fid == 0:
            continue
        n = refs.get((fn.cu, fn.fid), 0)
        if n == 1:
            out.append(fn)
    return out


def s2_slot_slack(funcs):
    """Per function: nLocals declared vs (max slot index touched + 1)."""
    rows = []
    total_slack = 0
    for fn in funcs:
        used = -1
        for ins in fn.insns:
            if ins.op in SLOT_OPS and ins.operand > used:
                used = ins.operand
        touched = used + 1
        slack = fn.nLocals - touched
        if slack > 0:
            rows.append((slack, fn, touched))
            total_slack += slack
    rows.sort(key=lambda r: r[0], reverse=True)
    return rows, total_slack


def s3_leaf(funcs):
    """Functions with no call-family op (pure straight-line + branches).
    Sub-flag those that still build thunks (MAKE_THUNK)."""
    leaves = []
    leaf_thunkers = []
    for fn in funcs:
        if any(ins.op in CALL_OPS for ins in fn.insns):
            continue
        leaves.append(fn)
        if any(ins.op == "OP_MAKE_THUNK" for ins in fn.insns):
            leaf_thunkers.append(fn)
    return leaves, leaf_thunkers


# ============================ B-series ====================================
def b_branches(funcs):
    """Return (b1 dead/degenerate, b2 threadable, b0 bad-target) counters,
    keyed by branch opcode."""
    b1 = Counter()  # target == fallthrough ip
    b2 = Counter()  # target is itself a JUMP
    b0 = Counter()  # target matches no instruction ip (integrity)
    for fn in funcs:
        ipmap = {ins.ip: ins for ins in fn.insns}
        for idx, ins in enumerate(fn.insns):
            if ins.target is None:
                continue
            nxt = fn.insns[idx + 1].ip if idx + 1 < len(fn.insns) else None
            if ins.target == nxt:
                b1[ins.op] += 1
            tgt = ipmap.get(ins.target)
            if tgt is None:
                b0[ins.op] += 1
            elif tgt.op == "OP_JUMP" and ins.op != "OP_JUMP":
                b2[ins.op] += 1
            elif tgt.op == "OP_JUMP" and ins.op == "OP_JUMP":
                b2[ins.op] += 1
    return b1, b2, b0


# ============================ R-series ====================================
# Register pressure (register-VM sizing).  A register VM replaces the
# SET_LOCAL/GET_LOCAL slot traffic (≈half of dispatch) with operands that
# name slots directly.  The "ideal register count" for a function is its
# register PRESSURE — the max number of slot-values simultaneously live —
# not its declared nLocals (the A-normal-form allocator rarely reuses
# slots, so nLocals over-counts).  We estimate pressure with LINEAR live
# intervals: each SET opens a value in that slot, GETs extend its last-read,
# the next SET to the slot (or function end) closes it; pressure = max
# overlap of all [def, last_read] intervals.  A GET with no prior SET is a
# parameter/preset slot, live from function entry.  Linear ⇒ exact for
# straight-line bodies (most v3 funcs), an over-estimate where a value is
# used on only one branch.
import bisect


def r1_register_pressure(funcs):
    """Return list of (pressure, fn).  See section header for the model."""
    out = []
    for fn in funcs:
        open_val = {}          # slot -> [def_ip, last_read_ip]
        intervals = []         # (def_ip, last_read_ip)
        for ins in fn.insns:
            if ins.op in ("OP_SET_LOCAL", "OP_SET_LOCAL_KEEP"):
                s = ins.operand
                if s in open_val:
                    intervals.append(tuple(open_val[s]))
                open_val[s] = [ins.ip, ins.ip]
            elif ins.op in ("OP_GET_LOCAL", "OP_GET_LOCAL_FORCE"):
                s = ins.operand
                if s in open_val:
                    open_val[s][1] = ins.ip
                else:                       # param / preset slot
                    open_val[s] = [fn.entry, ins.ip]
        for v in open_val.values():
            intervals.append(tuple(v))
        # Sweep line: +1 at each def, -1 just past each last-read.
        events = []
        for d, lr in intervals:
            events.append((d, 1))
            events.append((lr + 1, -1))
        events.sort()
        cur = peak = 0
        for _, delta in events:
            cur += delta
            if cur > peak:
                peak = cur
        out.append((peak, fn))
    return out


# ============================ report ======================================
def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dump", help="NIX_V3_EMIT_BYTECODE / --emit-bytecode dump")
    ap.add_argument("--top", type=int, default=15)
    ap.add_argument("--cu", default=None,
                    help="analyze only CU index N (0-based) or 'last' "
                         "(usually the user expression); default: all CUs")
    args = ap.parse_args()

    funcs, refs = parse_module(args.dump)

    if args.cu is not None:
        all_cus = sorted({f.cu for f in funcs})
        target = all_cus[-1] if args.cu == "last" else int(args.cu)
        funcs = [f for f in funcs if f.cu == target]
        if not funcs:
            print(f"analyze-operands: no functions in CU {target} "
                  f"(CUs present: {all_cus})", file=sys.stderr)
            sys.exit(1)
        print(f"[scope: CU {target} only — refs/counts restricted to it]")
    n_insn = sum(len(f.insns) for f in funcs)
    if not funcs or n_insn == 0:
        print(f"analyze-operands: no framed functions parsed from "
              f"{args.dump} (need `--emit-bytecode` / disassembleModule "
              f"output, not the old flat dump)", file=sys.stderr)
        sys.exit(1)

    n_cu = len({f.cu for f in funcs})
    print("=" * 70)
    print(f"v3 OPERAND + STRUCTURE scan — {args.dump}")
    print(f"  CUs: {n_cu}   functions: {len(funcs)}   instructions: {n_insn}")
    print("=" * 70)

    # ---- D-series -----------------------------------------------------
    s, d = d_adjacent(funcs, "OP_GET_LOCAL", "OP_GET_LOCAL")
    tot = s + d
    print("\n## D1  GET_LOCAL n ; GET_LOCAL n   (same vs different slot)")
    print(f"  same-slot (DUP candidate): {s}    different-slot (normal): {d}")
    if tot:
        print(f"  → {100.0*s/tot:.1f}% same-slot; opcode-only n-grams lump all "
              f"{tot} together.")

    s, d = d_adjacent(funcs, "OP_SET_LOCAL", "OP_GET_LOCAL")
    print("\n## D2  SET_LOCAL n ; GET_LOCAL n   (SET_LOCAL_KEEP residual)")
    print(f"  same-slot (KEEP MISS): {s}    different-slot: {d}")
    print("  → SET_LOCAL_KEEP is complete iff same-slot == 0."
          if s == 0 else
          f"  → {s} same-slot residual — audit the emit site.")

    by_primop, gcalls, attr = d3_primop_via_generic_call(funcs)
    print("\n## D3  generic OP_CALL attributed to nearest preceding LIT_PRIMOP")
    print(f"  generic OP_CALL: {gcalls}   attributed to a primop: {attr}"
          "   (rest are user-closure calls)")
    print(f"  {'primop (likely callee)':<24}{'generic-CALL sites':>20}")
    for nm, c in by_primop.most_common(args.top):
        print(f"  {nm:<24}{c:>20}")

    # ---- C-series -----------------------------------------------------
    cn, csave, cby, crows = c1_const_spill(funcs)
    print("\n## C1  constant spill-and-reload  (LIT_* ; SET_LOCAL s, s write-once)")
    print(f"  {cn} const-spill bindings → {csave} ops saveable by remat "
          f"({100.0*csave/n_insn:.1f}% of {n_insn} static insns).")
    if cn:
        print("  by literal op: " +
              ", ".join(f"{op.replace('OP_LIT_','')}={c}"
                        for op, c in cby.most_common()))
        print(f"  {'cu/fid':<10}{'name':<22}{'const-spills':>13}")
        for cnt, fn in crows[:args.top]:
            print(f"  {f'{fn.cu}/{fn.fid}':<10}{(fn.name or '<anon>'):<22}"
                  f"{cnt:>13}")

    # ---- C2: Lever 1A residual single-use spill -----------------------
    c2cand, c2sg, c2adj = c2_single_use_spill(funcs)
    print("\n## C2  single-use spill residual  (write-once+read-once SET_LOCAL; "
          "Lever 1A headroom)")
    print(f"  {c2cand} single-use spills → {2*c2cand} SET+GET ops removable")
    if c2sg:
        print(f"  total SET_LOCAL+GET_LOCAL(+KEEP/FORCE) = {c2sg}; removable "
              f"pairs = {100.0*2*c2cand/c2sg:.1f}% of that traffic (STATIC).")
    print(f"  of those, {c2adj} have an ADJACENT GET (defer/KEEP gap); "
          f"{c2cand-c2adj} are non-adjacent (the genuine scheduler target).")

    # ---- S-series -----------------------------------------------------
    single = s1_single_reference(funcs, refs)
    nonzero = [f for f in funcs if f.fid != 0]
    print("\n## S1  single-reference functions  (one MAKE_* site → inline)")
    print(f"  {len(single)} of {len(nonzero)} non-top-level functions are "
          f"created at exactly one site.")
    print(f"  {'cu/fid':<10}{'name':<22}{'arity':>6}{'insns':>7}")
    for fn in sorted(single, key=lambda f: len(f.insns))[:args.top]:
        print(f"  {f'{fn.cu}/{fn.fid}':<10}{(fn.name or '<anon>'):<22}"
              f"{fn.arity:>6}{len(fn.insns):>7}")

    rows, total_slack = s2_slot_slack(funcs)
    print("\n## S2  slot slack  (declared nLocals − max slot index touched − 1)")
    print(f"  {total_slack} declared-but-untouched slots across "
          f"{len(rows)} functions.")
    print(f"  {'cu/fid':<10}{'name':<22}{'nLocals':>8}{'touched':>8}{'slack':>7}")
    for slack, fn, touched in rows[:args.top]:
        print(f"  {f'{fn.cu}/{fn.fid}':<10}{(fn.name or '<anon>'):<22}"
              f"{fn.nLocals:>8}{touched:>8}{slack:>7}")
    print("  → hint only: high slots may be reserved scratch; confirm before"
          " shrinking frames.")

    leaves, leaf_thunkers = s3_leaf(funcs)
    print("\n## S3  leaf functions  (no call-family op)")
    print(f"  {len(leaves)} of {len(funcs)} functions are leaf "
          f"(straight-line + branches only);")
    print(f"  {len(leaf_thunkers)} of those still MAKE_THUNK — thunk-elision /"
          " strictness candidates.")
    print(f"  {'cu/fid':<10}{'name':<22}{'arity':>6}{'thunks':>7}")
    for fn in sorted(leaf_thunkers,
                     key=lambda f: sum(1 for i in f.insns
                                       if i.op == "OP_MAKE_THUNK"),
                     reverse=True)[:args.top]:
        nthunk = sum(1 for i in fn.insns if i.op == "OP_MAKE_THUNK")
        print(f"  {f'{fn.cu}/{fn.fid}':<10}{(fn.name or '<anon>'):<22}"
              f"{fn.arity:>6}{nthunk:>7}")

    # ---- B-series -----------------------------------------------------
    b1, b2, b0 = b_branches(funcs)
    print("\n## B1  branch-to-fallthrough  (target == next ip)")
    if sum(b1.values()) == 0:
        print("  none — no branch jumps to its own fallthrough.")
    else:
        for op, c in b1.most_common():
            kind = ("dead unconditional jump" if op == "OP_JUMP"
                    else "no-op conditional (both paths fall through)")
            print(f"  {op:<20}{c:>6}  ({kind})")

    print("\n## B2  branch-to-jump  (target is itself a JUMP → threadable)")
    if sum(b2.values()) == 0:
        print("  none — no jump-to-jump threading opportunity.")
    else:
        for op, c in b2.most_common():
            print(f"  {op:<20}{c:>6}")

    if sum(b0.values()):
        print("\n## B0  INTEGRITY: branch target matching no instruction ip")
        for op, c in b0.most_common():
            print(f"  {op:<20}{c:>6}  ← width-table desync? investigate")

    # ---- R-series: register pressure / register-VM sizing -------------
    rp = r1_register_pressure(funcs)
    pressures = sorted(p for p, _ in rp)
    nlocals = sorted(fn.nLocals for fn in funcs)

    def quant(sl, q):
        if not sl:
            return 0
        return sl[min(len(sl) - 1, int(q * (len(sl) - 1)))]

    print("\n## R1  register pressure  (max simultaneously-live local slots;"
          " linear est.)")
    print(f"  functions: {len(funcs)}")
    print(f"  {'pct':<6}{'reg-pressure':>13}{'declared nLocals':>18}")
    for lbl, q in (("p50", 0.50), ("p90", 0.90), ("p99", 0.99),
                   ("max", 1.00)):
        print(f"  {lbl:<6}{quant(pressures, q):>13}{quant(nlocals, q):>18}")
    print("  register-file sizing (% of functions that never spill):")
    for rf in (4, 8, 16, 32):
        cov = bisect.bisect_right(pressures, rf)
        print(f"    {rf:>2} regs → {100.0 * cov / len(pressures):5.1f}%"
              f"   (declared-nLocals coverage: "
              f"{100.0 * bisect.bisect_right(nlocals, rf) / len(nlocals):5.1f}%)")
    mean_p = sum(pressures) / len(pressures)
    mean_n = sum(nlocals) / len(nlocals)
    print(f"  mean pressure {mean_p:.2f} vs mean declared nLocals {mean_n:.2f}"
          f"  ({mean_n - mean_p:.2f} slots/fn the allocator over-reserves)")

    # ---- caveat -------------------------------------------------------
    print("\n## Caveat")
    print("  STATIC. Confirm any candidate execution-weighted (NIX_VM_OPCOUNTS")
    print("  / NIX_VM_BIGRAMS) before implementing — a hot-loop pattern can")
    print("  dwarf its static count, and a frequent static pattern can be")
    print("  dynamically cold (the SET_LOCAL_KEEP lesson).")


if __name__ == "__main__":
    main()
