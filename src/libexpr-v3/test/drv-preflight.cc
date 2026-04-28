/// @file
/// BR-3.0 — pre-flight libnixstore harness.
///
/// Constructs a hard-coded `nix::Derivation` matching the v3 expression
///
///   derivation { name = "test"; system = "x86_64-linux"; builder = "/bin/sh"; }
///
/// and asserts the resulting drvPath matches the value tree-walker's
/// nix-instantiate produces for the same input.  Validates that the
/// libnixstore APIs (BasicDerivation construction, fillInOutputPaths,
/// computeStorePath / writeDerivation) are usable from a freestanding
/// binary in readOnlyMode (`dummy://` store) before the v3 native
/// derivationStrict path is built on top of them.
///
/// Reference value comes from:
///   ./build/src/nix/nix-instantiate --eval --strict
///     -E '(import /tmp/refdrv.nix).drvPath'
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0
#include "nix/main/shared.hh"  // nix::initNix
#include "nix/store/derivations.hh"
#include "nix/store/derivation-options.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-api.hh"
#include "nix/store/store-open.hh"

#include <cstdio>
#include <string>

namespace {

// Reference drvPath produced by tree-walker for the test input above.
// If the libnixstore protocol changes (e.g. ATerm format / hash algo),
// this needs to be regenerated and the v3 native path retested.
constexpr const char * kExpectedDrvPath =
    "/nix/store/y1s2fiq89v2h9vkb38w508ir20dwv6v2-test.drv";

}  // namespace

int main()
{
    nix::initNix();
    nix::settings.readOnlyMode = true;
    auto store = nix::openStore("dummy://");

    // Match what `derivation { name; system; builder; }` produces in
    // tree-walker's derivationStrictInternal: a deferred-output drv
    // with one "out" output, env vars for builder/name/out/system
    // (lex order), and outputs={out: Deferred{}}.
    nix::Derivation drv;
    drv.name = "test";
    drv.builder = "/bin/sh";
    drv.platform = "x86_64-linux";

    // tree-walker iterates attrs in lexicographic order and emits each
    // into env via coerceToString.  For this minimal input the env is:
    drv.env.emplace("builder",  "/bin/sh");
    drv.env.emplace("name",     "test");
    // "out" is set to "" pre-fillInOutputPaths and overwritten by it.
    drv.env.emplace("out",      "");
    drv.env.emplace("system",   "x86_64-linux");

    // Single "out" output, deferred (input-addressed, path computed
    // by fillInOutputPaths).
    drv.outputs.insert_or_assign(
        "out", nix::DerivationOutput{nix::DerivationOutput::Deferred{}});

    // Fill in the output paths.  For deferred outputs this also
    // overwrites drv.env["out"] with the computed path.
    drv.fillInOutputPaths(*store);

    // Compute the drvPath without writing (readOnlyMode is set).
    nix::StorePath drvPath = nix::computeStorePath(*store, drv);
    std::string drvPathS = store->printStorePath(drvPath);

    std::fprintf(stderr, "drv-preflight: computed drvPath = %s\n",
        drvPathS.c_str());
    std::fprintf(stderr, "drv-preflight: expected drvPath = %s\n",
        kExpectedDrvPath);

    if (drvPathS != kExpectedDrvPath) {
        std::fprintf(stderr,
            "drv-preflight: FAIL — drvPath mismatch.\n"
            "  This means libnixstore's BasicDerivation/fillInOutputPaths/"
            "computeStorePath chain is producing a different drv-hash "
            "than tree-walker's derivationStrict for the same input "
            "fields.  Check env/outputs/builder/platform fields above.\n");
        return 1;
    }

    std::fprintf(stderr,
        "drv-preflight: OK — libnixstore drvPath matches tree-walker.\n");
    return 0;
}
