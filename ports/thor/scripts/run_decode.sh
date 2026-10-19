#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
config=${1:-configs/decoder_rtp.toml}
if [[ "$config" == configs/decoder_rtp.toml && ! -f "$config" ]]; then
  config="src/MLVC/ports/thor/$config"
fi
[[ -f "$config" ]] || { echo "Missing configuration: $config" >&2; exit 1; }
exec bin/mlvc_decode_thor --config "$config"
