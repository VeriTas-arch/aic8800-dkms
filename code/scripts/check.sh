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

if command -v shellcheck >/dev/null 2>&1; then
    info "Check shell scripts with shellcheck"
    mapfile -d '' -t shell_scripts < <(
        find "$SCRIPT_DIR" -maxdepth 1 -type f -name '*.sh' -print0
    )
    shellcheck "${shell_scripts[@]}"
else
    info "Skip shellcheck (not installed)"
fi

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

newest_kernel="${kernels[${#kernels[@]} - 1]}"

info "Strict warning build: $newest_kernel"
"$SCRIPT_DIR/build-test.sh" --warnings "$newest_kernel"

declare -a variant_names=(
    usb-rx-aggregate
    usb-rx-tasklet
    usb-tx-aggregate
    usb-preallocated-rx
    usb-no-dedicated-message-endpoint
)
declare -a variant_configs=(
    "CONFIG_USB_RX_AGGR=y"
    "CONFIG_RX_TASKLET=y"
    "CONFIG_USB_TX_AGGR=y"
    "CONFIG_PREALLOC_RX_SKB=y"
    "CONFIG_USB_MSG_IN_EP=n"
)

for i in "${!variant_names[@]}"; do
    read -r -a overrides <<<"${variant_configs[$i]}"
    args=(--warnings)
    for override in "${overrides[@]}"; do
        args+=(--config "$override")
    done
    info "Compile optional path: ${variant_names[$i]}"
    "$SCRIPT_DIR/build-test.sh" "${args[@]}" "$newest_kernel"
done

if command -v sparse >/dev/null 2>&1; then
    info "Sparse analysis: $newest_kernel"
    "$SCRIPT_DIR/build-test.sh" --sparse "$newest_kernel"
else
    info "Skip sparse analysis (not installed)"
fi

echo "[OK] Repository checks and compile-only matrix passed"
