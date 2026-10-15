#!/usr/bin/env bash
set -euo pipefail
usage() { echo 'Usage: bash scripts/run_encode.sh [config.toml] (RTSP input only)'; }
if [[ "${1:-}" == --help ]]; then usage; exit 0; fi
case "$#:${1:-}" in
  0:|1:rtsp) config=configs/encoder_rtsp.toml ;;
  1:*.toml) config=$1 ;;
  2:rtsp) config=$2 ;;
  *) usage >&2; exit 2 ;;
esac
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[[ -f "$config" ]] || { echo "Missing configuration: $config" >&2; exit 1; }
exec bin/mlvc_encode_thor --config "$config"
