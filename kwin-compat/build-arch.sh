#!/usr/bin/env bash
set -euo pipefail
[[ $# -ge 2 ]] || { echo "usage: $0 SOURCE BUILD [JOBS]" >&2; exit 2; }
exec "$(dirname "$0")/build-source.sh" --source "$1" --build "$2" --jobs "${3:-$(getconf _NPROCESSORS_ONLN)}"
