#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[[ $# -le 1 ]] || { echo 'Usage: bash scripts/run_encoder.sh [configs/encoder.toml]' >&2; exit 2; }
config=${1:-configs/encoder.toml}
[[ -f "$config" ]] || { echo "Missing configuration: $config" >&2; exit 1; }
exec bin/encoder --config "$config"
