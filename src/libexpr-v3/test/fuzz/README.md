# WS-D differential fuzzer (v3-vs-TW)

Coverage-directed differential fuzzer + minimizer for the v3 bytecode evaluator.
Three-way oracle: **P** = `v3-eval` (primop path), **O** = `nix eval` + `NIX_V3_DIRECT_EVAL=1`
(opcode path), **T** = `nix eval` (tree-walker oracle). `P!=O` = a v3 internal opcode/primop
split; `(P==O)!=T` = a v3-vs-TW divergence.

- `fuzz_directed.py N SEED` — type-directed generator over the ~185 uncovered/edge primops
  (steered by an `attrNames builtins` surface diff) + boundary values; 3-tier error oracle
  (class-parity throw-vs-succeed = zero-false-positive primary; normalized error-KIND = soft;
  value byte-id). Prints the divergence yield.
- `fuzz_framework.py` — signature-preserving grammar-aware **minimizer** (shrinks a failing
  expr to a minimal repro, K-run stability gate) + **fixture-emit** into the `@@@` row format
  of `../run-primop-edge-parity-tests.sh`.

Found (and got fixed) the `zipAttrsWith` non-attrset fail-open bug (2026-08-09). A raw fuzzer
finding gates nothing (Rule 0) — triage + minimize, then graduate by hand into a committed
`run-*-parity-tests.sh` suite + `REPROS.md`.

NOT YET BUILT (needs nej + push OK): the live `NIX_V3_PARITY_SHADOW` production monitor
(sampled shadow-compare of v3 vs TW drvPath in nej `worker.cc`).

Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
SPDX-License-Identifier: Apache-2.0
