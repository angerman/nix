# LEVER-1 #1.1 impurity fixture: an import-result whose BODY reads an impure
# builtin (currentTime).  currentTime is FIXED per process, so applying this
# twice in one process must give the SAME value whether or not the applied
# cache serves the second application from memo — the cache must never
# INTRODUCE a difference (nor, in a daemon chaining evals in one process,
# serve a value from a different instant: currentTime is process-constant).
# Returns currentTime directly so any cache-vs-nocache divergence is visible.
{ ... } @ args: builtins.currentTime
