#!/usr/bin/env bash
set -euo pipefail

info() { echo "[INFO] $*"; }
error() { echo "[ERROR] $*" >&2; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="$REPO_ROOT/VERSION"
DKMS_CONF_FILE="$REPO_ROOT/src/AIC8800/drivers/aic8800/dkms.conf"
DEBIAN_CONTROL_FILE="$REPO_ROOT/src/DEBIAN/control"
DRIVER_VERSION_HEADER="$REPO_ROOT/src/AIC8800/drivers/aic8800/aic_dkms_version.h"

usage() {
    echo "Usage: $0 <new-version>"
    echo "       $0 --check"
    echo "Example: $0 1.0.7"
}

if [[ $# -ne 1 ]]; then
    usage
    exit 1
fi

MODE=write
if [[ "$1" == "--check" ]]; then
    MODE=check
    NEW_VERSION=""
else
    NEW_VERSION=$1
fi

if [[ "$MODE" == write && ! "$NEW_VERSION" =~ ^[0-9]+(\.[0-9]+)*([.-][0-9A-Za-z]+)*$ ]]; then
    error "invalid version format: $NEW_VERSION"
    error "expected something like: 1.0.7 or 1.0.7-rc1"
    exit 1
fi

for file in "$VERSION_FILE" "$DKMS_CONF_FILE" "$DEBIAN_CONTROL_FILE" \
            "$DRIVER_VERSION_HEADER"; do
    if [[ ! -f "$file" ]]; then
        error "missing file: $file"
        exit 1
    fi
done

CURRENT_VERSION="$(<"$VERSION_FILE")"
DKMS_VERSION="$(sed -n -E 's/^PACKAGE_VERSION="([^"]+)"$/\1/p' "$DKMS_CONF_FILE")"
DEBIAN_VERSION="$(sed -n -E 's/^Version:[[:space:]]+([^[:space:]]+)[[:space:]]*$/\1/p' "$DEBIAN_CONTROL_FILE")"
DRIVER_VERSION="$(sed -n -E 's/^#define AIC_DKMS_VERSION "([^"]+)"$/\1/p' "$DRIVER_VERSION_HEADER")"

if [[ ! "$CURRENT_VERSION" =~ ^[0-9]+(\.[0-9]+)*([.-][0-9A-Za-z]+)*$ ]]; then
    error "invalid VERSION content: ${CURRENT_VERSION:-<empty>}"
    exit 1
fi

if [[ "$MODE" == check ]]; then
    if [[ "$CURRENT_VERSION" != "$DKMS_VERSION" ||
          "$CURRENT_VERSION" != "$DEBIAN_VERSION" ||
          "$CURRENT_VERSION" != "$DRIVER_VERSION" ]]; then
        error "version mismatch: VERSION=$CURRENT_VERSION dkms.conf=${DKMS_VERSION:-<missing>} control=${DEBIAN_VERSION:-<missing>} driver=${DRIVER_VERSION:-<missing>}"
        exit 1
    fi
    echo "[OK] Version metadata is synchronized: $CURRENT_VERSION"
    exit 0
fi

info "Current version: ${CURRENT_VERSION:-<empty>}"
info "Target  version: $NEW_VERSION"

printf '%s\n' "$NEW_VERSION" > "$VERSION_FILE"
sed -i -E "s/^PACKAGE_VERSION=\".*\"/PACKAGE_VERSION=\"$NEW_VERSION\"/" "$DKMS_CONF_FILE"
sed -i -E "s/^Version:[[:space:]]*.*/Version: $NEW_VERSION/" "$DEBIAN_CONTROL_FILE"
sed -i -E "s/^(#define AIC_DKMS_VERSION) \".*\"$/\1 \"$NEW_VERSION\"/" "$DRIVER_VERSION_HEADER"

"$0" --check

echo "[OK] Version synchronized across files:"
echo "  - $VERSION_FILE"
echo "  - $DKMS_CONF_FILE"
echo "  - $DEBIAN_CONTROL_FILE"
echo "  - $DRIVER_VERSION_HEADER"

info "Verification:"
grep -E '^PACKAGE_VERSION=' "$DKMS_CONF_FILE"
grep -E '^Version:' "$DEBIAN_CONTROL_FILE"
grep -E '^#define AIC_DKMS_VERSION ' "$DRIVER_VERSION_HEADER"
printf 'VERSION=%s\n' "$(cat "$VERSION_FILE")"
