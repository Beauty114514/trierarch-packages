#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_COMMIT="0543368d3262e098b92ff0305e149ff0d1fce3e1"
SOURCE_DIR="${PIPEWIRE_REFERENCE_SOURCE_DIR:-$ROOT_DIR/sources/pipewire-standalone-aaudio-reference}"
SOURCE_URL="${1:-}"

if [[ -z "$SOURCE_URL" ]]; then
    echo "Usage: $0 <upstream-source-git-url>" >&2
    exit 2
fi

command -v git >/dev/null || { echo "git is required" >&2; exit 1; }
mkdir -p "$ROOT_DIR/sources"

if [[ ! -d "$SOURCE_DIR/.git" ]]; then
    git init "$SOURCE_DIR"
    git -C "$SOURCE_DIR" remote add origin "$SOURCE_URL"
fi

if [[ "$(git -C "$SOURCE_DIR" remote get-url origin)" != "$SOURCE_URL" ]]; then
    echo "Unexpected source origin in $SOURCE_DIR; use a clean reference directory" >&2
    exit 1
fi

git -C "$SOURCE_DIR" fetch --depth=1 origin "$SOURCE_COMMIT"
git -C "$SOURCE_DIR" checkout --detach FETCH_HEAD

actual_commit="$(git -C "$SOURCE_DIR" rev-parse HEAD)"
if [[ "$actual_commit" != "$SOURCE_COMMIT" ]]; then
    echo "PipeWire source commit mismatch: $actual_commit" >&2
    exit 1
fi

echo "PipeWire/AAudio upstream reference pinned at $actual_commit"
