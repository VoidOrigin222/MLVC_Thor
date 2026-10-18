#!/usr/bin/env bash
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
tool="$thor_root/src/MLVC/ports/thor/tools/publish_test_sources.py"
[[ -f "$tool" ]] || tool="$thor_root/ports/thor/tools/publish_test_sources.py"
exec python3 "$tool" "$@"