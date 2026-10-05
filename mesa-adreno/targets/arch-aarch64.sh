#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)

case ${1:---help} in
    --check)
        "$root_dir/scripts/verify-source-lock.sh"
        printf '%s\n' 'mesa-adreno: Arch aarch64 target contract is ready; build steps are not implemented yet.'
        ;;
    --prepare-source)
        "$root_dir/scripts/prepare-source.sh" --prepare
        ;;
    --inspect-release)
        [[ $# -eq 2 ]] || {
            printf '%s\n' 'usage: arch-aarch64.sh --inspect-release <release-archive.tar>' >&2
            exit 64
        }
        "$root_dir/scripts/inspect-arch-release.sh" "$2"
        ;;
    --show-reference-options)
        awk '
            /^\[reference_build\]/ { printing = 1 }
            printing && printed && /^\[/ { exit }
            printing { print; printed = 1 }
        ' "$root_dir/targets/arch-aarch64.toml"
        ;;
    --help)
        cat <<'EOF'
Usage: arch-aarch64.sh --check | --prepare-source
       arch-aarch64.sh --inspect-release <release-archive.tar>
       arch-aarch64.sh --show-reference-options

Validates the source provenance required before an Arch Linux ARM bundle can
be built.  --prepare-source creates an isolated source worktree; this script
deliberately has no build mode yet.
EOF
        ;;
    *)
        printf 'unknown argument: %s\n' "$1" >&2
        exit 64
        ;;
esac
