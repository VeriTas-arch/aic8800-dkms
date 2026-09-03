#!/usr/bin/env bash
set -euo pipefail

info() { echo "[INFO] $*"; }
error() { echo "[ERROR] $*" >&2; }

usage() {
    echo "Usage: $0 [--warnings|--extra-warnings] [--sparse] [--config NAME=VALUE] [kernel-version]"
    echo "Example: $0"
    echo "Example: $0 --warnings --config CONFIG_USB_RX_AGGR=y 6.8.0-138-generic"
    echo "  --warnings        make ordinary compiler warnings fatal (W=e)"
    echo "  --extra-warnings  enable advisory Kbuild warnings (W=1)"
}

require_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        error "required command not found: $1"
        exit 1
    fi
}

WARNINGS=0
EXTRA_WARNINGS=0
SPARSE=0
KERNEL_VER=""
CONFIG_OVERRIDES=()
BUILD_LOAD_FW=1
BUILD_WLAN=1
while (($#)); do
    case "$1" in
        --warnings)
            WARNINGS=1
            shift
            ;;
        --extra-warnings)
            EXTRA_WARNINGS=1
            shift
            ;;
        --sparse)
            SPARSE=1
            shift
            ;;
        --config)
            if [[ $# -lt 2 ]]; then
                error "--config requires NAME=VALUE"
                exit 2
            fi
            CONFIG_OVERRIDES+=("$2")
            shift 2
            ;;
        --config=*)
            CONFIG_OVERRIDES+=("${1#--config=}")
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

if ((WARNINGS && EXTRA_WARNINGS)); then
    error "--warnings and --extra-warnings are mutually exclusive"
    exit 2
fi

for override in "${CONFIG_OVERRIDES[@]}"; do
    if [[ ! "$override" =~ ^CONFIG_[A-Z0-9_]+=[A-Za-z0-9_./:+-]+$ ]]; then
        error "invalid configuration override: $override"
        exit 2
    fi
    config_name=${override%%=*}
    config_value=${override#*=}
    case "$config_name" in
        CONFIG_AIC_LOADFW_SUPPORT)
            case "$config_value" in
                n) BUILD_LOAD_FW=0 ;;
                y|m) BUILD_LOAD_FW=1 ;;
                *) error "invalid module setting: $override"; exit 2 ;;
            esac
            ;;
        CONFIG_AIC8800_WLAN_SUPPORT)
            case "$config_value" in
                n) BUILD_WLAN=0 ;;
                y|m) BUILD_WLAN=1 ;;
                *) error "invalid module setting: $override"; exit 2 ;;
            esac
            ;;
    esac
done
if ((BUILD_LOAD_FW == 0 && BUILD_WLAN == 0)); then
    error "configuration disables both expected modules"
    exit 2
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=lib/common.sh
source "$REPO_ROOT/scripts/lib/common.sh"
SOURCE_DIR="$REPO_ROOT/src/AIC8800/drivers/aic8800"
VERSION_FILE="$REPO_ROOT/VERSION"
KERNEL_VER="${KERNEL_VER:-$(uname -r)}"
KERNEL_BUILD_DIR="/lib/modules/$KERNEL_VER/build"
BUILD_PARENT="${TMPDIR:-/tmp}"
BUILD_ROOT=""

require_cmd make
require_cmd mktemp
require_cmd modinfo
require_cmd tar
if ((SPARSE)); then
    require_cmd sparse
fi

if [[ ! "$KERNEL_VER" =~ ^[0-9A-Za-z._+-]+$ ]]; then
    error "invalid kernel version: $KERNEL_VER"
    exit 2
fi

if [[ ! -d "$SOURCE_DIR" ]]; then
    error "source directory not found: $SOURCE_DIR"
    exit 1
fi
if [[ ! -f "$VERSION_FILE" ]]; then
    error "VERSION file not found: $VERSION_FILE"
    exit 1
fi
EXPECTED_VERSION="$(<"$VERSION_FILE")"

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
    info "Checks : ordinary compiler warnings are fatal (W=e)"
fi
if ((EXTRA_WARNINGS)); then
    info "Checks : advisory extra compiler warnings enabled (W=1)"
fi
if ((SPARSE)); then
    info "Checks : sparse semantic analysis enabled (C=1)"
fi
if ((${#CONFIG_OVERRIDES[@]})); then
    info "Config : ${CONFIG_OVERRIDES[*]}"
fi

aic_stage_driver_source "$SOURCE_DIR" "$BUILD_ROOT"

make_args=(
    -C "$BUILD_ROOT"
    "KDIR=$KERNEL_BUILD_DIR"
    "PWD=$BUILD_ROOT"
    "ARCH=${ARCH:-$(uname -m)}"
    "CROSS_COMPILE=${CROSS_COMPILE:-}"
)
make_args+=("${CONFIG_OVERRIDES[@]}")
if ((WARNINGS)); then
    make_args+=(W=e)
fi
if ((EXTRA_WARNINGS)); then
    make_args+=(W=1)
fi
if ((SPARSE)); then
    make_args+=(C=1 CHECK=sparse)
fi
make "${make_args[@]}"

MODULES=()
if ((BUILD_LOAD_FW)); then
    MODULES+=("$BUILD_ROOT/aic_load_fw/aic_load_fw.ko")
fi
if ((BUILD_WLAN)); then
    MODULES+=("$BUILD_ROOT/aic8800_fdrv/aic8800_fdrv.ko")
fi

for module in "${MODULES[@]}"; do
    if [[ ! -s "$module" ]]; then
        error "expected module was not produced: $module"
        exit 2
    fi
    info "Built: ${module#"$BUILD_ROOT/"}"
    vermagic=$(modinfo -F vermagic "$module")
    if [[ "$vermagic" != "$KERNEL_VER "* ]]; then
        error "unexpected vermagic for ${module##*/}: $vermagic"
        exit 2
    fi
    info "Vermagic: $vermagic"
    module_version=$(modinfo -F dkms_version "$module")
    if [[ "$module_version" != "$EXPECTED_VERSION" ]]; then
        error "unexpected embedded DKMS version for ${module##*/}: ${module_version:-<missing>}"
        exit 2
    fi
    info "DKMS version: $module_version"
done

echo "[OK] Compile-only test passed for kernel $KERNEL_VER"
