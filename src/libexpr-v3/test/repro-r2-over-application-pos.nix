# R2 over-application regression (2026-06-12).
#
# Bug (FIXED): an arity-2 `mapAttrs` / `zipAttrsWith` callback whose body is a
# NON-lambda expression evaluating to a FUNCTION is packed into a Tag::App3 PAP
# (`App3(callback, name, value)` — the only two App3 builders).  That PAP is
# already saturated for the arity-2 callback (depth 2 == arity 2).  When it
# reaches a call site while STILL UNFORCED and is applied to a further argument
# — e.g. `map (g: g x) (attrValues (mapAttrs …))`, where `map`'s callback
# tail-calls the list element before anything forces it — the VM walked the
# App/App3 spine, found papDepth+1 (=3) > arity (=2), and threw
# `v3 OP_TAIL_CALL: PAP over-applied` (twin: `v3 OP_CALL: PAP over-applied`).
#
# This is *legitimate* over-application: the tree-walker applies one arg at a
# time, so the leaf saturates at arity 2, yields the inner function, and that
# function consumes the remaining arg(s).  v3 now does the same (saturate the
# leaf, then re-apply the remaining args via callClosure) instead of throwing.
#
# The cardano-node v3-direct symptom was `attrNames .packages.aarch64-darwin`
# → `OP_TAIL_CALL: PAP over-applied` (leaf='n' arity=2 papDepth=2 spine=[App3
# Closure]).  See lode RCA + memory project_codebase_review_impl_2026-06-12.
let
  # arity-2 callback (`n: v: <expr>`, body is `head [...]` — collapse stops at
  # 2) whose value is a 1-arg function → applying a 3rd arg over-applies by 1.
  m1 = builtins.mapAttrs (n: v: builtins.head [ (z: z) ]) { a = 1; b = 2; };
  # body returns a 2-arg function → over-applies by 2 (exercises the
  # multi-remaining-arg callClosure replay loop).
  m2 = builtins.mapAttrs (n: v: builtins.head [ (x: y: x + y) ]) { a = 1; };
in
[ (builtins.map (g: g 9) (builtins.attrValues m1))               # tail-call over-apply ×1
  (builtins.map (g: g 10 20) (builtins.attrValues m2))           # over-apply ×2
  (builtins.typeOf ((builtins.head (builtins.attrValues m1)) 42)) # non-tail over-apply (OP_CALL twin)
]
