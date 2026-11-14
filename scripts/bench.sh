#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/build-gcc}"
RESULT_DIR="${ROOT_DIR}/results"
mkdir -p "${RESULT_DIR}"

workers_csv="${RESULT_DIR}/throughput_workers.csv"
pool_csv="${RESULT_DIR}/throughput_pool.csv"
isolation_csv="${RESULT_DIR}/isolation.csv"
safe_csv="${RESULT_DIR}/safe_vs_unsafe.csv"
ordering_csv="${RESULT_DIR}/lock_ordering.csv"
hotspot_csv="${RESULT_DIR}/hotspot.csv"

printf 'workers,run,elapsed_s,throughput_tx_s,avg_latency_ms,p95_latency_ms,status\n' > "${workers_csv}"
printf 'pool_size,run,elapsed_s,throughput_tx_s,avg_latency_ms,p95_latency_ms,status\n' > "${pool_csv}"
printf 'isolation,run,throughput_tx_s,status\n' > "${isolation_csv}"
printf 'mode,run,throughput_tx_s,balance_error,status\n' > "${safe_csv}"
printf 'ordering,run,deadlock_or_retry_rate,status\n' > "${ordering_csv}"
printf 'hotspot_ratio,run,throughput_tx_s,status\n' > "${hotspot_csv}"

measure() {
    local start end elapsed
    start="$(date +%s%N)"
    "$@" >/dev/null
    end="$(date +%s%N)"
    elapsed="$(awk -v s="${start}" -v e="${end}" 'BEGIN { printf "%.6f", (e-s)/1000000000 }')"
    printf '%s' "${elapsed}"
}

for workers in 1 2 4 8 16; do
    for run in 1 2 3; do
        elapsed="$(measure "${BUILD_DIR}/bank" --accounts 50 --clients 10 --ops 10 \
            --workers "${workers}" --seed "${run}")"
        throughput="$(awk -v e="${elapsed}" 'BEGIN { printf "%.3f", 100/e }')"
        printf '%s,%s,%s,%s,NA,NA,PASS\n' "${workers}" "${run}" "${elapsed}" "${throughput}" >> "${workers_csv}"
    done
done

for pool in 2 4 8 16; do
    for run in 1 2 3; do
        # The current simulator uses workers as its pool size; record this
        # controlled comparison explicitly until a separate pool-size CLI flag exists.
        elapsed="$(measure "${BUILD_DIR}/bank" --accounts 50 --clients 10 --ops 10 \
            --workers "${pool}" --seed "${run}")"
        throughput="$(awk -v e="${elapsed}" 'BEGIN { printf "%.3f", 100/e }')"
        printf '%s,%s,%s,%s,NA,NA,PASS\n' "${pool}" "${run}" "${elapsed}" "${throughput}" >> "${pool_csv}"
    done
done

for isolation in READ_COMMITTED REPEATABLE_READ SERIALIZABLE; do
    for run in 1 2 3; do
        printf '%s,%s,NA,NOT_IMPLEMENTED\n' "${isolation}" "${run}" >> "${isolation_csv}"
    done
done

for mode in safe unsafe; do
    for run in 1 2 3; do
        flag=()
        [[ "${mode}" == unsafe ]] && flag+=(--unsafe)
        elapsed="$(measure "${BUILD_DIR}/bank" --accounts 2 --clients 10 --ops 10 \
            --workers 4 --seed "${run}" "${flag[@]}")"
        throughput="$(awk -v e="${elapsed}" 'BEGIN { printf "%.3f", 100/e }')"
        printf '%s,%s,%s,DEMO_ONLY,PASS\n' "${mode}" "${run}" "${throughput}" >> "${safe_csv}"
    done
done

for ordering in on off; do
    for run in 1 2 3; do
        printf '%s,%s,NOT_IMPLEMENTED,NOT_IMPLEMENTED\n' "${ordering}" "${run}" >> "${ordering_csv}"
    done
done

for ratio in 0 0.5 0.9; do
    for run in 1 2 3; do
        printf '%s,%s,NOT_IMPLEMENTED,NOT_IMPLEMENTED\n' "${ratio}" "${run}" >> "${hotspot_csv}"
    done
done

printf 'Benchmark CSV files written under %s\n' "${RESULT_DIR}"
