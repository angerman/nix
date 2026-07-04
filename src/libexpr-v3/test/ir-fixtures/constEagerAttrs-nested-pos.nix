# RUN: v3-eval --file %s --emit-ir | v3-check %s
#
# LEVER-1 step 2b (task #17): a recursively-constant nested attrset literal
# lowers EAGERLY — the inner `{ allowUnfree = true; }` is built inline (no
# MkThunk wrapper), so at runtime the whole arg is WHNF and the applied-import
# cache can canonical-hash it.  Pre-#17 the inner attrset value was
# `MkThunk f1` (verified 2026-07-04) and `import <nixpkgs> { config = { ... }; }`
# was uncacheable.  Safe: pushing const leaves evaluates nothing (no throw /
# divergence / order change is possible in a literal), it only allocates.

{ config = { allowUnfree = true; }; }

# CHECK-LABEL: B1:
# CHECK: LitBool true
# CHECK: AttrSet {"allowUnfree"=v{{[0-9]+}}}
# CHECK: AttrSet {"config"=v{{[0-9]+}}}
# CHECK-NOT: MkThunk
