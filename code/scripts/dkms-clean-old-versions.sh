#!/usr/bin/env bash
set -euo pipefail

info() { echo "[INFO] $*"; }
warn() { echo "[WARN] $*"; }
error() { echo "[ERROR] $*" >&2; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_NAME="aic8800fdrv"
VERSION_FILE="$REPO_ROOT/VERSION"

if [[ ! -f "$VERSION_FILE" ]]; then
    error "VERSION file not found: $VERSION_FILE"
    exit 1
fi

if ! command -v dkms >/dev/null 2>&1; then
    error "dkms is not installed. Please install dkms first"
    exit 1
fi

KEEP_VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
if [[ -z "$KEEP_VERSION" ]]; then
    error "VERSION file is empty"
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
for version in "${versions[@]}"; do
    if [[ "$version" == "$KEEP_VERSION" ]]; then
        continue
    fi

    echo "[INFO] Removing old version: $version"
    if sudo dkms remove -m "$MODULE_NAME" -v "$version" --all; then
        removed=$((removed + 1))
    else
        warn "Failed to remove $MODULE_NAME/$version"
    fi
done

info "Removed versions: $removed"
info "Final DKMS status"
dkms status | grep "$MODULE_NAME" || true

echo "[OK] Cleanup completed. Kept version: $KEEP_VERSION"
