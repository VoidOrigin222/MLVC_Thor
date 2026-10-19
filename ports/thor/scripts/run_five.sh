#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

operation="${1:-}"
mode="${2:-configs/${operation}r.toml}"
if [[ "$operation" != encode && "$operation" != decode ]]; then
  echo "Usage: bash scripts/run_five.sh encode|decode [config.toml]" >&2
  exit 1
fi
if [[ "$mode" == *.toml ]]; then
  [[ $# -le 2 && -f "$mode" ]] || { echo "Missing configuration or unexpected arguments: $mode" >&2; exit 1; }
  exec "bin/${operation}r" --config "$mode"
fi
# Retain explicit legacy modes for existing benchmarks and deployment scripts.
if [[ "$mode" != processes && "$mode" != contexts && "$mode" != batch5 ]]; then
  echo "Mode must be processes, contexts or batch5" >&2
  exit 1
fi
mkdir -p logs
configs=()
config_mode="$mode"
[[ "$mode" != processes ]] || config_mode=contexts
for lane in 0 1 2 3 4; do
  template="configs/five/$config_mode/$operation-$lane.toml"
  [[ -f "$template" ]] || template="src/MLVC/ports/thor/$template"
  [[ -f "$template" ]] || { echo "Missing benchmark template: $template" >&2; exit 1; }
  configs+=("$template")
done
if [[ "$mode" == processes ]]; then
  unset MLVC_THOR_BATCH5 MLVC_THOR_SHARE_ENGINES
  trap 'kill $(jobs -pr) 2>/dev/null || true' EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM
  task_pids=()
  for lane in 0 1 2 3 4; do
    "bin/mlvc_${operation}_thor" --config "${configs[$lane]}" \
      >"logs/five-$operation-lane$lane.log" 2>&1 &
    task_pids+=("$!")
  done
  status=0
  for task_pid in "${task_pids[@]}"; do
    wait "$task_pid" || status=1
  done
  exit "$status"
fi
exec bin/mlvc_multi_thor "$operation" "$mode" "logs/five-$operation" "${configs[@]}"
