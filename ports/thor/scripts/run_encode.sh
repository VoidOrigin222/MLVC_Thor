#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
case "${1:-offline}" in
  offline|rtsp|camera|fp16) mode=${1:-offline} ;;
  *) echo 'Usage: bash scripts/run_encode.sh {offline|rtsp|camera|fp16} [config.toml]' >&2; exit 2 ;;
esac
config=${2:-configs/encoder_${mode}.toml}
[[ -f "$config" ]] || { echo "Missing configuration: $config" >&2; exit 1; }
exec bin/mlvc_encode_thor --config "$config"
