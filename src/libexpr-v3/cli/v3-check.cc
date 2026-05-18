/// @file
/// v3-check — LLVM-FileCheck-compatible (subset) directive matcher.
///
/// Usage:
///   v3-check MATCH-FILE [--check-prefix=PREFIX]
///
/// Reads the "actual" output text from stdin, the directive source
/// from MATCH-FILE (a Nix fixture file with `# CHECK:` directives or
/// any text with `; CHECK:` / `// CHECK:` directives), and runs the
/// extended `checkIr` library against them.
///
/// Exits 0 on PASS, non-zero with diagnostic on FAIL.
///
/// Supported directives (see include/v3/ir_dump.hh for the full
/// language spec):
///   CHECK:, CHECK-NOT:, CHECK-LABEL:, CHECK-NEXT:
///   `{{regex}}` embedded regex within otherwise-literal patterns.
///   Comment-prefix `;`, `#`, or `//`.
///   RUN:/COM: line-skip — directive lines containing "RUN:" or
///   "COM:" are not parsed as CHECK directives.
///
/// Intentionally a tiny ~80-line binary — the heavy lifting is in
/// `ir_dump.cc::checkIrEx`.  See
/// lode/IR_CHECK_INFRASTRUCTURE_PLAN_2026-05-18.md §2.7 for the
/// "build our own, don't pull in LLVM toolchain" decision.
///
/// Copyright (c) 2026 Moritz Angermann <moritz.angermann@iohk.io>,
///   Input Output Group.
/// SPDX-License-Identifier: Apache-2.0

#include "v3/ir_dump.hh"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

static void usage(const char * argv0)
{
    std::fprintf(stderr,
        "usage: %s MATCH-FILE [--check-prefix=PREFIX]\n"
        "       (reads actual text from stdin, exits 0 on PASS)\n",
        argv0);
}

static std::string slurpFile(const std::string & path)
{
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open '" + path + "'");
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string slurpStdin()
{
    std::stringstream ss;
    ss << std::cin.rdbuf();
    return ss.str();
}

int main(int argc, char ** argv)
{
    std::string matchFile;
    std::string prefix = "CHECK";

    for (int i = 1; i < argc; ++i) {
        std::string_view a(argv[i]);
        constexpr std::string_view prefixFlag = "--check-prefix=";
        if (a == "--help" || a == "-h") {
            usage(argv[0]);
            return 0;
        }
        if (a.substr(0, prefixFlag.size()) == prefixFlag) {
            prefix = std::string(a.substr(prefixFlag.size()));
            if (prefix.empty()) {
                std::fprintf(stderr,
                    "v3-check: --check-prefix= requires a non-empty value\n");
                return 2;
            }
            continue;
        }
        if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr,
                "v3-check: unknown flag '%s'\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
        if (matchFile.empty()) {
            matchFile = argv[i];
        } else {
            std::fprintf(stderr,
                "v3-check: only one MATCH-FILE argument is supported\n");
            return 2;
        }
    }

    if (matchFile.empty()) {
        usage(argv[0]);
        return 2;
    }

    try {
        std::string expected = slurpFile(matchFile);
        std::string actual   = slurpStdin();

        std::string err = nix::v3::ir::checkIrEx(actual, expected, prefix);
        if (err.empty()) {
            // Silent PASS — keeps test output clean.  The runner
            // reports per-fixture PASS/FAIL.
            return 0;
        }
        std::fprintf(stderr,
            "v3-check FAIL on match-file '%s' (prefix=%s):\n%s\n",
            matchFile.c_str(), prefix.c_str(), err.c_str());
        return 1;
    } catch (const std::exception & ex) {
        std::fprintf(stderr, "v3-check error: %s\n", ex.what());
        return 2;
    }
}
