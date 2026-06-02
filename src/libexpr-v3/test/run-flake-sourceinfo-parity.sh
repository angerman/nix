#!/usr/bin/env bash
# Flake sourceInfo parity — TW vs v3-direct byte-equality guard for the
# v3-native callFlake marshaller (v3_call_flake.cc + ffi::readLockedFlake).
#
# Why this exists: FFI_CONSOLIDATION_AUDIT Phase 4 moved the libflake /
# libfetchers READING out of v3_call_flake.cc and behind
# `ffi::readLockedFlake` (ffi.cc), feeding `v3EmitTreeAttrs` plain data
# (ffi::TreeAttrsInfo).  The per-node `sourceInfo` attrs (outPath, narHash,
# rev/shortRev/revCount, submodules, lastModified/lastModifiedDate) feed
# downstream drvPaths, so ANY divergence from TW is a store-path bug.  This
# test pins byte-equality across the non-git and git code paths.
#
# Covered fields:
#   non-git flake: outPath, narHash, lastModified, lastModifiedDate, attrNames
#   git flake:     outPath, rev, shortRev, revCount, submodules (via attrNames)
#
# Usage:
#   ./run-flake-sourceinfo-parity.sh           # summary
#   NIX=/path/to/nix ./run-flake-sourceinfo-parity.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "flake-sourceinfo-parity: nix not found at $NIX" >&2
  exit 1
fi
if ! command -v git >/dev/null 2>&1; then
  echo "flake-sourceinfo-parity: SKIP (git not available)"
  exit 0
fi

EXF=(--extra-experimental-features "nix-command flakes")

pass=0
fail=0
failed=()

# Compare one flake-output attr under TW vs v3-direct; assert byte-equal
# AND non-empty (empty would mean both errored — a false pass).
check() {  # url attr
  local url="$1" attr="$2" tw v3
  tw=$("$NIX" "${EXF[@]}" eval --json "$url#$attr" 2>/dev/null)
  v3=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=30s \
        "$NIX" "${EXF[@]}" eval --json "$url#$attr" 2>/dev/null)
  if [[ -n "$tw" && "$tw" == "$v3" ]]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed+=("$attr (TW=$tw v3=$v3)")
  fi
}

# --- non-git flake (path: source) -----------------------------------------
T1=$(mktemp -d)
cat > "$T1/flake.nix" <<'EOF'
{
  outputs = { self }: {
    answer = 42;
    op  = self.outPath;
    nh  = self.sourceInfo.narHash;
    lm  = self.lastModified or "NONE";
    lmd = self.lastModifiedDate or "NONE";
    si  = builtins.attrNames self.sourceInfo;
  };
}
EOF
for a in answer op nh lm lmd si; do check "$T1" "$a"; done
rm -rf "$T1"

# --- git flake (rev / shortRev / revCount / submodules) -------------------
T2=$(mktemp -d)
(
  cd "$T2" || exit 1
  git init -q -b main
  git config user.email parity@example.com
  git config user.name parity
  cat > flake.nix <<'EOF'
{
  outputs = { self }: {
    op  = self.outPath;
    rv  = self.rev or "NONE";
    srv = self.shortRev or "NONE";
    rc  = self.revCount or (-1);
    si  = builtins.attrNames self.sourceInfo;
  };
}
EOF
  git add flake.nix
  git commit -qm init
)
for a in op rv srv rc si; do check "git+file://$T2" "$a"; done
rm -rf "$T2"

echo "flake-sourceinfo-parity: $pass passed, $fail failed"
if (( fail > 0 )); then
  printf '  FAIL: %s\n' "${failed[@]}" >&2
  exit 1
fi
exit 0
