#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MODULE_NAME="aic8800fdrv"
VERSION_FILE="$REPO_ROOT/VERSION"
SOURCE_DIR="$REPO_ROOT/src/AIC8800/drivers/aic8800"
FW_SRC_DIR="$REPO_ROOT/src/AIC8800/fw/aic8800DC"
FW_DST_DIR="/lib/firmware/aic8800DC"
KERNEL_VER="${1:-$(uname -r)}"
RULES_SRC="$REPO_ROOT/src/AIC8800/aic.rules"
RULES_DST="/etc/udev/rules.d/aic.rules"

if [[ ! -f "$VERSION_FILE" ]]; then
    echo "[ERROR] VERSION file not found: $VERSION_FILE" >&2
    exit 1
fi

if [[ ! -d "$SOURCE_DIR" ]]; then
    echo "[ERROR] Source directory not found: $SOURCE_DIR" >&2
    exit 1
fi

if ! command -v dkms >/dev/null 2>&1; then
    echo "[ERROR] dkms is not installed. Please install dkms first." >&2
    exit 1
fi

VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
if [[ -z "$VERSION" ]]; then
    echo "[ERROR] VERSION file is empty." >&2
    exit 1
fi

DKMS_SRC_DIR="/usr/src/${MODULE_NAME}-${VERSION}"

echo "[INFO] Module: $MODULE_NAME"
echo "[INFO] Version: $VERSION"
echo "[INFO] Kernel : $KERNEL_VER"
echo "[INFO] Copy source to: $DKMS_SRC_DIR"

sudo rm -rf "$DKMS_SRC_DIR"
sudo mkdir -p "$DKMS_SRC_DIR"
sudo cp -a "$SOURCE_DIR/." "$DKMS_SRC_DIR/"
sudo sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$VERSION\"/" "$DKMS_SRC_DIR/dkms.conf"

echo "[INFO] Refresh old DKMS state if exists"
sudo dkms remove -m "$MODULE_NAME" -v "$VERSION" --all >/dev/null 2>&1 || true

echo "[INFO] dkms add"
sudo dkms add -m "$MODULE_NAME" -v "$VERSION"

echo "[INFO] dkms build"
sudo dkms build -m "$MODULE_NAME" -v "$VERSION" -k "$KERNEL_VER"

echo "[INFO] dkms install"
sudo dkms install -m "$MODULE_NAME" -v "$VERSION" -k "$KERNEL_VER"

echo "[INFO] Install firmware files"
if [[ -d "$FW_SRC_DIR" ]]; then
    sudo rm -rf "$FW_DST_DIR"
    sudo mkdir -p "$FW_DST_DIR"
    sudo cp -a "$FW_SRC_DIR/." "$FW_DST_DIR/"
else
    echo "[WARN] firmware source not found: $FW_SRC_DIR"
fi

echo "[INFO] Install udev rule for AIC MSC eject"
if [[ -f "$RULES_SRC" ]]; then
    sudo install -m 0644 "$RULES_SRC" "$RULES_DST"
    sudo udevadm control --reload
    sudo udevadm trigger
else
    echo "[WARN] udev rule source not found: $RULES_SRC"
fi

echo "[INFO] Remove old usb-storage quirk config if exists"
sudo rm -f /etc/modprobe.d/aic8800-usb-storage-quirks.conf

echo "[OK] DKMS install completed"
echo "[NEXT] Check status: dkms status | grep $MODULE_NAME"
echo "[NEXT] Replug dongle, then load: sudo modprobe aic_load_fw && sudo modprobe aic8800_fdrv"
