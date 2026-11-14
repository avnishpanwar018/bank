#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/build-tsan}"
TEST_BIN="${BUILD_DIR}/bank_tests"
RESULT_DIR="${ROOT_DIR}/results"
SUMMARY="${RESULT_DIR}/s5_summary.csv"

mkdir -p "${RESULT_DIR}"
if [[ ! -f "${SUMMARY}" ]]; then
    printf 'seed,committed,rolled_back,retries,I1,I2,I3,I4,I5\n' > "${SUMMARY}"
fi

start_seed=1
if [[ -f "${SUMMARY}" ]]; then
    last_seed="$(awk -F, 'NR > 1 { seed=$1 } END { print seed }' "${SUMMARY}")"
    if [[ "${last_seed}" =~ ^[0-9]+$ ]]; then
        start_seed=$((last_seed + 1))
    fi
fi

for seed in $(seq "${start_seed}" 50); do
    output="$(cd "${ROOT_DIR}" && BANK_STRESS_SEED="${seed}" "${TEST_BIN}" \
        --gtest_filter=StressTest.S1HundredClientsMixedOperations 2>&1)" || {
        printf 'S5 failed at seed=%s\n' "${seed}" >&2
        printf '%s\n' "${output}" >&2
        exit 1
    }
    printf '%s\n' "${output}"
    line="$(printf '%s\n' "${output}" | awk '/^S1 seed=/{print; exit}')"
    committed="$(printf '%s' "${line}" | sed -n 's/.*committed=\([0-9][0-9]*\).*/\1/p')"
    rolled_back="$(printf '%s' "${line}" | sed -n 's/.*rolled_back=\([0-9][0-9]*\).*/\1/p')"
    i1="$(printf '%s' "${line}" | sed -n 's/.*I1=\([01]\).*/\1/p')"
    i2="$(printf '%s' "${line}" | sed -n 's/.*I2=\([01]\).*/\1/p')"
    i3="$(printf '%s' "${line}" | sed -n 's/.*I3=\([01]\).*/\1/p')"
    i4="$(printf '%s' "${line}" | sed -n 's/.*I4=\([01]\).*/\1/p')"
    i5="$(printf '%s' "${line}" | sed -n 's/.*I5=\([01]\).*/\1/p')"
    if [[ -z "${committed}" || -z "${rolled_back}" || "${i1}" != 1 || "${i2}" != 1 ||
          "${i3}" != 1 || "${i4}" != 1 || "${i5}" != 1 ]]; then
        printf 'S5 malformed or invariant-failing output at seed=%s\n' "${seed}" >&2
        exit 1
    fi
    printf '%s,%s,%s,0,%s,%s,%s,%s,%s\n' "${seed}" "${committed}" \
        "${rolled_back}" "${i1}" "${i2}" "${i3}" "${i4}" "${i5}" >> "${SUMMARY}"
done

printf 'S5 summary written to %s (50/50 passed)\n' "${SUMMARY}"
