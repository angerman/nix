#!/usr/bin/env python3
# WS-D differential-fuzzer framework — grammar-aware MINIMIZER + FIXTURE-EMIT.
#
# Complements the directed fuzzer (fuzz2.py) that FINDS divergences. This module
# takes a divergence a human/fuzzer already surfaced and (1) shrinks it to a
# minimal repro that preserves the SAME divergence signature, gated by a K-run
# stability check, then (2) emits it as a drop-in @@@-row fixture in the exact
# format of src/libexpr-v3/test/run-primop-edge-parity-tests.sh.
#
# Design (see src/libexpr-v3/lode/PLAN_TIERD_FUZZ_MONITOR_2026-08-09.md §2.4/§2.5):
#   - 3-way oracle P=v3-eval / O=nix+DIRECT_EVAL / T=nix(TW), copied from fuzz2.py.
#   - A comparable SIGNATURE = (kind, P/O/T class-pattern, normalized error-kinds).
#     Success VALUES are deliberately EXCLUDED from the signature so a shrink that
#     changes the value while keeping the divergence (e.g. shrinking a list in a
#     VAL divergence) is accepted; error KINDS are INCLUDED so a shrink can never
#     drift to a different error.
#   - shrink() greedily accepts a strictly-smaller candidate iff its signature ==
#     the target, confirmed by a K-run stability gate (flaky => quarantined).
#   - emit: CLASS (v3 succeeds / TW throws) => NEG row asserting TW's fragment;
#     VAL (both succeed, differ) => POS row asserting TW's value; CLASS the other
#     way (v3 throws / TW succeeds) => POS row asserting TW's value.
#
# The oracle backend is a pluggable `runner` so the shrink/stability/emit logic
# can be demonstrated against a synthetic injected divergence without a live bug.
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
#   Input Output Group.  SPDX-License-Identifier: Apache-2.0
import subprocess, os, re, sys

ROOT = "/Users/angerman/Projects/iohk/nix"
V3 = f"{ROOT}/build/src/libexpr-v3/v3-eval"
NIX = f"{ROOT}/build/src/nix/nix"

# ---------------------------------------------------------------------------
# 3-way oracle backend (copied from fuzz2.py; kept verbatim in spirit so the two
# tools share the exact engine-invocation idiom). Copied, not imported: importing
# fuzz2.py would run its top-level surface-diff sweep on import.
# ---------------------------------------------------------------------------
def run(mode, expr, timeout=10):
    env = dict(os.environ)
    env["NIX_V3_MAX_WALL_TIME"] = "8s"; env["NIX_V3_MAX_HEAP"] = "2G"
    if mode == "P":
        env["NIX_V3_DIRECT_EVAL"] = "1"; cmd = [V3, "--expr", expr]
    elif mode == "O":
        env["NIX_V3_DIRECT_EVAL"] = "1"; env.pop("NIX_V3_REQUIRE", None)
        cmd = [NIX, "eval", "--impure", "--expr", expr]
    else:
        env.pop("NIX_V3_DIRECT_EVAL", None)
        cmd = [NIX, "eval", "--impure", "--expr", expr]
    try:
        r = subprocess.run(cmd, env=env, capture_output=True, text=True,
                           errors="replace", timeout=timeout)
        lines = [l for l in (r.stdout + r.stderr).splitlines() if l.strip()]
        return (0 if r.returncode == 0 else 1,
                r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "",
                lines[-1] if lines else "")
    except subprocess.TimeoutExpired:
        return (2, "<timeout>", "<timeout>")


def norm_err(s):  # normalize an error line to a comparable KIND signature
    s = re.sub(r'\x1b\[[0-9;]*m', '', s)                        # ansi
    s = re.sub(r'/nix/store/[a-z0-9]{32}-', '/nix/store/H-', s)  # store hashes
    s = re.sub(r"at «[^»]*»:\d+:\d+", "at POS", s); s = re.sub(r":\d+:\d+", "", s)
    s = re.sub(r'\bv3-eval\b', '', s); s = s.replace("error:", "").strip().lower()
    return s[:70]


def terminal_fragment(err):
    # A human-readable (case-preserving) error fragment for a NEG @@@ row.
    s = re.sub(r'\x1b\[[0-9;]*m', '', err).strip()
    s = re.sub(r'^\s*(error|v3-eval error)\s*:\s*', '', s)
    s = re.sub(r'/nix/store/[a-z0-9]{32}-', '/nix/store/H-', s)
    s = re.sub(r"\s+at «[^»]*»:\d+:\d+", "", s).strip()
    return s


# ---------------------------------------------------------------------------
# Signature: what the shrinker must preserve.
# ---------------------------------------------------------------------------
def _cls(rc):
    return {0: "ok", 1: "throw", 2: "timeout"}.get(rc, "throw")


def signature(expr, runner=run, timeout=10):
    """Return (sig, details). sig is a hashable tuple identifying the divergence:
       (kind, Pclass, Oclass, Tclass, Perr, Oerr, Terr) where err-slots are the
       normalized error-KIND for a throwing engine and '' for a succeeding one
       (success VALUES excluded on purpose — see module header)."""
    prc, pv, pe = runner("P", expr, timeout)
    orc, ov, oe = runner("O", expr, timeout)
    trc, tv, te = runner("T", expr, timeout)
    pc, oc, tc = _cls(prc), _cls(orc), _cls(trc)
    pn = norm_err(pe) if pc != "ok" else ""
    on = norm_err(oe) if oc != "ok" else ""
    tn = norm_err(te) if tc != "ok" else ""
    if pc != oc or (pc == "ok" and oc == "ok" and pv != ov):
        kind = "SPLIT"                      # v3-internal opcode/primop split
    elif oc == "ok" and tc == "ok":
        kind = "VAL" if ov != tv else "AGREE"
    elif oc != tc:
        kind = "CLASS"                      # throw-vs-succeed vs TW (the killer)
    elif oc == "throw" and tc == "throw":
        kind = "KIND" if on != tn else "AGREE"
    else:
        kind = "AGREE"
    sig = (kind, pc, oc, tc, pn, on, tn)
    details = {"kind": kind, "p": (pc, pv, pe), "o": (oc, ov, oe),
               "t": (tc, tv, te)}
    return sig, details


def stable_signature(expr, K=3, runner=run, timeout=10):
    """Re-run K times. Returns (sig, is_stable); is_stable is False if the
       verdict flickers across runs (a flake => quarantine, never shrink/file)."""
    sigs = [signature(expr, runner, timeout)[0] for _ in range(K)]
    return sigs[0], all(s == sigs[0] for s in sigs)


# ---------------------------------------------------------------------------
# Lightweight bracket/string-aware scanning (enough for the grammar-aware moves;
# not a full Nix parser — invalid candidates are simply rejected by the oracle).
# ---------------------------------------------------------------------------
def _skip_dquote(s, i):
    n = len(s); i += 1
    while i < n:
        c = s[i]
        if c == "\\":
            i += 2; continue
        if c == "$" and i + 1 < n and s[i + 1] == "{":     # ${...} interpolation
            depth = 1; i += 2
            while i < n and depth > 0:
                if s[i] == '"':
                    i = _skip_dquote(s, i); continue
                if s[i] == "{": depth += 1
                elif s[i] == "}": depth -= 1
                i += 1
            continue
        if c == '"':
            return i + 1
        i += 1
    return n


def _match(s, i):
    """s[i] is an opener; return index of its matching closer, or -1."""
    pairs = {"(": ")", "[": "]", "{": "}"}
    stack = []; n = len(s)
    while i < n:
        c = s[i]
        if c == '"':
            i = _skip_dquote(s, i); continue
        if c in "([{":
            stack.append(pairs[c])
        elif c in ")]}":
            if not stack or stack.pop() != c:
                return -1
            if not stack:
                return i
        i += 1
    return -1


def _spans(s, openc):
    """All matched (start, end_inclusive) spans whose opener is `openc`."""
    out = []; i = 0; n = len(s)
    while i < n:
        c = s[i]
        if c == '"':
            i = _skip_dquote(s, i); continue
        if c == openc:
            j = _match(s, i)
            if j != -1:
                out.append((i, j))
        i += 1
    return out


def _string_spans(s):
    out = []; i = 0; n = len(s)
    while i < n:
        if s[i] == '"':
            j = _skip_dquote(s, i); out.append((i, j)); i = j; continue
        i += 1
    return out


def _tokenize_list(inner):
    """Split a list interior into whitespace-separated top-level elements."""
    elems = []; buf = ""; depth = 0; i = 0; n = len(inner)
    while i < n:
        c = inner[i]
        if c == '"':
            j = _skip_dquote(inner, i); buf += inner[i:j]; i = j; continue
        if c in "([{":
            depth += 1; buf += c; i += 1; continue
        if c in ")]}":
            depth -= 1; buf += c; i += 1; continue
        if c.isspace() and depth == 0:
            if buf.strip():
                elems.append(buf.strip())
            buf = ""; i += 1; continue
        buf += c; i += 1
    if buf.strip():
        elems.append(buf.strip())
    return elems


def _split_bindings(inner):
    """Split an attrs interior into `name = value` bindings on top-level ';'."""
    parts = []; buf = ""; depth = 0; i = 0; n = len(inner)
    while i < n:
        c = inner[i]
        if c == '"':
            j = _skip_dquote(inner, i); buf += inner[i:j]; i = j; continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == ";" and depth == 0:
            if buf.strip():
                parts.append(buf.strip())
            buf = ""; i += 1; continue
        buf += c; i += 1
    return parts


def _is_int(tok):
    return re.fullmatch(r"-?\d+", tok) is not None


def _complex(tok):
    return any(ch in tok for ch in "[]{}()") or len(tok) > 3


# ---------------------------------------------------------------------------
# Grammar-aware shrink candidate generators.
# ---------------------------------------------------------------------------
def _candidates(s):
    seen = set()

    def emit(cand):
        if cand and cand != s and cand not in seen:
            seen.add(cand); return cand
        return None

    # 1. List element reduction (keep-first tried FIRST — the strongest move).
    for (a, b) in _spans(s, "["):
        elems = _tokenize_list(s[a + 1:b])
        if len(elems) >= 2:
            c = emit(s[:a] + "[ " + elems[0] + " ]" + s[b + 1:])   # keep first
            if c: yield c
            c = emit(s[:a] + "[ " + elems[-1] + " ]" + s[b + 1:])  # keep last
            if c: yield c
            for k in range(len(elems)):                             # drop one
                rest = elems[:k] + elems[k + 1:]
                c = emit(s[:a] + "[ " + " ".join(rest) + " ]" + s[b + 1:])
                if c: yield c
        # element -> boundary leaf
        for k, el in enumerate(elems):
            if _complex(el):
                rest = elems[:]; rest[k] = "0"
                c = emit(s[:a] + "[ " + " ".join(rest) + " ]" + s[b + 1:])
                if c: yield c

    # 2. Attrs binding reduction (drop one binding at a time).
    for (a, b) in _spans(s, "{"):
        binds = _split_bindings(s[a + 1:b])
        if len(binds) >= 2:
            for k in range(len(binds)):
                rest = binds[:k] + binds[k + 1:]
                c = emit(s[:a] + "{ " + " ".join(x + ";" for x in rest) + " }" + s[b + 1:])
                if c: yield c

    # 3. Redundant-paren unwrap.
    for (a, b) in _spans(s, "("):
        c = emit(s[:a] + s[a + 1:b] + s[b + 1:])
        if c: yield c

    # 4. Number simplification (multi-digit magnitudes toward 0), skip in strings.
    sspans = _string_spans(s)
    def in_str(pos):
        return any(a <= pos < b for (a, b) in sspans)
    for m in re.finditer(r"\d{2,}", s):
        if not in_str(m.start()):
            c = emit(s[:m.start()] + "0" + s[m.end():])
            if c: yield c

    # 5. String simplification (non-empty literal -> "").
    for (a, b) in sspans:
        if b - a > 2:  # more than just the two quotes
            c = emit(s[:a] + '""' + s[b:])
            if c: yield c


def _cost(s):
    return len(re.sub(r"\s+", "", s))


def shrink(expr, target_sig, K=3, runner=run, timeout=10, verbose=True):
    """Greedy grammar-aware shrink to a fixpoint preserving `target_sig`.
       Returns (minimized_expr, steps, quarantined)."""
    cur = expr; curcost = _cost(cur); steps = []; quarantined = []
    improved = True
    while improved:
        improved = False
        for cand in _candidates(cur):
            if _cost(cand) >= curcost:
                continue
            sig, _ = signature(cand, runner, timeout)   # cheap pre-check (1 pass)
            if sig != target_sig:
                continue
            sig2, stable = stable_signature(cand, K, runner, timeout)
            if not stable or sig2 != target_sig:        # flaky => quarantine
                quarantined.append(cand); continue
            cur = cand; curcost = _cost(cand); steps.append(cand)
            improved = True
            if verbose:
                print(f"   accept  [{cand}]")
            break
    return cur, steps, quarantined


# ---------------------------------------------------------------------------
# Fixture emission — @@@ rows matching run-primop-edge-parity-tests.sh.
# ---------------------------------------------------------------------------
def classify_fixture(expr, details, seed=0, origin=""):
    """Turn a minimized finding into (kind, row_type, row_text, provenance)."""
    kind = details["kind"]
    oc, ov, oe = details["o"]; tc, tv, te = details["t"]
    prov = f"# fuzz-finding seed={seed} kind={kind} minimized-from: {origin}"
    if kind == "CLASS" and oc == "ok" and tc == "throw":
        # v3 SUCCEEDS where TW THROWS -> a v3 bug. Assert TW's error fragment
        # (v3 must be made to throw it too). This is the div-SIGFPE / catAttrs class.
        return kind, "NEG", f"{expr}@@@{terminal_fragment(te)}", prov
    if kind == "CLASS" and oc == "throw" and tc == "ok":
        # v3 OVER-throws where TW succeeds -> v3 bug. Assert TW's value.
        return kind, "POS", f"{expr}@@@{tv}", prov
    if kind == "VAL":
        # both succeed, values differ -> assert TW's value as the oracle.
        return kind, "POS", f"{expr}@@@{tv}", prov
    if kind in ("KIND", "SPLIT"):
        note = (f"# SOFT {kind}: v3 O=[{terminal_fragment(oe) or ov}] "
                f"T=[{terminal_fragment(te) or tv}] -- human triage before filing")
        return kind, "NOTE", f"# {expr}", prov + "\n" + note
    return kind, "NOTE", f"# {expr}  (kind={kind}, no divergence)", prov


def write_findings(path, rows):
    """rows: list of (row_type, row_text, provenance). Writes a DRAFT fixtures
       file grouped POS/NEG/NOTE in the committed suite's @@@ convention."""
    header = (
        "# fuzz-findings.txt -- DRAFT auto-emitted parity fixtures (WS-D framework)\n"
        "#\n"
        "# Row format matches src/libexpr-v3/test/run-primop-edge-parity-tests.sh:\n"
        "#   POS rows:  expr@@@expected-value    (P, O, and T must all produce it)\n"
        "#   NEG rows:  expr@@@error-fragment    (P, O, and T stderr must contain it)\n"
        "# A provenance comment precedes each row. This is a DRAFT: an un-triaged\n"
        "# fuzzer finding gates nothing (Rule 0). Graduate by hand into REPROS.md +\n"
        "# the relevant committed run-*-parity script after confirming the fix.\n"
        "#\n"
        "# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,\n"
        "#   Input Output Group.  SPDX-License-Identifier: Apache-2.0\n")
    blocks = {"POS": [], "NEG": [], "NOTE": []}
    for rtype, rtext, prov in rows:
        blocks[rtype].append(prov + "\n" + rtext)
    out = [header]
    if blocks["POS"]:
        out.append("\n# ---- POSITIVE (VAL / v3-over-throws; oracle = TW value) ----")
        out.extend(blocks["POS"])
    if blocks["NEG"]:
        out.append("\n# ---- NEGATIVE (CLASS: v3 succeeds, TW throws; v3 bug) ----")
        out.extend(blocks["NEG"])
    if blocks["NOTE"]:
        out.append("\n# ---- NOTES (SOFT KIND / internal SPLIT; triage first) ----")
        out.extend(blocks["NOTE"])
    text = "\n".join(out) + "\n"
    with open(path, "w") as f:
        f.write(text)
    return text


# ---------------------------------------------------------------------------
# Synthetic injected divergence — a pluggable mock engine used ONLY to
# demonstrate the shrink loop preserves an actual VAL divergence (the live
# zipAttrsWith bug is already fixed). Models a plausible v3 bug: "builtins.length
# over-counts by 1 when the list is non-empty and its first element is an int."
# ---------------------------------------------------------------------------
def mock_run(mode, expr, timeout=10):
    m = re.match(r"\s*builtins\.length\s*(\[.*\])\s*$", expr)
    if not m:
        return (1, "", "v3-eval error: mock: unsupported shape")
    lst = m.group(1)
    end = _match(lst, 0)
    if end == -1:
        return (1, "", "v3-eval error: mock: unbalanced list")
    elems = _tokenize_list(lst[1:end])
    n = len(elems)
    if mode == "T":                                   # tree-walker = correct
        return (0, str(n), "")
    buggy = n + 1 if (n > 0 and _is_int(elems[0])) else n   # v3 P and O paths
    return (0, str(buggy), "")


# ---------------------------------------------------------------------------
# Demo.
# ---------------------------------------------------------------------------
def _demo():
    print("=" * 78)
    print("DEMO A -- REAL engines: shrink the zipAttrsWith family (signature-preserving)")
    print("=" * 78)
    seedA = "builtins.zipAttrsWith (n: vs: vs) [ 1 2 3 {a=1;} ]"
    print(f"seed   [{seedA}]")
    tgtA, stableA = stable_signature(seedA, K=3)
    print(f"target signature = {tgtA}")
    print(f"stability gate (K=3) = {'STABLE' if stableA else 'FLAKY -> quarantine'}")
    if stableA:
        minA, stepsA, qA = shrink(seedA, tgtA, K=3)
        print(f"minimized -> [{minA}]  ({len(stepsA)} accepted move(s), "
              f"{len(qA)} quarantined)")
        print(f"note: kind={tgtA[0]} (the bug is FIXED -> all three throw the same"
              " kind), so no fixture is filed; the shrink loop still holds the"
              " exact signature (same class-pattern + same normalized error-kind).")

    print()
    print("=" * 78)
    print("DEMO B -- SYNTHETIC injected VAL divergence (mock engine): shrink + emit")
    print("=" * 78)
    seedB = "builtins.length [ 3 [1] {a=1;} 4 5 ]"
    print(f"seed   [{seedB}]")
    tgtB, stableB = stable_signature(seedB, K=3, runner=mock_run)
    detB0 = signature(seedB, runner=mock_run)[1]
    print(f"target signature = {tgtB}   (O={detB0['o'][1]!r} vs T={detB0['t'][1]!r})")
    print(f"stability gate (K=3) = {'STABLE' if stableB else 'FLAKY -> quarantine'}")
    minB, stepsB, qB = shrink(seedB, tgtB, K=3, runner=mock_run)
    print(f"minimized -> [{minB}]  ({len(stepsB)} accepted move(s))")
    detB = signature(minB, runner=mock_run)[1]
    kindB, rtypeB, rowB, provB = classify_fixture(minB, detB, seed=0, origin=seedB)
    print(f"emit -> {rtypeB} row: {rowB}")

    # A NEG archetype from the historical zipAttrsWith bug (pre-fix: v3 returned
    # {} where TW threw) to demonstrate the CLASS -> NEG classification branch.
    hist_details = {"kind": "CLASS",
                    "o": ("ok", "{ }", ""),
                    "t": ("throw", "", "error: expected a set but found an integer: 1")}
    kH, rtH, rowH, provH = classify_fixture(
        "builtins.zipAttrsWith (n: vs: vs) [1]", hist_details, seed=0,
        origin="builtins.zipAttrsWith (n: vs: vs) [ 1 2 3 {a=1;} ]  (HISTORICAL, now fixed)")
    print(f"emit -> {rtH} row: {rowH}   (historical CLASS archetype)")

    out_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "fuzz-findings.txt")
    text = write_findings(out_path, [(rtypeB, rowB, provB), (rtH, rowH, provH)])
    print(f"\nwrote {out_path}:\n")
    print(text)


if __name__ == "__main__":
    _demo()
