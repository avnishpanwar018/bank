#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -z "${BUILD_DIR:-}" ]]; then
    case "$(uname -s)" in
        Linux*) BUILD_DIR="${HOME:-/tmp}/bank-build" ;;
        *) BUILD_DIR="${ROOT_DIR}/build-gcc" ;;
    esac
fi
BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}"

if [[ -z "${CMAKE_COMMAND:-}" ]]; then
    CMAKE_COMMAND="$(command -v cmake || true)"
fi
if [[ -z "${CMAKE_COMMAND}" && -x "/mnt/c/Program Files/CMake/bin/cmake.exe" ]]; then
    CMAKE_COMMAND="/mnt/c/Program Files/CMake/bin/cmake.exe"
fi
if [[ -z "${CMAKE_COMMAND}" ]]; then
    printf 'cmake was not found; set CMAKE_COMMAND to its executable path\n' >&2
    exit 1
fi

"${CMAKE_COMMAND}" -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE="${BUILD_TYPE}"

build_args=(--build "${BUILD_DIR}")
if [[ -n "${CMAKE_BUILD_PARALLEL_LEVEL:-}" ]]; then
    build_args+=(--parallel "${CMAKE_BUILD_PARALLEL_LEVEL}")
else
    build_args+=(--parallel 2)
fi
"${CMAKE_COMMAND}" "${build_args[@]}"
