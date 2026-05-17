#!/usr/bin/env bash
# TW vs v3-direct primop property-test runner.
#
# Wraps property_tests.py with the right nix-bin path and shell env.
# Designed to be called from CI or `make test`-style targets.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
exec python3 "$HERE/property_tests.py" "$@"
