#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 [kernel-version]"
    echo "Example: $0"
    echo "Example: $0 6.8.0-90-generic"
}

info() { echo "[INFO] $*"; }
warn() { echo "[WARN] $*"; }
error() { echo "[ERROR] $*" >&2; }

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

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        error "required command not found: $1"
        exit 1
    fi
}

if [[ $# -gt 1 ]]; then
    usage
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_NAME="aic8800fdrv"
VERSION_FILE="$REPO_ROOT/VERSION"
SOURCE_DIR="$REPO_ROOT/src/AIC8800/drivers/aic8800"
FW_SRC_DIR="$REPO_ROOT/src/AIC8800/fw/aic8800DC"
FW_DST_DIR="/lib/firmware/aic8800DC"
KERNEL_VER="${1:-$(uname -r)}"
RULES_SRC="$REPO_ROOT/src/AIC8800/aic.rules"
RULES_DST="/etc/udev/rules.d/aic.rules"
BUILD_TEST="$REPO_ROOT/scripts/build-test.sh"

if [[ ! -f "$VERSION_FILE" ]]; then
    error "VERSION file not found: $VERSION_FILE"
    exit 1
fi

if [[ ! -d "$SOURCE_DIR" ]]; then
    error "source directory not found: $SOURCE_DIR"
    exit 1
fi

require_cmd dkms
require_cmd sha256sum
require_cmd sudo
require_cmd udevadm

VERSION="$(<"$VERSION_FILE")"
if [[ ! "$VERSION" =~ ^[0-9]+(\.[0-9]+)*([.-][0-9A-Za-z]+)*$ ]]; then
    error "invalid VERSION content: ${VERSION:-<empty>}"
    exit 1
fi

if [[ ! -d "/lib/modules/$KERNEL_VER/build" ]]; then
    error "kernel headers not found for $KERNEL_VER (/lib/modules/$KERNEL_VER/build)"
    exit 1
fi

DKMS_SRC_DIR="/usr/src/${MODULE_NAME}-${VERSION}"

info "Preflight compile for $KERNEL_VER"
"$BUILD_TEST" "$KERNEL_VER"

if [[ -f "$FW_SRC_DIR/SHA256SUMS" ]]; then
    info "Verify firmware checksums"
    (cd "$FW_SRC_DIR" && sha256sum -c SHA256SUMS)
else
    error "firmware checksum manifest not found: $FW_SRC_DIR/SHA256SUMS"
    exit 1
fi

info "Module: $MODULE_NAME"
info "Version: $VERSION"
info "Kernel : $KERNEL_VER"
info "Copy source to: $DKMS_SRC_DIR"

sudo rm -rf "$DKMS_SRC_DIR"
sudo mkdir -p "$DKMS_SRC_DIR"
sudo cp -a "$SOURCE_DIR/." "$DKMS_SRC_DIR/"
sudo sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$VERSION\"/" "$DKMS_SRC_DIR/dkms.conf"

info "Refresh DKMS state for target kernel"
sudo dkms remove -m "$MODULE_NAME" -v "$VERSION" -k "$KERNEL_VER" \
    >/dev/null 2>&1 || true
cleanup_legacy_module_dirs

if dkms status -m "$MODULE_NAME" -v "$VERSION" 2>/dev/null |
    grep -Fq "$MODULE_NAME/$VERSION"; then
    info "DKMS source is already registered for another kernel"
else
    info "dkms add"
    sudo dkms add -m "$MODULE_NAME" -v "$VERSION"
fi

info "dkms build"
sudo dkms build -m "$MODULE_NAME" -v "$VERSION" -k "$KERNEL_VER"

info "dkms install"
sudo dkms install -m "$MODULE_NAME" -v "$VERSION" -k "$KERNEL_VER"

info "Install firmware files"
if [[ -d "$FW_SRC_DIR" ]]; then
    sudo install -d -m 0755 "$FW_DST_DIR"
    sudo cp -a "$FW_SRC_DIR/." "$FW_DST_DIR/"
else
    warn "firmware source not found: $FW_SRC_DIR"
fi

info "Install udev rule for AIC MSC eject"
if [[ -f "$RULES_SRC" ]]; then
    sudo install -m 0644 "$RULES_SRC" "$RULES_DST"
    sudo udevadm control --reload
    info "udev rules reloaded; replug the AIC device to apply the rule"
else
    warn "udev rule source not found: $RULES_SRC"
fi

info "Remove old usb-storage quirk config if exists"
sudo rm -f /etc/modprobe.d/aic8800-usb-storage-quirks.conf

echo "[OK] DKMS install completed"
echo "[NEXT] Check status: dkms status | grep $MODULE_NAME"
echo "[NEXT] Replug dongle, then load: sudo modprobe aic_load_fw && sudo modprobe aic8800_fdrv"
