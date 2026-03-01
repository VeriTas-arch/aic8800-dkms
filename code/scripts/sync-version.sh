#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="$REPO_ROOT/VERSION"
DKMS_CONF_FILE="$REPO_ROOT/src/AIC8800/drivers/aic8800/dkms.conf"
DEBIAN_CONTROL_FILE="$REPO_ROOT/src/DEBIAN/control"

usage() {
    echo "Usage: $0 <new-version>"
    echo "Example: $0 1.0.7"
}

if [[ $# -ne 1 ]]; then
    usage
    exit 1
fi

NEW_VERSION="$(printf '%s' "$1" | tr -d '[:space:]')"

if [[ ! "$NEW_VERSION" =~ ^[0-9]+(\.[0-9]+)*([.-][0-9A-Za-z]+)*$ ]]; then
    echo "[ERROR] Invalid version format: $NEW_VERSION" >&2
    echo "[ERROR] Expected something like: 1.0.7 or 1.0.7-rc1" >&2
    exit 1
fi

for file in "$VERSION_FILE" "$DKMS_CONF_FILE" "$DEBIAN_CONTROL_FILE"; do
    if [[ ! -f "$file" ]]; then
        echo "[ERROR] Missing file: $file" >&2
        exit 1
    fi
done

CURRENT_VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"

echo "[INFO] Current version: ${CURRENT_VERSION:-<empty>}"
echo "[INFO] Target  version: $NEW_VERSION"

printf '%s' "$NEW_VERSION" > "$VERSION_FILE"
sed -i -E "s/^PACKAGE_VERSION=\".*\"/PACKAGE_VERSION=\"$NEW_VERSION\"/" "$DKMS_CONF_FILE"
sed -i -E "s/^Version:.*/Version:$NEW_VERSION/" "$DEBIAN_CONTROL_FILE"

echo "[OK] Version synchronized across files:"
echo "  - $VERSION_FILE"
echo "  - $DKMS_CONF_FILE"
echo "  - $DEBIAN_CONTROL_FILE"

echo "[INFO] Verification:"
grep -E '^PACKAGE_VERSION=' "$DKMS_CONF_FILE"
grep -E '^Version:' "$DEBIAN_CONTROL_FILE"
printf 'VERSION=%s\n' "$(cat "$VERSION_FILE")"
