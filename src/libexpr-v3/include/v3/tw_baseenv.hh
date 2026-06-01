#pragma once
/// @file
/// FFI query: the set of names TW exposes as BARE base-env globals.
///
/// The v3-native lowerer (cli/lower_v3.hh) has no TW bindVars, so it
/// resolves a free name to a primop ONLY if that name is one of TW's
/// actual bare globals — otherwise it falls through to the `with`-chain,
/// exactly as bindVars classifies it.  Without this, the native lowerer
/// would shortcut nixpkgs' bare `fetchurl` (the `with pkgs`-bound FOD) to
/// the `builtins.fetchurl` primop — a store-path-affecting divergence
/// (firefox.drvPath).  See run-baseenv-globals-tests.sh.
///
/// This is an explicit, documented FFI leaf (reads TW's `staticBaseEnv`)
/// per the V3-NATIVE constraint — parsing/resolution metadata, not
/// evaluation.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "nix/expr/eval.hh"

#include <set>
#include <string>

namespace nix::v3 {

/// TW's bare base-env globals (`builtins`, `true`/`false`/`null`,
/// `import`, `derivation`, `map`, `throw`, … — NOT the `__`-prefixed
/// builtins-only primops).  Built once per process (single TW
/// EvalState); authoritative by construction — it IS TW's base env,
/// walked including the `up` chain.
inline const std::set<std::string> & twBaseEnvGlobals(const nix::EvalState & es)
{
    static const std::set<std::string> s = [&] {
        std::set<std::string> out;
        for (const auto * env = es.staticBaseEnv.get(); env; env = env->up.get())
            for (const auto & v : env->vars)
                out.insert(std::string(es.symbols[v.first]));
        return out;
    }();
    return s;
}

}  // namespace nix::v3
