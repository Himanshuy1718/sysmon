#!/usr/bin/env bash
# Pushes ALL CPU cores to 100% for N seconds (default 60), to trigger cpu_high.
# One busy-loop process per core: WSL sees 16 cores on a Ryzen 7, so one
# process alone would only show ~6% overall.
# Usage: ./scripts/stress_cpu.sh [seconds]
set -euo pipefail
SECONDS_TO_RUN="${1:-60}"
CORES="$(nproc)"
echo "Loading ${CORES} cores for ${SECONDS_TO_RUN}s (Ctrl+C to stop early)..."
pids=()
for _ in $(seq "${CORES}"); do
  timeout "${SECONDS_TO_RUN}" yes > /dev/null &
  pids+=($!)
done
trap 'kill "${pids[@]}" 2>/dev/null; echo; echo stopped.' INT TERM
wait
echo "done."
