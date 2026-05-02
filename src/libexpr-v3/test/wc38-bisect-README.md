# WC-38 Bisect Harness

Used during WC-38 investigation to bisect a writable copy of nixpkgs
to isolate the dynamic-attr-value laziness bug (commit c95be6461).

## How it was used

1. Copy a failing nixpkgs revision to a writable location:
   ```
   cp -r /nix/store/<HASH>-source /tmp/nixpkgs-bisect
   chmod -R u+w /tmp/nixpkgs-bisect
   ```

2. Run the harness to verify the bug reproduces:
   ```
   ./src/libexpr-v3/test/wc38-bisect-harness.sh
   # → STATUS: BUG-REPRODUCED
   ```

3. Progressively simplify files in `/tmp/nixpkgs-bisect/`, re-running
   the harness after each change to test if the bug still reproduces.
   Keep the simplification if the bug remains; revert if it
   disappears.

4. Eventually arrive at a minimal Nix expression in
   `/tmp/nixpkgs-bisect/` that triggers the bug.  This pinpoints the
   feature in nixpkgs that exercises the v3 bug.

## What was found

Reduced from full nixpkgs (~13k+ lines under `pkgs/top-level/`) to:
- `pkgs/stdenv/darwin/default.nix` stages 0+1 only (~520 lines)
- → just one entry: `"llvmPackages_${llvmVersion}" = overrideLlvmPackagesScope ...`
- → dynamic-LHS attr key + a function call value triggered the bug.

This led to the v3 `lower.cc:1084` fix (use `thunkifyForAttr` instead
of `lowerExpr` for non-rec attrset's dynamic-key VALUE expressions).

## Current behaviour

After the fix, the harness reports the WC-38 OP_WITH_LOOKUP error is
GONE.  Subsequent failures are unrelated categories
(`Too many root sets` Boehm GC, then `primOp->isPrimOp()` tree-walker
bridge assertion) — see Phase 12 / Phase 13 in
`memory/project_wc38_with_blackhole.md`.

## Custom expressions

Set EXPR='...' to test any expression.  Set NIXPKGS=/path to point
at a different copy.
