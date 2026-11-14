#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${BUILD_DIR:-}" ]]; then
    case "$(uname -s)" in
        Linux*) BUILD_DIR="${HOME:-/tmp}/bank-build" ;;
        *) BUILD_DIR="${ROOT_DIR}/build-gcc" ;;
    esac
fi

bash "${ROOT_DIR}/scripts/build.sh"

if [[ ! -x "${BUILD_DIR}/bank" ]]; then
    printf 'bank executable was not produced at %s\n' "${BUILD_DIR}/bank" >&2
    exit 1
fi

if [[ "$#" -eq 0 ]]; then
    set -- --accounts 50 --clients 10 --ops 10 --workers 4 --seed 42 --verbose
fi

cd "${ROOT_DIR}"
"${BUILD_DIR}/bank" "$@"
