# LEVER-1 applied-cache fixture: arg-dependent work that the optimizer
# cannot const-fold (n depends on the runtime argument), heavy enough that
# the repeated-eval insns collapse is unambiguous (~2M insns per eval).
{ ... } @ args:
let n = 2000 + builtins.length (builtins.attrNames args);
in builtins.foldl' (a: b: a + b) 0 (builtins.genList (i: i) n)
