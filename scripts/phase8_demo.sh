#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${BUILD_DIR:-}" ]]; then
    case "$(uname -s)" in
        Linux*) BUILD_DIR="${HOME:-/tmp}/bank-build" ;;
        *) BUILD_DIR="${ROOT_DIR}/build-gcc" ;;
    esac
fi
BINARY="${BUILD_DIR}/bank"
SKIP_BUILD=0
SKIP_CHECK=0
for argument in "$@"; do
    case "${argument}" in
        --skip-build) SKIP_BUILD=1 ;;
        --skip-check) SKIP_CHECK=1 ;;
        *) printf 'unknown option: %s\n' "${argument}" >&2; exit 2 ;;
    esac
done

step() {
    printf '\n=== %s ===\n' "$1"
}

if [[ "${SKIP_BUILD}" -eq 0 ]]; then
    step 'Build'
    bash "${ROOT_DIR}/scripts/build.sh"
fi
if [[ ! -x "${BINARY}" ]]; then
    printf 'bank executable was not found at %s\n' "${BINARY}" >&2
    exit 1
fi

cd "${ROOT_DIR}"
step 'Startup recovery'
"${BINARY}" --recover
if [[ "${SKIP_CHECK}" -eq 0 ]]; then
    step 'Consistency checker'
    "${BINARY}" --check --baseline-file "${ROOT_DIR}/results/baseline.txt"
fi
step 'Safe live simulation'
"${BINARY}" --accounts 50 --clients 20 --ops 100 --workers 4 --seed 42 --verbose
step 'Unsafe lost-update test'
ctest --test-dir "${BUILD_DIR}" --output-on-failure \
    -R 'UnsafeRaceTest.R2ReportsLostUpdatesAcrossTwentyRuns'
step 'Safe synchronization tests'
ctest --test-dir "${BUILD_DIR}" --output-on-failure \
    -R 'SynchronizationTest.R1|SynchronizationTest.R3|SynchronizationTest.D2'
printf '\n=== Unsupported live crash step ===\n'
printf '%s\n' 'X2 process-kill recovery is documented but has no automated harness yet.'
printf '%s\n' 'See docs/DEMO_RUNBOOK.md for the evidence boundary and next step.'
