#!/usr/bin/env bash
set -euo pipefail

format_binary="${CLANG_FORMAT:-clang-format}"
command -v "${format_binary}" >/dev/null 2>&1 || {
  echo "clang-format is required; set CLANG_FORMAT to its path" >&2
  exit 1
}

mapfile -d '' source_files < <(
  find common mlvc application apps tools ports custom_ops -type f \
    \( -name '*.h' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cpp' \
       -o -name '*.cu' -o -name '*.cuh' \) -print0
)
if ((${#source_files[@]} == 0)); then
  exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
  "${format_binary}" --dry-run --Werror "${source_files[@]}"
else
  "${format_binary}" -i "${source_files[@]}"
fi
