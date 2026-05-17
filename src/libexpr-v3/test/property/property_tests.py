#!/usr/bin/env python3
"""
Property-test suite for TW vs v3-direct primop semantic parity.

For each primop class, generates N random inputs (deterministic via
seeded PRNG), evaluates the Nix expression on both TW and v3-direct,
and asserts byte-exact output parity.  When either side throws, we
require that BOTH sides throw with overlapping error tokens.

This is the safety net for the bytecode-emit-primops architectural
refactor (project_a12b_depth5000.md): as we rewrite each C primop
as a v3 bytecode template, this suite guarantees we don't drift
from TW's exact semantics.

Usage:
    python3 property_tests.py [--seed N] [--cases N] [--primop NAME] [--verbose]

Exit code: 0 if all tests pass, 1 if any fail, 2 on harness error.

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
Input Output Group.  SPDX-License-Identifier: Apache-2.0.
"""

from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, List, Optional, Tuple

# --------------------------------------------------------------------
# Nix value generators
# --------------------------------------------------------------------

def gen_int(rng: random.Random, lo: int = -100, hi: int = 100) -> int:
    return rng.randint(lo, hi)


def gen_bool(rng: random.Random) -> str:
    return "true" if rng.random() < 0.5 else "false"


def gen_string(rng: random.Random, max_len: int = 8) -> str:
    chars = "abcdefghijklmnop"
    n = rng.randint(0, max_len)
    return ''.join(rng.choice(chars) for _ in range(n))


def gen_string_nix(rng: random.Random, max_len: int = 8) -> str:
    return f'"{gen_string(rng, max_len)}"'


def gen_list_int(rng: random.Random, n_min: int = 0, n_max: int = 15) -> str:
    """Return a Nix list literal of ints, e.g. '[ 1 2 3 ]'."""
    n = rng.randint(n_min, n_max)
    if n == 0:
        return "[ ]"
    items = " ".join(str(gen_int(rng)) for _ in range(n))
    return f"[ {items} ]"


def gen_list_string(rng: random.Random, n_min: int = 0, n_max: int = 8) -> str:
    n = rng.randint(n_min, n_max)
    if n == 0:
        return "[ ]"
    items = " ".join(gen_string_nix(rng) for _ in range(n))
    return f"[ {items} ]"


def gen_attrs_int(rng: random.Random, n_min: int = 0, n_max: int = 8) -> str:
    """Return a Nix attrset literal with int values, e.g. '{ a = 1; b = 2; }'."""
    n = rng.randint(n_min, n_max)
    names = sorted(set(gen_string(rng, 3) or 'a' for _ in range(n)))
    if not names:
        return "{ }"
    entries = " ".join(f'{name} = {gen_int(rng)};' for name in names)
    return f"{{ {entries} }}"


# --------------------------------------------------------------------
# Test case
# --------------------------------------------------------------------

@dataclass
class TestCase:
    primop: str
    expr: str
    seed: int

    def __str__(self) -> str:
        return f"{self.primop}/seed={self.seed}: {self.expr}"


# Each primop test is a function: (rng) -> Nix-expression string.
TestGenerator = Callable[[random.Random], str]


# --------------------------------------------------------------------
# Test generators per primop
# --------------------------------------------------------------------

def t_head(rng):
    return f"builtins.head {gen_list_int(rng, 1, 12)}"


def t_tail(rng):
    return f"builtins.tail {gen_list_int(rng, 1, 12)}"


def t_length(rng):
    return f"builtins.length {gen_list_int(rng, 0, 20)}"


def t_elemAt(rng):
    lst = gen_list_int(rng, 1, 12)
    # Compute valid index: 0..len-1
    n = lst.count(" ") if lst != "[ ]" else 0
    n = max(0, n - 1)  # rough estimate; we'll bound by n above
    idx = rng.randint(0, n if n > 0 else 0)
    return f"builtins.elemAt {lst} {idx}"


def t_elem(rng):
    lst = gen_list_int(rng, 0, 12)
    needle = gen_int(rng)
    return f"builtins.elem {needle} {lst}"


def t_map_inc(rng):
    return f"builtins.map (x: x + 1) {gen_list_int(rng, 0, 15)}"


def t_filter_even(rng):
    return f"builtins.filter (x: x / 2 * 2 == x) {gen_list_int(rng, 0, 15)}"


def t_foldl_sum(rng):
    return f"builtins.foldl' (a: b: a + b) 0 {gen_list_int(rng, 0, 15)}"


def t_foldl_mul(rng):
    return f"builtins.foldl' (a: b: a * b) 1 {gen_list_int(rng, 0, 8)}"


def t_concatMap(rng):
    return f"builtins.concatMap (x: [ x x ]) {gen_list_int(rng, 0, 8)}"


def t_genList(rng):
    n = rng.randint(0, 20)
    return f"builtins.genList (i: i * {gen_int(rng, 1, 5)}) {n}"


def t_all_true(rng):
    return f"builtins.all (x: x < 1000) {gen_list_int(rng, 0, 10)}"


def t_any_false(rng):
    return f"builtins.any (x: x > 1000) {gen_list_int(rng, 0, 10)}"


def t_attrNames(rng):
    return f"builtins.attrNames {gen_attrs_int(rng, 0, 8)}"


def t_attrValues(rng):
    return f"builtins.attrValues {gen_attrs_int(rng, 0, 8)}"


def t_hasAttr(rng):
    attrs = gen_attrs_int(rng, 1, 6)
    # Pick a name that's likely in OR likely out of attrs.
    name = gen_string(rng, 3) or "a"
    return f'builtins.hasAttr "{name}" {attrs}'


def t_getAttr_default(rng):
    attrs = gen_attrs_int(rng, 1, 6)
    name = gen_string(rng, 3) or "a"
    default = gen_int(rng)
    return f'({attrs}).{name} or {default}'


def t_mapAttrs(rng):
    return f'builtins.mapAttrs (n: v: v + 1) {gen_attrs_int(rng, 0, 6)}'


def t_listToAttrs(rng):
    n = rng.randint(0, 6)
    pairs = []
    for i in range(n):
        nm = gen_string(rng, 3) or f"k{i}"
        val = gen_int(rng)
        pairs.append(f'{{ name = "{nm}"; value = {val}; }}')
    body = " ".join(pairs)
    return f"builtins.listToAttrs [ {body} ]"


def t_removeAttrs(rng):
    attrs = gen_attrs_int(rng, 2, 8)
    n_remove = rng.randint(0, 3)
    names = " ".join(f'"{gen_string(rng, 3) or "x"}"' for _ in range(n_remove))
    return f"builtins.removeAttrs {attrs} [ {names} ]"


def t_intersectAttrs(rng):
    a = gen_attrs_int(rng, 1, 6)
    b = gen_attrs_int(rng, 1, 6)
    return f"builtins.intersectAttrs {a} {b}"


def t_catAttrs(rng):
    n = rng.randint(0, 5)
    name = gen_string(rng, 3) or "a"
    entries = []
    for _ in range(n):
        # Some entries have the attr; some don't.
        if rng.random() < 0.5:
            entries.append(f'{{ {name} = {gen_int(rng)}; other = 0; }}')
        else:
            entries.append(f'{{ other = {gen_int(rng)}; }}')
    body = " ".join(entries)
    return f'builtins.catAttrs "{name}" [ {body} ]'


def t_concatStringsSep(rng):
    n = rng.randint(0, 8)
    sep = gen_string(rng, 2)
    parts = " ".join(gen_string_nix(rng, 4) for _ in range(n))
    return f'builtins.concatStringsSep "{sep}" [ {parts} ]'


def t_substring(rng):
    s = gen_string(rng, 12)
    start = gen_int(rng, 0, 10)
    length = gen_int(rng, 0, 10)
    return f'builtins.substring {start} {length} "{s}"'


def t_stringLength(rng):
    return f'builtins.stringLength {gen_string_nix(rng, 20)}'


def t_toString_int(rng):
    return f'builtins.toString {gen_int(rng)}'


def t_arith_add(rng):
    return f"{gen_int(rng)} + {gen_int(rng)}"


def t_arith_sub(rng):
    return f"{gen_int(rng)} - {gen_int(rng)}"


def t_arith_mul(rng):
    return f"{gen_int(rng, -50, 50)} * {gen_int(rng, -50, 50)}"


def t_arith_div(rng):
    a = gen_int(rng)
    b = gen_int(rng, 1, 100)  # avoid div by 0
    return f"{a} / {b}"


def t_compare_lt(rng):
    return f"{gen_int(rng)} < {gen_int(rng)}"


def t_compare_eq_list(rng):
    a = gen_list_int(rng, 0, 8)
    b = gen_list_int(rng, 0, 8)
    return f"{a} == {b}"


def t_compare_eq_attrs(rng):
    a = gen_attrs_int(rng, 0, 4)
    b = gen_attrs_int(rng, 0, 4)
    return f"{a} == {b}"


def t_isType(rng):
    candidates = [
        gen_int(rng),
        gen_string_nix(rng),
        gen_list_int(rng, 0, 3),
        gen_attrs_int(rng, 0, 3),
        gen_bool(rng),
        "null",
    ]
    expr = rng.choice(candidates)
    predicate = rng.choice(["isInt", "isString", "isList", "isAttrs", "isBool", "isNull"])
    return f"builtins.{predicate} ({expr})"


def t_list_concat(rng):
    a = gen_list_int(rng, 0, 8)
    b = gen_list_int(rng, 0, 8)
    return f"{a} ++ {b}"


def t_attrs_merge(rng):
    a = gen_attrs_int(rng, 0, 5)
    b = gen_attrs_int(rng, 0, 5)
    return f"{a} // {b}"


def t_string_concat(rng):
    a = gen_string_nix(rng, 6)
    b = gen_string_nix(rng, 6)
    return f"{a} + {b}"


def t_seq(rng):
    a = gen_int(rng)
    b = gen_int(rng)
    return f"builtins.seq ({a}) ({b})"


def t_deepSeq(rng):
    return f"builtins.deepSeq {gen_list_int(rng, 0, 5)} {gen_int(rng)}"


def t_tryEval_ok(rng):
    return f"(builtins.tryEval ({gen_int(rng)})).value"


def t_tryEval_throw(rng):
    return f"(builtins.tryEval (throw \"err\")).success"


# --------------------------------------------------------------------
# Error-class tests: BOTH TW and v3 must throw with matching class.
# These catch silent divergences where v3 succeeds-with-junk while
# TW raises (or vice versa).
# --------------------------------------------------------------------

def t_err_div_by_zero(rng):
    return f"{gen_int(rng)} / 0"


def t_err_elemAt_oob(rng):
    n = rng.randint(0, 5)
    lst = gen_list_int(rng, n, n)  # length exactly n
    return f"builtins.elemAt {lst} {n + rng.randint(0, 3)}"  # idx >= n


def t_err_head_empty(rng):
    # Confuse the optimizer by building empty list dynamically.
    return f"builtins.head (builtins.genList (i: i) 0)"


def t_err_missing_attr(rng):
    attrs = gen_attrs_int(rng, 1, 3)
    return f'({attrs}).nonexistent_{gen_string(rng, 4)}'


def t_err_typeerror_add(rng):
    return f'builtins.head 42'  # head not a list


def t_err_throw_msg(rng):
    msg = gen_string(rng, 8)
    return f'throw "fail-{msg}"'


def t_err_assert_false(rng):
    return f"assert false; {gen_int(rng)}"


# --------------------------------------------------------------------
# Nested / compositional tests.  These catch lazy-vs-strict
# differences and recursive force interactions.
# --------------------------------------------------------------------

def t_nested_list_of_lists(rng):
    n = rng.randint(0, 5)
    inners = " ".join(gen_list_int(rng, 0, 4) for _ in range(n))
    return f"builtins.map builtins.length [ {inners} ]"


def t_nested_attrs_of_lists(rng):
    # Make an attrset with list values, then sum them up.
    n = rng.randint(1, 4)
    entries = []
    for i in range(n):
        nm = gen_string(rng, 3) or f"k{i}"
        entries.append(f'{nm} = {gen_list_int(rng, 0, 5)};')
    body = " ".join(entries)
    return f"builtins.map (xs: builtins.foldl' (a: b: a + b) 0 xs) (builtins.attrValues {{ {body} }})"


def t_let_in(rng):
    a = gen_int(rng)
    b = gen_int(rng)
    return f"let x = {a}; y = {b}; in x + y * 2"


def t_let_rec_fixpoint(rng):
    # Pre-known: factorial of small n.
    n = rng.randint(0, 6)
    return f"let rec = n: if n <= 1 then 1 else n * rec (n - 1); in rec {n}"


def t_with_attrs(rng):
    a = gen_int(rng)
    b = gen_int(rng)
    return f"with {{ x = {a}; y = {b}; }}; x + y"


def t_assert_true(rng):
    a = gen_int(rng)
    return f"assert {a} == {a}; {a} + 1"


def t_string_interp(rng):
    a = gen_int(rng)
    b = gen_string(rng, 4)
    return f'"v=${{toString {a}}}-{b}"'


def t_rec_self_ref(rng):
    a = gen_int(rng)
    return f"(rec {{ x = {a}; y = x + 1; z = y * 2; }}).z"


def t_curry_apply(rng):
    a = gen_int(rng)
    b = gen_int(rng)
    return f"(a: b: a * 10 + b) {a} {b}"


def t_higher_order_compose(rng):
    a = gen_int(rng)
    return f"(let f = x: x + 1; g = x: x * 2; in g (f {a}))"


# --------------------------------------------------------------------
# Test registry
# --------------------------------------------------------------------

TESTS: List[Tuple[str, TestGenerator]] = [
    ("head", t_head),
    ("tail", t_tail),
    ("length", t_length),
    ("elemAt", t_elemAt),
    ("elem", t_elem),
    ("map", t_map_inc),
    ("filter", t_filter_even),
    ("foldl_sum", t_foldl_sum),
    ("foldl_mul", t_foldl_mul),
    ("concatMap", t_concatMap),
    ("genList", t_genList),
    ("all", t_all_true),
    ("any", t_any_false),
    ("attrNames", t_attrNames),
    ("attrValues", t_attrValues),
    ("hasAttr", t_hasAttr),
    ("getAttr_default", t_getAttr_default),
    ("mapAttrs", t_mapAttrs),
    ("listToAttrs", t_listToAttrs),
    ("removeAttrs", t_removeAttrs),
    ("intersectAttrs", t_intersectAttrs),
    ("catAttrs", t_catAttrs),
    ("concatStringsSep", t_concatStringsSep),
    ("substring", t_substring),
    ("stringLength", t_stringLength),
    ("toString_int", t_toString_int),
    ("arith_add", t_arith_add),
    ("arith_sub", t_arith_sub),
    ("arith_mul", t_arith_mul),
    ("arith_div", t_arith_div),
    ("compare_lt", t_compare_lt),
    ("compare_eq_list", t_compare_eq_list),
    ("compare_eq_attrs", t_compare_eq_attrs),
    ("isType", t_isType),
    ("list_concat", t_list_concat),
    ("attrs_merge", t_attrs_merge),
    ("string_concat", t_string_concat),
    ("seq", t_seq),
    ("deepSeq", t_deepSeq),
    ("tryEval_ok", t_tryEval_ok),
    ("tryEval_throw", t_tryEval_throw),
    # Error-class tests
    ("err_div_by_zero", t_err_div_by_zero),
    ("err_elemAt_oob", t_err_elemAt_oob),
    ("err_head_empty", t_err_head_empty),
    ("err_missing_attr", t_err_missing_attr),
    ("err_typeerror_add", t_err_typeerror_add),
    ("err_throw_msg", t_err_throw_msg),
    ("err_assert_false", t_err_assert_false),
    # Nested / compositional
    ("nested_list_of_lists", t_nested_list_of_lists),
    ("nested_attrs_of_lists", t_nested_attrs_of_lists),
    ("let_in", t_let_in),
    ("let_rec_fixpoint", t_let_rec_fixpoint),
    ("with_attrs", t_with_attrs),
    ("assert_true", t_assert_true),
    ("string_interp", t_string_interp),
    ("rec_self_ref", t_rec_self_ref),
    ("curry_apply", t_curry_apply),
    ("higher_order_compose", t_higher_order_compose),
]


# --------------------------------------------------------------------
# Evaluation
# --------------------------------------------------------------------

@dataclass
class EvalResult:
    rc: int
    stdout: str
    stderr: str

    @property
    def threw(self) -> bool:
        return self.rc != 0

    def error_class(self) -> Optional[str]:
        """Extract the leading error class token from stderr, e.g. 'error:',
        'TypeError', 'AssertionError'.  Used to compare error semantics
        without depending on exact message text."""
        if not self.threw:
            return None
        m = re.search(r'\b(error|assertion failed|infinite recursion|division by zero|stack overflow|index .* out of bounds|undefined variable|missing attribute)\b',
                      self.stderr, re.IGNORECASE)
        return m.group(1).lower() if m else "error"


def run_eval(nix_bin: Path, expr: str, mode: str, timeout: float = 10.0) -> EvalResult:
    env = os.environ.copy()
    if mode == "v3":
        env["NIX_V3_DIRECT_EVAL"] = "1"
        env["NIX_V3_SKIP_INSTALLABLE_PREEVAL"] = "1"
    cmd = [str(nix_bin), "eval", "--impure", "--expr", expr]
    try:
        proc = subprocess.run(cmd, env=env, capture_output=True,
                              timeout=timeout, text=True)
    except subprocess.TimeoutExpired:
        return EvalResult(rc=124, stdout="", stderr=f"<TIMEOUT after {timeout}s>")
    return EvalResult(rc=proc.returncode, stdout=proc.stdout.strip(),
                      stderr=proc.stderr.strip())


# --------------------------------------------------------------------
# Comparison
# --------------------------------------------------------------------

def filter_warnings(s: str) -> str:
    """Strip warning lines we want to ignore in comparisons."""
    lines = s.split("\n")
    out = []
    for line in lines:
        if re.match(r"^warning: Git tree", line):
            continue
        if re.match(r"^warning: Nix search path", line):
            continue
        if re.match(r"^Failed to increase stack size", line):
            continue
        out.append(line)
    return "\n".join(out).strip()


def compare(tw: EvalResult, v3: EvalResult) -> Tuple[bool, str]:
    """Returns (parity_ok, reason).  Parity is OK if:
       - both succeed and stdout matches, OR
       - both fail with overlapping error class.
    """
    if tw.threw and v3.threw:
        tc = tw.error_class() or "error"
        vc = v3.error_class() or "error"
        if tc == vc:
            return True, f"both threw [{tc}]"
        return False, f"both threw but different class: tw='{tc}' v3='{vc}'"
    if tw.threw != v3.threw:
        return False, f"throw mismatch: tw.threw={tw.threw} v3.threw={v3.threw}"
    tw_out = filter_warnings(tw.stdout)
    v3_out = filter_warnings(v3.stdout)
    if tw_out == v3_out:
        return True, "outputs match"
    return False, f"output mismatch: tw='{tw_out!r}' v3='{v3_out!r}'"


# --------------------------------------------------------------------
# Main
# --------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=20260517,
                        help="PRNG seed (default: 20260517 for reproducibility)")
    parser.add_argument("--cases", type=int, default=10,
                        help="Number of random test cases per primop (default: 10)")
    parser.add_argument("--primop", type=str, default=None,
                        help="Only run tests for this primop name")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="Print every test case")
    parser.add_argument("--nix-bin", type=Path, default=None,
                        help="Path to nix binary (default: ../../../../build/src/nix/nix)")
    args = parser.parse_args()

    here = Path(__file__).resolve().parent
    nix_bin = args.nix_bin or (here.parent.parent.parent.parent / "build/src/nix/nix")
    if not nix_bin.is_file() or not os.access(nix_bin, os.X_OK):
        print(f"ERROR: nix binary not found or not executable: {nix_bin}", file=sys.stderr)
        return 2

    rng = random.Random(args.seed)

    tests = TESTS
    if args.primop:
        tests = [t for t in TESTS if t[0] == args.primop]
        if not tests:
            print(f"ERROR: unknown primop '{args.primop}'.  Available: {[t[0] for t in TESTS]}",
                  file=sys.stderr)
            return 2

    print(f"=== TW vs v3-direct primop parity (seed={args.seed}, cases={args.cases}) ===")
    print(f"    nix-bin: {nix_bin}")
    print()

    total_pass = 0
    total_fail = 0
    failures: List[Tuple[TestCase, EvalResult, EvalResult, str]] = []

    for primop_name, gen in tests:
        primop_pass = 0
        primop_fail = 0
        for i in range(args.cases):
            # Per-test deterministic sub-seed for repeatable failures.
            sub_rng = random.Random(rng.randint(0, 2**31 - 1))
            expr = gen(sub_rng)
            seed_used = rng.getstate()[1][0]  # crude record
            tc = TestCase(primop=primop_name, expr=expr, seed=i)
            tw = run_eval(nix_bin, expr, "tw")
            v3 = run_eval(nix_bin, expr, "v3")
            ok, reason = compare(tw, v3)
            if ok:
                primop_pass += 1
                total_pass += 1
                if args.verbose:
                    print(f"  PASS [{primop_name}/{i}] {reason}: {expr}")
            else:
                primop_fail += 1
                total_fail += 1
                failures.append((tc, tw, v3, reason))
                if args.verbose:
                    print(f"  FAIL [{primop_name}/{i}] {reason}")
                    print(f"    expr: {expr}")
                    print(f"    tw  : rc={tw.rc} out='{tw.stdout}' err='{tw.stderr[:80]}'")
                    print(f"    v3  : rc={v3.rc} out='{v3.stdout}' err='{v3.stderr[:80]}'")
        status = "OK   " if primop_fail == 0 else "FAIL "
        print(f"  {status} {primop_name:24s} {primop_pass:3d}/{args.cases} pass")

    print()
    print(f"=== Summary: {total_pass} pass, {total_fail} fail "
          f"({len(tests) * args.cases} total) ===")

    if total_fail > 0:
        print()
        print("=== Failure details ===")
        max_print = 20
        for i, (tc, tw, v3, reason) in enumerate(failures[:max_print]):
            print(f"  [{i}] {tc.primop}/seed={tc.seed} — {reason}")
            print(f"      expr: {tc.expr}")
            print(f"      tw  : rc={tw.rc} stdout={tw.stdout!r}")
            if tw.stderr:
                print(f"          : stderr={tw.stderr[:120]!r}")
            print(f"      v3  : rc={v3.rc} stdout={v3.stdout!r}")
            if v3.stderr:
                print(f"          : stderr={v3.stderr[:120]!r}")
        if len(failures) > max_print:
            print(f"  ... and {len(failures) - max_print} more.")

    return 0 if total_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
