#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[[ $# -le 1 ]] || { echo 'Usage: bash scripts/run_decoder.sh [configs/decoder.toml]' >&2; exit 2; }
config=${1:-configs/decoder.toml}
[[ -f "$config" ]] || { echo "Missing configuration: $config" >&2; exit 1; }
exec bin/decoder --config "$config"
