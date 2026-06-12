# R2 negative regression (2026-06-12): the fix must NOT change behaviour of the
# NON-over-applied App3 shapes that already worked.
#
#  * a saturated arity-2 mapAttrs callback (body NOT a function) → the App3
#    forces to its plain result, no over-application.
#  * an UNDER-applied App3 (arity-3 callback packed with 2 args) stays a PAP
#    until the 3rd arg saturates it — must NOT be treated as over-applied.
#  * a normal saturated curried call is unaffected.
let
  saturated  = builtins.mapAttrs (n: v: "${n}=${toString v}") { a = 1; };       # plain string result
  underApp3  = builtins.mapAttrs (n: v: extra: "${n}-${toString v}-${extra}") { a = 1; }; # arity-3
in
[ saturated.a                                   # "a=1"  (App3 saturates to a string)
  (underApp3.a "X")                             # "a-1-X" (App3 under-applied, 3rd arg saturates)
  ((x: y: x + y) 3 4)                           # 7      (ordinary saturated call)
]
