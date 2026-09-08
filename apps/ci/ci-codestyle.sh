#!/bin/bash
set -euo pipefail

echo "Codestyle check script:"
echo

if ! command -v rg >/dev/null 2>&1; then
  echo "ripgrep (rg) is required" >&2
  exit 2
fi

check_pattern() {
  local mode="$1"
  local pattern="$2"
  local message="$3"
  local options=(-n)

  if [[ "$mode" == "multiline" ]]; then
    options+=(-U)
  fi

  echo "  Checking RegEx: '${pattern}'"
  if rg "${options[@]}" -- "$pattern" src; then
    echo
    echo "$message"
    exit 1
  fi
}

check_pattern single 'LOG_.+GetCounter' \
  'Use ObjectGuid::ToString().c_str() instead of ObjectGuid::GetCounter() when logging. Check the lines above'
check_pattern single '[[:blank:]]$' \
  'Remove whitespace at the end of the lines above'
check_pattern single '\t' \
  'Replace tabs with 4 spaces in the lines above'
check_pattern multiline 'LOG_[^;]+GetCounter' \
  'Use ObjectGuid::ToString().c_str() instead of ObjectGuid::GetCounter() when logging. Check the lines above'
check_pattern multiline '\n\n\n' \
  'Multiple blank lines detected, keep only one. Check the lines above'

echo
echo "Everything looks good"
