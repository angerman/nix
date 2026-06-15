#!/usr/bin/env bash
# v3 nursery+gen-major FLIP soak — byte-equality across a broad nixpkgs slice.
#
# Shipped 2026-06-15 with the default-on flip (commit e863f127d). This is the
# confidence instrument for the flip and for the eventual RETIREMENT of the
# NIX_V3_NURSERY opt-out gates: run it on darwin-4 with a large PKGS set; once a
# full-nixpkgs run is clean, the opt-out can be removed.
#
# For each package it compares three evaluations of `<pkg>.drvPath`:
#   v3-flip      = NIX_V3_DIRECT_EVAL=1, DEFAULT config (nursery+gen-major ON)
#   v3-majordft  = NIX_V3_DIRECT_EVAL=1 NIX_V3_NURSERY=0 (old major-GC default)
#   TW           = nix eval (the oracle)
#
# PRIMARY gate = flip-transparency: v3-flip MUST equal v3-majordft for every
# package (a GC change must not alter observable output).  AUDIT must be 0 under
# the flip.  tw-diverge (v3 != TW) is reported but is NOT a flip failure — it is
# a pre-existing v3 residual (tracked separately; see RCA_DRVPATH_SYSTEMIC_*).
#
# Usage:
#   bench/flip-soak.sh            # default ~57-pkg slice
#   PKGS="hello git curl" bench/flip-soak.sh   # custom slice
#   NIX=/path/to/nix bench/flip-soak.sh        # custom nix binary
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output
# Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
HEAP="${NIX_V3_MAX_HEAP:-4G}"

if [[ ! -x "$NIX" ]]; then echo "flip-soak: $NIX not found, build first" >&2; exit 2; fi

# Default slice: diverse leaf + library packages exercising many primop /
# construction paths.  Override with PKGS="..." for a darwin-4 full sweep.
read -r -a PKGS <<< "${PKGS:-hello git coreutils bash gnumake gnugrep gnused gawk \
zlib ncurses openssl curl python3 ncdu jq ripgrep wget htop tmux file findutils \
diffutils patch gzip bzip2 xz zstd lz4 gnutar cpio readline pcre pcre2 libxml2 \
libxslt sqlite libffi gmp mpfr libpng libjpeg freetype fontconfig cmake ninja \
meson pkg-config autoconf automake libtool bison flex m4 rsync openssh gnupg \
less which gettext}"

pass=0; flip_fail=0; audit_fail=0; eval_fail=0; tw_div=0
flip_names=""; audit_names=""; eval_names=""; tw_names=""
for p in "${PKGS[@]}"; do
  expr="(import <nixpkgs> {}).$p.drvPath"
  fl=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_HEAP=$HEAP "$NIX" eval --impure --raw --expr "$expr" 2>/dev/null)
  if [ -z "$fl" ]; then printf "  EVAL-FAIL    %-14s\n" "$p"; eval_fail=$((eval_fail+1)); eval_names="$eval_names $p"; continue; fi
  md=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_HEAP=$HEAP NIX_V3_NURSERY=0 "$NIX" eval --impure --raw --expr "$expr" 2>/dev/null)
  if [ "$fl" != "$md" ]; then printf "  FLIP-DIVERGE %-14s flip=%s majordft=%s\n" "$p" "${fl##*/}" "${md##*/}"; flip_fail=$((flip_fail+1)); flip_names="$flip_names $p"; continue; fi
  au=$(NIX_V3_DIRECT_EVAL=1 NIX_V3_MAX_HEAP=$HEAP V3_DBG_NURSERY_AUDIT=1 "$NIX" eval --impure --raw --expr "$expr" 2>&1 >/dev/null | grep -c 'reachable via')
  if [ "$au" != "0" ]; then printf "  AUDIT-HIT    %-14s hits=%s\n" "$p" "$au"; audit_fail=$((audit_fail+1)); audit_names="$audit_names $p"; continue; fi
  tw=$("$NIX" eval --impure --raw --expr "$expr" 2>/dev/null)
  if [ "$fl" != "$tw" ]; then tw_div=$((tw_div+1)); tw_names="$tw_names $p"; fi
  pass=$((pass+1))
done
echo "==== flip-soak: ok=$pass  flip-diverge=$flip_fail  audit-hit=$audit_fail  eval-fail=$eval_fail (n=${#PKGS[@]}) ===="
[ -n "$flip_names" ]  && echo "  flip-diverge:$flip_names  <-- HARD FAIL (flip altered output)"
[ -n "$audit_names" ] && echo "  audit-hit:$audit_names    <-- HARD FAIL (missed root under flip)"
[ -n "$eval_names" ]  && echo "  eval-fail:$eval_names"
echo "==== tw-only-diverge=$tw_div (pre-existing v3 residuals, NOT flip-caused):$tw_names ===="
# Exit non-zero only on a flip-caused failure (the soak's contract).
if (( flip_fail > 0 || audit_fail > 0 )); then exit 1; fi
exit 0
