#!/usr/bin/env python3
# Tier-D Phase-1 v2: coverage-DIRECTED differential fuzzer over the UNCOVERED/edge primop
# surface, with the 3-tier error oracle. 3-way: P=v3-eval(primop) O=nix+DIRECT_EVAL(opcode)
# T=nix(TW).  P!=O = v3 internal split; (P==O)!=T = v3-vs-TW divergence.
# Error tiers: (1) CLASS parity throw-vs-succeed (zero-FP, hard); (2) normalized-KIND parity
# (strip positions/ansi/store-hashes -> core phrase; soft); value byte-id for successes.
import subprocess, random, sys, os, re

ROOT="/Users/angerman/Projects/iohk/nix"
V3=f"{ROOT}/build/src/libexpr-v3/v3-eval"; NIX=f"{ROOT}/build/src/nix/nix"
N=int(sys.argv[1]) if len(sys.argv)>1 else 400
SEED=int(sys.argv[2]) if len(sys.argv)>2 else 1

def run(mode, expr, timeout=10):
    env=dict(os.environ); env["NIX_V3_MAX_WALL_TIME"]="8s"; env["NIX_V3_MAX_HEAP"]="2G"
    if mode=="P": env["NIX_V3_DIRECT_EVAL"]="1"; cmd=[V3,"--expr",expr]
    elif mode=="O": env["NIX_V3_DIRECT_EVAL"]="1"; env.pop("NIX_V3_REQUIRE",None); cmd=[NIX,"eval","--impure","--expr",expr]
    else: env.pop("NIX_V3_DIRECT_EVAL",None); cmd=[NIX,"eval","--impure","--expr",expr]
    try:
        r=subprocess.run(cmd,env=env,capture_output=True,text=True,errors="replace",timeout=timeout)
        lines=[l for l in (r.stdout+r.stderr).splitlines() if l.strip()]
        return (0 if r.returncode==0 else 1, r.stdout.strip().splitlines()[-1] if r.stdout.strip() else "",
                lines[-1] if lines else "")
    except subprocess.TimeoutExpired: return (2,"<timeout>","<timeout>")

def norm_err(s):  # normalize an error line to a comparable KIND signature
    s=re.sub(r'\x1b\[[0-9;]*m','',s)                      # ansi
    s=re.sub(r'/nix/store/[a-z0-9]{32}-','/nix/store/H-',s)  # store hashes
    s=re.sub(r"at «[^»]*»:\d+:\d+","at POS",s); s=re.sub(r":\d+:\d+","",s)
    s=re.sub(r'\bv3-eval\b','',s); s=s.replace("error:","").strip().lower()
    return s[:70]

# boundary value pools
I=["0","1","(-1)","2","(-9223372036854775807 - 1)","9223372036854775807","255","7","(-7)"]
S=['""','"a"','"café"','"1.2.3pre"','"a-b-1.0"','"x/y"','"/a/b"','"abc"','"{\\\"a\\\":1}"','"a=1\\nb=2"']
L=['[]','[1]','[1 2 3]','[1 [2] 3]','["a" "b"]','[{a=1;} {b=2;}]']
A=['{}','{a=1;}','{a=1;b=2;}','{a="x";}']
def pick(rng,pool): return rng.choice(pool)

# DIRECTED templates over uncovered / edge primops (pure; no network/store-write).
TEMPLATES=[
 'builtins.toXML {I}','builtins.toXML {L}','builtins.toXML {A}',
 'builtins.fromJSON {S}','builtins.fromJSON "[1,2.5,true,null]"','builtins.fromTOML {S}','builtins.fromTOML "a=1"',
 'builtins.hashString "sha256" {S}','builtins.hashString "md5" {S}','builtins.hashString "sha1" ""',
 'builtins.parseDrvName {S}','builtins.parseDrvName "foo-1.2.3"',
 'builtins.splitVersion {S}','builtins.splitVersion "1.2.3pre"',
 'builtins.compareVersions {S} {S}','builtins.compareVersions "1.0" "1.0.1"',
 'builtins.match "([0-9]+)-([a-z]+)" {S}','builtins.match "a*" {S}','builtins.split "(a)" {S}','builtins.split "" {S}',
 'builtins.replaceStrings [""] ["X"] {S}','builtins.replaceStrings ["a" ""] ["b" "Y"] {S}',
 'builtins.substring {I} {I} {S}','builtins.concatStringsSep "," (map toString {L})',
 'builtins.unsafeDiscardStringContext {S}','builtins.hasContext {S}','builtins.getContext {S}',
 'builtins.baseNameOf {S}','builtins.dirOf {S}','builtins.toString {L}','builtins.toString {A}',
 'builtins.typeOf {L}','builtins.typeOf {A}','builtins.isFunction (builtins.add 1)','builtins.functionArgs ({{a?1,b}}: a)',
 'builtins.functionArgs builtins.map','builtins.genericClosure {{ startSet=[{{key=1;}}]; operator = x: []; }}',
 'builtins.zipAttrsWith (n: vs: vs) {L}','builtins.mapAttrs (n: v: [n v]) {A}',
 'builtins.intersectAttrs {A} {A}','builtins.removeAttrs {A} ["a" "z"]','builtins.catAttrs "a" {L}',
 'builtins.listToAttrs {L}','builtins.attrValues {A}','builtins.attrNames {A}',
 'builtins.groupBy (x: if x>1 then "hi" else "lo") {L}','builtins.partition (x: x>1) {L}',
 'builtins.concatMap (x: [x x]) {L}','builtins.concatLists {L}','builtins.foldl'"'"' (a: b: a ++ [b]) [] {L}',
 'builtins.elem {I} {L}','builtins.elemAt {L} {I}','builtins.genList (x: x*x) {I}',
 'builtins.ceil {I}','builtins.floor {I}','builtins.bitAnd {I} {I}','builtins.bitXor {I} {I}',
 'builtins.seq {I} {S}','builtins.deepSeq {L} {I}','(builtins.tryEval (throw {S})).success','(builtins.tryEval {I}).success',
 'builtins.addErrorContext "ctx" {I}','builtins.toJSON {A}','builtins.toJSON {L}',
]
def fill(rng,t):
    for k,pool in (("{I}",I),("{S}",S),("{L}",L),("{A}",A)):
        while k in t: t=t.replace(k, pick(rng,pool),1)
    return t

# surface diff first
def attrnames(mode):
    rc,val,_=run(mode,"builtins.toJSON (builtins.attrNames builtins)")
    try:
        import json; return set(json.loads(json.loads(val))) if val.startswith('"') else set(json.loads(val))
    except Exception: return set()
pset=attrnames("O"); tset=attrnames("T")
print(f"== builtins surface: v3={len(pset)} TW={len(tset)}  v3-only={sorted(pset-tset)[:20]}  TW-only={sorted(tset-pset)[:20]}")

rng=random.Random(SEED); splits=[]; divs=[]
for i in range(N):
    e=fill(rng, rng.choice(TEMPLATES))
    prc,pv,pe=run("P",e); orc,ov,oe=run("O",e); trc,tv,te=run("T",e)
    # internal split: class mismatch, or both-ok value mismatch
    if prc!=orc or (prc==0 and orc==0 and pv!=ov): splits.append((e,prc,pv,orc,ov))
    # v3(opcode O) vs TW: (1) class parity hard; (2) value byte-id; (3) normalized error-kind soft
    if orc==0 and trc==0:
        if ov!=tv: divs.append(("VAL",e,ov,tv))
    elif (orc==0)!=(trc==0):
        divs.append(("CLASS",e,f"O rc{orc}:{oe[:40]}",f"T rc{trc}:{te[:40]}"))
    elif orc==1 and trc==1 and norm_err(oe)!=norm_err(te):
        divs.append(("KIND?",e,norm_err(oe),norm_err(te)))
print(f"==== seed={SEED} N={N}: {len(splits)} internal-split, {len(divs)} v3-vs-TW (incl soft KIND) ====")
for e,prc,pv,orc,ov in splits[:15]: print(f"SPLIT  P(rc{prc})=[{pv[:45]}] O(rc{orc})=[{ov[:45]}] :: {e[:95]}")
for kind,e,a,b in divs[:30]: print(f"{kind:6} O=[{a[:48]}] T=[{b[:48]}] :: {e[:95]}")

# Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>, Input Output Group.
# SPDX-License-Identifier: Apache-2.0
