#!/usr/bin/env bash
set -euo pipefail

info() { echo "[INFO] $*"; }
warn() { echo "[WARN] $*"; }
error() { echo "[ERROR] $*" >&2; }

usage() {
    echo "Usage: $0 [--dry-run]"
}


require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        error "required command not found: $1"
        exit 1
    fi
}

cleanup_legacy_module_dirs() {
    local modules_root="/lib/modules"
    local legacy_dir

    [[ -d "$modules_root" ]] || return 0

    for legacy_dir in "$modules_root"/*/kernel/drivers/net/wireless/aic8800; do
        [[ -d "$legacy_dir" ]] || continue

        if sudo rmdir "$legacy_dir" 2>/dev/null; then
            info "Removed empty legacy module directory: $legacy_dir"
        else
            warn "Legacy module directory is not empty, keeping: $legacy_dir"
        fi
    done
}

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_NAME="aic8800fdrv"
VERSION_FILE="$REPO_ROOT/VERSION"
DRY_RUN=0

if [[ $# -gt 1 ]]; then
    usage >&2
    exit 2
fi
if [[ ${1:-} == "--dry-run" ]]; then
    DRY_RUN=1
elif [[ $# -ne 0 ]]; then
    usage >&2
    exit 2
fi

if [[ ! -f "$VERSION_FILE" ]]; then
    error "VERSION file not found: $VERSION_FILE"
    exit 1
fi

require_cmd dkms
require_cmd sudo

KEEP_VERSION="$(<"$VERSION_FILE")"
if [[ ! "$KEEP_VERSION" =~ ^[0-9]+(\.[0-9]+)*([.-][0-9A-Za-z]+)*$ ]]; then
    error "invalid VERSION content: ${KEEP_VERSION:-<empty>}"
    exit 1
fi

info "Module      : $MODULE_NAME"
info "Keep version: $KEEP_VERSION"

declare -a versions=()
while IFS= read -r line; do
    versions+=("$line")
done < <(dkms status | awk -F'[,/]' -v m="$MODULE_NAME" '$1==m{gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2}' | sort -u)

if [[ ${#versions[@]} -eq 0 ]]; then
    info "No DKMS entries found for $MODULE_NAME"
    exit 0
fi

removed=0
failed=0
for version in "${versions[@]}"; do
    if [[ "$version" == "$KEEP_VERSION" ]]; then
        continue
    fi

    if ((DRY_RUN)); then
        info "Would remove old version: $version"
        continue
    fi

    info "Removing old version: $version"
    if sudo dkms remove -m "$MODULE_NAME" -v "$version" --all; then
        cleanup_legacy_module_dirs
        removed=$((removed + 1))
    else
        warn "Failed to remove $MODULE_NAME/$version"
        failed=$((failed + 1))
    fi
done

if ((DRY_RUN)); then
    echo "[OK] Dry run completed. Kept version: $KEEP_VERSION"
    exit 0
fi

info "Removed versions: $removed"
info "Final DKMS status"
dkms status | grep "$MODULE_NAME" || true

if ((failed)); then
    error "Failed removals: $failed"
    exit 2
fi

echo "[OK] Cleanup completed. Kept version: $KEEP_VERSION"
