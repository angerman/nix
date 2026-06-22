#!/usr/bin/env bash
# bench/decompose-other.sh — break down the OTHER on-CPU bucket from
# profile-at-scale.sh.  Applies the SAME category patterns as the canonical
# categorizer, but prints the top self-time symbols that fall into OTHER (i.e.
# everything not already attributed to ALLOC/BINDINGS/PARSE/DISPATCH/LOWER/HASH/
# STRING/TLS/GC/FORCE/CALL/POS/WAIT).  Answers "is the ~16% OTHER a hidden
# bounded lever, or irreducibly diffuse?".
#
# MUST run on a QUIET host (darwin-4).  One-off investigation tool.
#
# Usage:  bench/decompose-other.sh [firefox|M5|HNE]    (default firefox)
#
# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
# Input Output Group.  SPDX-License-Identifier: Apache-2.0
set -u
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
NIX="${NIX:-$ROOT/build/src/nix/nix}"
source "$ROOT/src/libexpr-v3/test/nixpkgs-pin.sh"
CN_PATH="${CN_PATH:-/Users/angerman/Projects/iohk/cardano-node}"
HNE_PATH="${HNE_PATH:-/Users/angerman/Projects/iohk/haskell-nix-example}"
W="${1:-firefox}"
DUR="${DUR:-8}"; [ "$W" = M5 ] && DUR="${DUR:-13}"

V3="NIX_V3_DIRECT_EVAL=1 NIX_V3_NO_DISK_CACHE=1"
V3F="$V3 NIX_V3_NO_NATIVE_CALL_FLAKE=1"
case "$W" in
  firefox) ENV_W="$V3";  EXPR='(import <nixpkgs> { config.allowUnfree = true; }).firefox.drvPath' ;;
  M5)  ENV_W="$V3F"; EXPR="(builtins.getFlake \"path:$CN_PATH\").outputs.packages.aarch64-darwin.cardano-node.name" ;;
  HNE) ENV_W="$V3F"; EXPR="(builtins.getFlake \"path:$HNE_PATH\").packages.aarch64-darwin.hello.drvPath" ;;
  *) echo "unknown workload $W"; exit 2 ;;
esac

SAMP="$(mktemp -t other.XXXX).sample"
env $ENV_W "$NIX" eval --impure --option allow-import-from-derivation true \
  --raw --expr "$EXPR" >/dev/null 2>&1 &
pid=$!
sample "$pid" "$DUR" -file "$SAMP" >/dev/null 2>&1
wait "$pid" 2>/dev/null

echo "==== OTHER-bucket decomposition: $W (dur ${DUR}s, $(hostname -s)) ===="
awk '
/Sort by top of stack/{f=1; next}
/Binary Images|^Total number/{f=0}
f && /\(in / {
  n=$NF+0
  line=$0; sub(/[[:space:]]*\(in .*/,"",line); gsub(/^[[:space:]]+/,"",line); sym=line
  sub(/^DYLD-STUB\$\$/,"",sym)
  cat="OTHER"
  if (sym ~ /__psynch|__sigwait|__workq|kevent|^poll$|mach_msg|__getdirentries|^__open$|^read$|^write$|semaphore|__commpage/) cat="WAIT"
  else if (sym ~ /yylex|Parser::parse|ParserState|addAttrLeaf|parser::|ParserLoc/) cat="PARSE"
  else if (sym ~ /globalInternSymbol|inlineTrivial|nix::v3::ir::|optimi|opt_|::lower|Module&|emit[A-Z]|Emitter|Lowerer/) cat="LOWER"
  else if (sym ~ /dispatchLoop/) cat="DISPATCH"
  else if (sym ~ /forceValue/) cat="FORCE"
  else if (sym ~ /callClosure/) cat="CALL"
  else if (sym ~ /mergeBindings|countDistinct|Bindings::|allocBindings|Cursor/) cat="BINDINGS"
  else if (sym ~ /tryMark|visitValue|Nursery|RootVisitor|[Ss]cavenge|MarkSweep|MarkVisitor|Marker|markRoots|walkV3|walkAll/) cat="GC"
  else if (sym ~ /strlen|printString|StringContext|stringContext|^memcmp$|basic_string|coerceToString/) cat="STRING"
  else if (sym ~ /sha256|blake|murmur|cityhash|hash_table|__emplace|__hash/) cat="HASH"
  else if (sym ~ /malloc|free|xzm|_platform_mem|operator new|operator delete|nanov2|tiny_|bzero|alloc[A-Z]|Arena::alloc|traceable_allocator|::__append/) cat="ALLOC"
  else if (sym ~ /PosTable|PosIdx|LinesIterator/) cat="POS"
  else if (sym ~ /_tlv_get_addr|tlv/) cat="TLS"
  if(cat!="WAIT") oncpu+=n
  if(cat=="OTHER"){ other[sym]+=n; otot+=n }
}
END{
  if(oncpu==0){print "  (no on-CPU samples)"; exit}
  printf "  OTHER total = %d samples = %.1f%% of on-CPU (oncpu=%d)\n\n", otot, 100.0*otot/oncpu, oncpu
  printf "  %8s  %6s  %s\n","samples","%onCPU","symbol"
  n=0
  for(s in other) arr[++n]=other[s] SUBSEP s
  # simple selection sort for the top 30 (n is small)
  for(i=1;i<=n && i<=30;i++){ mx=i; for(j=i+1;j<=n;j++){split(arr[j],b,SUBSEP);split(arr[mx],c,SUBSEP); if(b[1]+0>c[1]+0) mx=j} t=arr[i];arr[i]=arr[mx];arr[mx]=t
    split(arr[i],p,SUBSEP); printf "  %8d  %5.1f%%  %s\n", p[1]+0, 100.0*(p[1]+0)/oncpu, p[2] }
}
' "$SAMP"
rm -f "$SAMP"
