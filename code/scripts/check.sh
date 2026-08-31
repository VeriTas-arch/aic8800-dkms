#!/usr/bin/env bash
set -euo pipefail

info() { echo "[INFO] $*"; }
error() { echo "[ERROR] $*" >&2; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

for command in bash git; do
    if ! command -v "$command" >/dev/null 2>&1; then
        error "required command not found: $command"
        exit 1
    fi
done

info "Check shell syntax"
while IFS= read -r -d '' script; do
    bash -n "$script"
done < <(find "$SCRIPT_DIR" -maxdepth 1 -type f -name '*.sh' -print0)

info "Check version metadata"
"$SCRIPT_DIR/sync-version.sh" --check

info "Check tracked whitespace"
git -C "$REPO_ROOT" diff --check

mapfile -t kernels < <(
    for build_dir in /lib/modules/*/build; do
        [[ -d "$build_dir" ]] || continue
        basename "$(dirname "$build_dir")"
    done | sort -V -u
)

if [[ ${#kernels[@]} -eq 0 ]]; then
    error "no usable kernel build directories found under /lib/modules"
    exit 1
fi

info "Compile-only matrix: ${kernels[*]}"
for kernel_ver in "${kernels[@]}"; do
    "$SCRIPT_DIR/build-test.sh" "$kernel_ver"
done

echo "[OK] Repository checks and compile-only matrix passed"
