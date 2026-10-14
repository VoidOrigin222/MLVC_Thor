#!/usr/bin/env bash
set -euo pipefail
thor_root=${MLVC_THOR_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
while [[ ! -f "$thor_root/models/manifest_thor.json" ]]; do
  [[ "$thor_root" != / ]] || { echo 'Set MLVC_THOR_ROOT to the installed Thor workspace.' >&2; exit 1; }
  thor_root=$(dirname "$thor_root")
done
cd "$thor_root"
mkdir -p logs data/bitstreams data/decoded
