#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${BUILD_DIR:-}" ]]; then
    case "$(uname -s)" in
        Linux*) BUILD_DIR="${HOME:-/tmp}/bank-build" ;;
        *) BUILD_DIR="${ROOT_DIR}/build-gcc" ;;
    esac
fi
BASELINE_FILE="${BASELINE_FILE:-${ROOT_DIR}/results/baseline.txt}"

bash "${ROOT_DIR}/scripts/build.sh"

if [[ ! -f "${BASELINE_FILE}" ]]; then
    printf 'baseline file was not found at %s\n' "${BASELINE_FILE}" >&2
    exit 1
fi

cd "${ROOT_DIR}"
"${BUILD_DIR}/bank" --recover --check --baseline-file "${BASELINE_FILE}" "$@"
