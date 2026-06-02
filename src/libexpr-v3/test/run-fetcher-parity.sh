#!/usr/bin/env bash
# Fetcher parity — TW vs v3-direct byte-equality guard for the v3-native
# fetcher path (TW_VALUE_ERADICATION F1/F2: primFetchTree / primFetchGit now
# extract args v3-native + call ffi::fetchTree (libfetchers directly) +
# build the result via v3EmitTreeAttrs — NO v3ToTreeWalker / callFunction
# round-trip).
#
# The fetcher result attrs (outPath/rev/shortRev/revCount/lastModified) feed
# downstream drvPaths, so any divergence from TW is a store-path bug.  This
# pins byte-equality on the offline-reproducible git fetcher.
#
# Usage:  NIX=/path/to/nix ./run-fetcher-parity.sh
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0

set -u

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"

if [[ ! -x "$NIX" ]]; then
  echo "fetcher-parity: nix not found at $NIX" >&2
  exit 1
fi
if ! command -v git >/dev/null 2>&1; then
  echo "fetcher-parity: SKIP (git not available)"
  exit 0
fi

EXF=(--extra-experimental-features "nix-command flakes")
pass=0
fail=0
failed=()

check() {  # label expr
  local label="$1" expr="$2" tw v3
  tw=$("$NIX" "${EXF[@]}" eval --impure --json --expr "$expr" 2>/dev/null)
  v3=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s \
        "$NIX" "${EXF[@]}" eval --impure --json --expr "$expr" 2>/dev/null)
  if [[ -n "$tw" && "$tw" == "$v3" ]]; then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    failed+=("$label (TW=$tw v3=$v3)")
  fi
}

# --- builtins.fetchGit on a local clean repo (attrset + URL-string forms) ---
G=$(mktemp -d)
(
  cd "$G" || exit 1
  git init -q -b main
  git config user.email parity@example.com
  git config user.name parity
  printf 'content\n' > file.txt
  git add file.txt
  git commit -qm init
)

# attrset form: { url = ...; }
check "fetchGit-attrs" \
  "let r = builtins.fetchGit { url = \"$G\"; }; in { inherit (r) rev shortRev revCount lastModified; op = r.outPath; sa = builtins.attrNames r; }"

# bare URL-string form
check "fetchGit-url" \
  "let r = builtins.fetchGit \"$G\"; in { inherit (r) rev shortRev; op = r.outPath; }"

# explicit ref
check "fetchGit-ref" \
  "(builtins.fetchGit { url = \"$G\"; ref = \"main\"; }).rev"

rm -rf "$G"

# --- builtins.fetchTree {type=path} (the non-git fromAttrs + fromURL paths) ---
# realpath the dir: some platforms symlink the mktemp root (e.g. macOS
# /tmp -> /private/tmp), which fetchTree rejects ("path is a symlink").
P=$(cd "$(mktemp -d)" && pwd -P)
printf 'tree\n' > "$P/data.txt"
check "fetchTree-path-attrs" \
  "let r = builtins.fetchTree { type = \"path\"; path = \"$P\"; }; in { op = r.outPath; nh = r.narHash; sa = builtins.attrNames r; }"
check "fetchTree-path-url" \
  "(builtins.fetchTree \"path:$P\").outPath"
rm -rf "$P"

# --- builtins.fetchurl / fetchTarball via file:// (the fetch() family) ---
F=$(cd "$(mktemp -d)" && pwd -P)
printf 'urlcontent\n' > "$F/x.txt"
check "fetchurl-string" \
  "builtins.fetchurl \"file://$F/x.txt\""
check "fetchurl-attrs-name" \
  "builtins.fetchurl { url = \"file://$F/x.txt\"; name = \"renamed\"; }"
mkdir -p "$F/src"; printf 'tree\n' > "$F/src/f"
if command -v tar >/dev/null 2>&1 && tar -czf "$F/t.tar.gz" -C "$F" src 2>/dev/null; then
  check "fetchTarball-string" \
    "builtins.fetchTarball \"file://$F/t.tar.gz\""
fi
rm -rf "$F"

# --- builtins.fetchMercurial on a local hg repo (if hg is available) ---
if command -v hg >/dev/null 2>&1; then
  M=$(cd "$(mktemp -d)" && pwd -P)
  (
    cd "$M" && hg init -q
    printf 'hgcontent\n' > f.txt
    hg add f.txt >/dev/null 2>&1
    HGUSER="parity <parity@example.com>" hg commit -qm init
  )
  check "fetchMercurial-attrs" \
    "let r = builtins.fetchMercurial { url = \"$M\"; }; in { op = r.outPath; inherit (r) rev shortRev branch; sa = builtins.attrNames r; }"
  rm -rf "$M"
fi

# --- builtins.fetchClosure (content-addressed, via a local file:// cache) ---
# Self-contained: add a CA path, push its closure to a file:// cache, then
# fetchClosure it back.  Needs the fetch-closure feature + _NIX_IN_TEST=1
# (file:// fromStore is gated on that).
C=$(cd "$(mktemp -d)" && pwd -P)
mkdir -p "$C/src"; printf 'closure\n' > "$C/src/f.txt"
SP=$("$NIX" "${EXF[@]}" store add-path "$C/src" --name fc-parity 2>/dev/null || true)
if [[ -n "$SP" ]] && "$NIX" "${EXF[@]}" copy --to "file://$C/cache" "$SP" >/dev/null 2>&1; then
  FCQ="builtins.fetchClosure { fromStore = \"file://$C/cache\"; fromPath = \"$SP\"; }"
  fctw=$(_NIX_IN_TEST=1 "$NIX" "${EXF[@]}" --extra-experimental-features fetch-closure eval --impure --raw --expr "$FCQ" 2>/dev/null)
  fcv3=$(_NIX_IN_TEST=1 NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_WALL_TIME=60s "$NIX" "${EXF[@]}" --extra-experimental-features fetch-closure eval --impure --raw --expr "$FCQ" 2>/dev/null)
  if [[ -n "$fctw" && "$fctw" == "$fcv3" ]]; then pass=$((pass+1)); else fail=$((fail+1)); failed+=("fetchClosure-ca (TW=$fctw v3=$fcv3)"); fi
fi
rm -rf "$C"

# --- builtins.filterSource (F3: filter closure re-enters v3's VM per entry) ---
S=$(cd "$(mktemp -d)" && pwd -P)
printf 'a\n' > "$S/keep.txt"; printf 'b\n' > "$S/drop.log"; mkdir "$S/sub"; printf 'c\n' > "$S/sub/x"
check "filterSource-filtered" \
  "builtins.filterSource (path: type: (type == \"directory\") || (builtins.match \".*\\\\.txt\" (baseNameOf path) != null)) $S"
check "filterSource-keep-all" \
  "builtins.filterSource (path: type: true) $S"
rm -rf "$S"

# --- builtins.path { filter } (F3 keystone: native filtered path-copy) ---
# THIS is the cardano-node hot path: haskell.nix copies source trees with
# cleanSourceWith-style `path: type:` filters via builtins.path.  Pre-fix,
# v3 bridged the filter closure into TW (v3ToTreeWalker) and TW's dumpPath
# NAR walk dispatched it once per fs entry — 20034 __v3_call_bridge_1 /
# 10033 bridged closures on cardano-node (see TW_VALUE_ERADICATION_GOAL).
# primPathFilteredNative now drives fetchToStore directly with a per-entry
# callClosure callback.  Byte-equality pins that the native filtered copy
# yields the same content-addressed store path as TW's bridged builtins.path.
BP=$(cd "$(mktemp -d)" && pwd -P)
printf 'a\n' > "$BP/keep.txt"; printf 'b\n' > "$BP/drop.log"
mkdir "$BP/sub"; printf 'c\n' > "$BP/sub/x.txt"; printf 'd\n' > "$BP/sub/y.log"
check "path-filter-txt-only" \
  "builtins.path { path = \"$BP\"; name = \"src\"; filter = (p: t: (t == \"directory\") || (builtins.match \".*\\\\.txt\" (baseNameOf p) != null)); }"
check "path-filter-keep-all" \
  "builtins.path { path = \"$BP\"; filter = (p: t: true); }"
check "path-filter-nonrecursive-file" \
  "builtins.path { path = \"$BP/keep.txt\"; recursive = false; filter = (p: t: true); }"
rm -rf "$BP"

echo "fetcher-parity: $pass passed, $fail failed"
if (( fail > 0 )); then
  printf '  FAIL: %s\n' "${failed[@]}" >&2
  exit 1
fi
exit 0
