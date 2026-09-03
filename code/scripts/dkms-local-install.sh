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

dkms_registered() {
    dkms status -m "$MODULE_NAME" -v "$VERSION" 2>/dev/null |
        grep -Fq "$MODULE_NAME/$VERSION"
}

rollback_dkms() {
    local rollback_failed=0

    ROLLBACK_RUNNING=1
    set +e
    warn "Install failed; restoring previous DKMS source and state"
    if dkms_registered; then
        sudo dkms remove -m "$MODULE_NAME" -v "$VERSION" --all ||
            rollback_failed=1
    fi
    sudo rm -rf -- "$DKMS_SRC_DIR" || rollback_failed=1
    if ((HAD_DKMS_SOURCE)); then
        sudo mkdir -p "$DKMS_SRC_DIR" || rollback_failed=1
        sudo tar -C "$DKMS_SRC_DIR" -xpf "$DKMS_BACKUP" ||
            rollback_failed=1
    fi

    if ((HAD_DKMS_REGISTRATION)); then
        sudo dkms add -m "$MODULE_NAME" -v "$VERSION" ||
            rollback_failed=1
        if [[ "$PREVIOUS_DKMS_STATE" == built ||
              "$PREVIOUS_DKMS_STATE" == installed ]]; then
            sudo dkms build -m "$MODULE_NAME" -v "$VERSION" \
                -k "$KERNEL_VER" || rollback_failed=1
        fi
        if [[ "$PREVIOUS_DKMS_STATE" == installed ]]; then
            sudo dkms install -m "$MODULE_NAME" -v "$VERSION" \
                -k "$KERNEL_VER" --force || rollback_failed=1
        fi
    fi

    if ((rollback_failed)); then
        error "automatic DKMS rollback was incomplete; inspect 'dkms status'"
    else
        info "Previous DKMS state restored"
    fi
    set -e
}

on_exit() {
    local status=$?

    trap - EXIT
    if ((status != 0 && MUTATION_ACTIVE && !ROLLBACK_RUNNING)); then
        rollback_dkms
    fi
    if [[ -n "${BACKUP_ROOT:-}" && -d "$BACKUP_ROOT" &&
          "$BACKUP_ROOT" == "$BACKUP_PARENT"/aic-dkms-install.* ]]; then
        rm -rf -- "$BACKUP_ROOT"
    fi
    exit "$status"
}

if [[ $# -gt 1 ]]; then
    usage
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
source "$REPO_ROOT/scripts/lib/common.sh"
MODULE_NAME="aic8800fdrv"
VERSION_FILE="$REPO_ROOT/VERSION"
SOURCE_DIR="$REPO_ROOT/src/AIC8800/drivers/aic8800"
FW_SRC_DIR="$REPO_ROOT/src/AIC8800/fw/aic8800DC"
FW_DST_DIR="/lib/firmware/aic8800DC"
KERNEL_VER="${1:-$(uname -r)}"
RULES_SRC="$REPO_ROOT/src/AIC8800/aic.rules"
RULES_DST="/etc/udev/rules.d/aic.rules"
BUILD_TEST="$REPO_ROOT/scripts/build-test.sh"
VERSION_CHECK="$REPO_ROOT/scripts/sync-version.sh"
FIRMWARE_CHECK="$REPO_ROOT/scripts/verify-firmware.sh"

if [[ ! -f "$VERSION_FILE" ]]; then
    error "VERSION file not found: $VERSION_FILE"
    exit 1
fi

if [[ ! -d "$SOURCE_DIR" ]]; then
    error "source directory not found: $SOURCE_DIR"
    exit 1
fi

require_cmd dkms
require_cmd grep
require_cmd mktemp
require_cmd modinfo
require_cmd sha256sum
require_cmd sudo
require_cmd tar
require_cmd udevadm

VERSION="$(<"$VERSION_FILE")"
if [[ ! "$VERSION" =~ ^[0-9]+(\.[0-9]+)*([.-][0-9A-Za-z]+)*$ ]]; then
    error "invalid VERSION content: ${VERSION:-<empty>}"
    exit 1
fi

if [[ ! "$KERNEL_VER" =~ ^[0-9A-Za-z._+-]+$ ]]; then
    error "invalid kernel version: $KERNEL_VER"
    exit 2
fi
if [[ ! -d "/lib/modules/$KERNEL_VER/build" ]]; then
    error "kernel headers not found for $KERNEL_VER (/lib/modules/$KERNEL_VER/build)"
    exit 1
fi

DKMS_SRC_DIR="/usr/src/${MODULE_NAME}-${VERSION}"
if [[ ! "$DKMS_SRC_DIR" =~ ^/usr/src/aic8800fdrv-[0-9A-Za-z.-]+$ ]]; then
    error "unsafe DKMS source path: $DKMS_SRC_DIR"
    exit 1
fi

info "Check version metadata"
"$VERSION_CHECK" --check

mapfile -t existing_status < <(
    dkms status -m "$MODULE_NAME" -v "$VERSION" 2>/dev/null
)
TARGET_REGISTERED=0
HAD_DKMS_REGISTRATION=0
PREVIOUS_DKMS_STATE=added
if ((${#existing_status[@]})); then
    HAD_DKMS_REGISTRATION=1
fi
for status_line in "${existing_status[@]}"; do
    if [[ "$status_line" == "$MODULE_NAME/$VERSION, "* ]]; then
        status_tail=${status_line#"$MODULE_NAME/$VERSION, "}
        registered_kernel=${status_tail%%,*}
        if [[ "$registered_kernel" == "$KERNEL_VER" ]]; then
            TARGET_REGISTERED=1
            PREVIOUS_DKMS_STATE=${status_line##*: }
        else
            error "$MODULE_NAME/$VERSION is also registered for $registered_kernel"
            error "use dkms-refresh-all-kernels.sh so one source version cannot diverge between kernels"
            exit 2
        fi
    fi
done

info "Preflight compile for $KERNEL_VER"
"$BUILD_TEST" "$KERNEL_VER"

if [[ -f "$FW_SRC_DIR/SHA256SUMS" ]]; then
    info "Verify firmware checksums and manifest coverage"
    "$FIRMWARE_CHECK" "$FW_SRC_DIR"
else
    error "firmware checksum manifest not found: $FW_SRC_DIR/SHA256SUMS"
    exit 1
fi

BACKUP_PARENT="${TMPDIR:-/tmp}"
if [[ ! -d "$BACKUP_PARENT" ]]; then
    error "temporary directory not found: $BACKUP_PARENT"
    exit 1
fi
BACKUP_PARENT="$(cd "$BACKUP_PARENT" && pwd -P)"
BACKUP_ROOT="$(mktemp -d "$BACKUP_PARENT/aic-dkms-install.XXXXXX")"
DKMS_BACKUP="$BACKUP_ROOT/source.tar"
HAD_DKMS_SOURCE=0
MUTATION_ACTIVE=0
ROLLBACK_RUNNING=0
trap on_exit EXIT

if [[ -d "$DKMS_SRC_DIR" ]]; then
    HAD_DKMS_SOURCE=1
    info "Back up current DKMS source"
    sudo tar -C "$DKMS_SRC_DIR" -cpf - . >"$DKMS_BACKUP"
fi

info "Module: $MODULE_NAME"
info "Version: $VERSION"
info "Kernel : $KERNEL_VER"
info "Copy source to: $DKMS_SRC_DIR"

MUTATION_ACTIVE=1
sudo rm -rf -- "$DKMS_SRC_DIR"
sudo mkdir -p "$DKMS_SRC_DIR"
aic_stage_driver_source "$SOURCE_DIR" "$DKMS_SRC_DIR" sudo
sudo sed -i "s/^PACKAGE_VERSION=.*/PACKAGE_VERSION=\"$VERSION\"/" "$DKMS_SRC_DIR/dkms.conf"

info "Refresh DKMS state for target kernel"
if ((TARGET_REGISTERED)); then
    sudo dkms remove -m "$MODULE_NAME" -v "$VERSION" -k "$KERNEL_VER"
else
    info "No existing DKMS build for $MODULE_NAME/$VERSION on $KERNEL_VER"
fi
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

info "Verify installed modules"
status="$(dkms status -m "$MODULE_NAME" -v "$VERSION" -k "$KERNEL_VER")"
if [[ "$status" != *": installed" ]]; then
    error "DKMS status is not installed for $KERNEL_VER: ${status:-<empty>}"
    exit 2
fi
for module in aic8800_fdrv aic_load_fw; do
    module_path="$(modinfo -k "$KERNEL_VER" -n "$module")"
    vermagic="$(modinfo -k "$KERNEL_VER" -F vermagic "$module")"
    module_version="$(modinfo -k "$KERNEL_VER" -F dkms_version "$module")"
    if [[ "$module_path" != "/lib/modules/$KERNEL_VER/updates/dkms/"* ]]; then
        error "unexpected module path for $module: $module_path"
        exit 2
    fi
    if [[ "$vermagic" != "$KERNEL_VER "* ]]; then
        error "unexpected vermagic for $module: $vermagic"
        exit 2
    fi
    if [[ "$module_version" != "$VERSION" ]]; then
        error "unexpected DKMS version for $module: ${module_version:-<missing>}"
        exit 2
    fi
done

info "Install firmware files"
if [[ -d "$FW_SRC_DIR" ]]; then
    sudo install -d -m 0755 "$FW_DST_DIR"
    sudo cp -a "$FW_SRC_DIR/." "$FW_DST_DIR/"
    "$FIRMWARE_CHECK" "$FW_DST_DIR" "$FW_SRC_DIR/SHA256SUMS"
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

MUTATION_ACTIVE=0
echo "[OK] DKMS install completed"
echo "[NEXT] Check status: dkms status | grep $MODULE_NAME"
echo "[NEXT] Replug dongle, then load: sudo modprobe aic_load_fw && sudo modprobe aic8800_fdrv"
