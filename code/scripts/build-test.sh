#!/usr/bin/env bash
set -euo pipefail

info() { echo "[INFO] $*"; }
error() { echo "[ERROR] $*" >&2; }

usage() {
    echo "Usage: $0 [--warnings] [kernel-version]"
    echo "Example: $0"
    echo "Example: $0 --warnings 6.8.0-138-generic"
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        error "required command not found: $1"
        exit 1
    fi
}

WARNINGS=0
KERNEL_VER=""
while (($#)); do
    case "$1" in
        --warnings)
            WARNINGS=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -* )
            error "unknown option: $1"
            usage >&2
            exit 2
            ;;
        *)
            if [[ -n "$KERNEL_VER" ]]; then
                error "only one kernel version may be specified"
                usage >&2
                exit 2
            fi
            KERNEL_VER=$1
            shift
            ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE_DIR="$REPO_ROOT/src/AIC8800/drivers/aic8800"
KERNEL_VER="${KERNEL_VER:-$(uname -r)}"
KERNEL_BUILD_DIR="/lib/modules/$KERNEL_VER/build"
BUILD_PARENT="${TMPDIR:-/tmp}"
BUILD_ROOT=""

require_cmd cp
require_cmd make
require_cmd mktemp

if [[ ! "$KERNEL_VER" =~ ^[0-9A-Za-z._+-]+$ ]]; then
    error "invalid kernel version: $KERNEL_VER"
    exit 2
fi

if [[ ! -d "$SOURCE_DIR" ]]; then
    error "source directory not found: $SOURCE_DIR"
    exit 1
fi

if [[ ! -d "$KERNEL_BUILD_DIR" ]]; then
    error "kernel headers not found: $KERNEL_BUILD_DIR"
    exit 1
fi

if [[ ! -d "$BUILD_PARENT" ]]; then
    error "temporary directory not found: $BUILD_PARENT"
    exit 1
fi
BUILD_PARENT="$(cd "$BUILD_PARENT" && pwd -P)"
BUILD_ROOT="$(mktemp -d "$BUILD_PARENT/aic8800-build-test.XXXXXX")"

cleanup() {
    if [[ -n "$BUILD_ROOT" && -d "$BUILD_ROOT" &&
          "$BUILD_ROOT" == "$BUILD_PARENT"/aic8800-build-test.* ]]; then
        rm -rf -- "$BUILD_ROOT"
    fi
}
trap cleanup EXIT

info "Source : $SOURCE_DIR"
info "Kernel : $KERNEL_VER"
info "Build  : $BUILD_ROOT"
info "Mode   : compile only (no DKMS, install, module load, or network changes)"
if ((WARNINGS)); then
    info "Checks : kernel extra warnings enabled (W=1)"
fi

cp -a "$SOURCE_DIR/." "$BUILD_ROOT/"

make_args=(
    -C "$BUILD_ROOT"
    "KDIR=$KERNEL_BUILD_DIR"
    "PWD=$BUILD_ROOT"
    "ARCH=${ARCH:-$(uname -m)}"
    "CROSS_COMPILE=${CROSS_COMPILE:-}"
)
if ((WARNINGS)); then
    make_args+=(W=1)
fi
make "${make_args[@]}"

MODULES=(
    "$BUILD_ROOT/aic_load_fw/aic_load_fw.ko"
    "$BUILD_ROOT/aic8800_fdrv/aic8800_fdrv.ko"
)

for module in "${MODULES[@]}"; do
    if [[ ! -s "$module" ]]; then
        error "expected module was not produced: $module"
        exit 2
    fi
    info "Built: ${module#"$BUILD_ROOT/"}"
    if command -v modinfo >/dev/null 2>&1; then
        vermagic=$(modinfo -F vermagic "$module")
        if [[ "$vermagic" != "$KERNEL_VER "* ]]; then
            error "unexpected vermagic for ${module##*/}: $vermagic"
            exit 2
        fi
        info "Vermagic: $vermagic"
    fi
done

echo "[OK] Compile-only test passed for kernel $KERNEL_VER"
