#!/usr/bin/env bash
set -euo pipefail

info() { echo "[INFO] $*"; }
error() { echo "[ERROR] $*" >&2; }

usage() {
    echo "Usage: $0 [firmware-directory [checksum-manifest]]"
}

if [[ $# -gt 2 ]]; then
    usage >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="${1:-$SCRIPT_DIR/../src/AIC8800/fw/aic8800DC}"
MANIFEST="${2:-$FIRMWARE_DIR/SHA256SUMS}"

for command in find sha256sum; do
    if ! command -v "$command" >/dev/null 2>&1; then
        error "required command not found: $command"
        exit 1
    fi
done

if [[ ! -d "$FIRMWARE_DIR" ]]; then
    error "firmware directory not found: $FIRMWARE_DIR"
    exit 1
fi
if [[ ! -f "$MANIFEST" || -L "$MANIFEST" ]]; then
    error "checksum manifest must be a regular, non-symlink file: $MANIFEST"
    exit 1
fi

FIRMWARE_DIR="$(cd "$FIRMWARE_DIR" && pwd -P)"
MANIFEST_DIR="$(cd "$(dirname "$MANIFEST")" && pwd -P)"
MANIFEST="$MANIFEST_DIR/$(basename "$MANIFEST")"

listed=()
manifest_lists() {
    local expected=$1
    local listed_name

    for listed_name in "${listed[@]}"; do
        if [[ "$listed_name" == "$expected" ]]; then
            return 0
        fi
    done
    return 1
}

line_no=0
while IFS= read -r line || [[ -n "$line" ]]; do
    ((line_no += 1))
    if [[ ! "$line" =~ ^([[:xdigit:]]{64})[[:space:]][[:space:]]([A-Za-z0-9._+-]+)$ ]]; then
        error "invalid checksum manifest line $line_no"
        exit 1
    fi
    filename=${BASH_REMATCH[2]}
    if [[ "$filename" == "SHA256SUMS" ]]; then
        error "manifest must not list itself"
        exit 1
    fi
    if manifest_lists "$filename"; then
        error "duplicate manifest entry: $filename"
        exit 1
    fi
    if [[ ! -f "$FIRMWARE_DIR/$filename" || -L "$FIRMWARE_DIR/$filename" ]]; then
        error "listed firmware is missing or not a regular file: $filename"
        exit 1
    fi
    listed+=("$filename")
done <"$MANIFEST"

if ((line_no == 0)); then
    error "checksum manifest is empty: $MANIFEST"
    exit 1
fi

while IFS= read -r -d '' entry; do
    filename=${entry##*/}
    if [[ "$filename" == "SHA256SUMS" ]]; then
        if [[ -L "$entry" || ! -f "$entry" ]]; then
            error "firmware manifest entry is not a regular file: $entry"
            exit 1
        fi
        continue
    fi
    if [[ -L "$entry" || ! -f "$entry" ]]; then
        error "unexpected non-regular firmware entry: $entry"
        exit 1
    fi
    if ! manifest_lists "$filename"; then
        error "firmware file is not covered by the manifest: $filename"
        exit 1
    fi
done < <(find "$FIRMWARE_DIR" -mindepth 1 -maxdepth 1 -print0)

info "Verify firmware checksums and exact manifest coverage"
(cd "$FIRMWARE_DIR" && sha256sum --check --strict "$MANIFEST")
echo "[OK] Firmware manifest covers every file exactly once."
