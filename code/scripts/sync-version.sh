#!/usr/bin/env bash
set -euo pipefail

info() { echo "[INFO] $*"; }
warn() { echo "[WARN] $*" >&2; }
error() { echo "[ERROR] $*" >&2; }

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION_FILE="$REPO_ROOT/VERSION"
DKMS_CONF_FILE="$REPO_ROOT/src/AIC8800/drivers/aic8800/dkms.conf"
DEBIAN_CONTROL_FILE="$REPO_ROOT/src/DEBIAN/control"
DRIVER_VERSION_HEADER="$REPO_ROOT/src/AIC8800/drivers/aic8800/aic_dkms_version.h"
VERSION_PATTERN='^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$'

usage() {
    echo "Usage: $0 <new-version>"
    echo "       $0 --force <new-version>"
    echo "       $0 --check"
    echo "Example: $0 1.1.10"
}

MODE=write
FORCE=0
NEW_VERSION=""

case $# in
    1)
        case "$1" in
            --check)
                MODE=check
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            --*)
                error "unknown option: $1"
                usage >&2
                exit 1
                ;;
            *)
                NEW_VERSION=$1
                ;;
        esac
        ;;
    2)
        if [[ "$1" != "--force" ]]; then
            error "expected --force before the target version"
            usage >&2
            exit 1
        fi
        FORCE=1
        NEW_VERSION=$2
        ;;
    *)
        usage >&2
        exit 1
        ;;
esac

is_valid_version() {
    [[ "${1:-}" =~ $VERSION_PATTERN ]]
}

if [[ "$MODE" == write ]] && ! is_valid_version "$NEW_VERSION"; then
    error "invalid version format: ${NEW_VERSION:-<empty>}"
    error "expected MAJOR.MINOR.PATCH with no leading zeroes, for example 1.1.10"
    exit 1
fi

for command in cp flock mkdir mktemp mv rm sed; do
    if ! command -v "$command" >/dev/null 2>&1; then
        error "required command not found: $command"
        exit 1
    fi
done

exec {VERSION_LOCK_FD}<"$0"
if [[ "$MODE" == check ]]; then
    if ! flock -s -n "$VERSION_LOCK_FD"; then
        error "another version update is in progress"
        exit 1
    fi
elif ! flock -n "$VERSION_LOCK_FD"; then
    error "another version check or update is in progress"
    exit 1
fi

for file in "$VERSION_FILE" "$DKMS_CONF_FILE" "$DEBIAN_CONTROL_FILE" \
            "$DRIVER_VERSION_HEADER"; do
    if [[ ! -f "$file" ]]; then
        error "missing file: $file"
        exit 1
    fi
done

read_plain_version() {
    local label=$1
    local file=$2
    local output_name=$3
    local -a lines=()

    mapfile -t lines <"$file"
    if [[ ${#lines[@]} -ne 1 ]]; then
        error "$label must contain exactly one version line: $file"
        return 1
    fi
    if ! is_valid_version "${lines[0]}"; then
        error "invalid $label version: ${lines[0]:-<empty>}"
        return 1
    fi
    printf -v "$output_name" '%s' "${lines[0]}"
}

read_pattern_version() {
    local label=$1
    local file=$2
    local expression=$3
    local output_name=$4
    local -a matches=()

    mapfile -t matches < <(sed -n -E "$expression" "$file")
    if [[ ${#matches[@]} -ne 1 ]]; then
        error "$label must contain exactly one recognizable version field: $file"
        return 1
    fi
    if ! is_valid_version "${matches[0]}"; then
        error "invalid $label version: ${matches[0]:-<empty>}"
        return 1
    fi
    printf -v "$output_name" '%s' "${matches[0]}"
}

load_metadata() {
    local version_file=$1
    local dkms_file=$2
    local control_file=$3
    local header_file=$4

    read_plain_version VERSION "$version_file" META_VERSION
    read_pattern_version dkms.conf "$dkms_file" \
        's/^PACKAGE_VERSION="([^"]+)"$/\1/p' META_DKMS_VERSION
    read_pattern_version DEBIAN/control "$control_file" \
        's/^Version:[[:space:]]+([^[:space:]]+)[[:space:]]*$/\1/p' \
        META_DEBIAN_VERSION
    read_pattern_version aic_dkms_version.h "$header_file" \
        's/^#define AIC_DKMS_VERSION "([^"]+)"$/\1/p' \
        META_DRIVER_VERSION
}

metadata_is_synchronized() {
    [[ "$META_VERSION" == "$META_DKMS_VERSION" &&
       "$META_VERSION" == "$META_DEBIAN_VERSION" &&
       "$META_VERSION" == "$META_DRIVER_VERSION" ]]
}

print_metadata() {
    warn "VERSION=$META_VERSION dkms.conf=$META_DKMS_VERSION control=$META_DEBIAN_VERSION driver=$META_DRIVER_VERSION"
}

version_compare() {
    local left=$1
    local right=$2
    local -a left_parts right_parts
    local index left_value right_value
    local LC_ALL=C

    IFS=. read -r -a left_parts <<<"$left"
    IFS=. read -r -a right_parts <<<"$right"
    for index in 0 1 2; do
        left_value=${left_parts[$index]}
        right_value=${right_parts[$index]}
        if ((${#left_value} < ${#right_value})) ||
           [[ ${#left_value} -eq ${#right_value} &&
              "$left_value" < "$right_value" ]]; then
            echo -1
            return
        fi
        if ((${#left_value} > ${#right_value})) ||
           [[ ${#left_value} -eq ${#right_value} &&
              "$left_value" > "$right_value" ]]; then
            echo 1
            return
        fi
    done
    echo 0
}

load_metadata "$VERSION_FILE" "$DKMS_CONF_FILE" "$DEBIAN_CONTROL_FILE" \
              "$DRIVER_VERSION_HEADER"

CURRENT_VERSION=$META_VERSION
METADATA_DRIFT=0
if ! metadata_is_synchronized; then
    METADATA_DRIFT=1
fi

if [[ "$MODE" == check ]]; then
    if ((METADATA_DRIFT)); then
        error "version metadata is not synchronized"
        print_metadata
        exit 1
    fi
    echo "[OK] Version metadata is synchronized: $CURRENT_VERSION"
    exit 0
fi

info "Current version: $CURRENT_VERSION"
info "Target  version: $NEW_VERSION"

if ((METADATA_DRIFT)); then
    warn "version metadata drift detected"
    print_metadata
    if ((!FORCE)); then
        error "refusing to overwrite inconsistent metadata; review it and rerun with --force $NEW_VERSION"
        exit 2
    fi
    warn "--force accepted; all metadata will be synchronized to $NEW_VERSION"
fi

COMPARISON=$(version_compare "$NEW_VERSION" "$CURRENT_VERSION")
if ((COMPARISON == 0 && !METADATA_DRIFT)); then
    echo "[OK] Version metadata is already synchronized at $CURRENT_VERSION; no files changed."
    exit 0
fi

if ((COMPARISON < 0)); then
    if ((!FORCE)); then
        warn "requested version $NEW_VERSION is lower than current version $CURRENT_VERSION"
        error "refusing version downgrade; rerun with --force $NEW_VERSION to proceed"
        exit 2
    fi
    warn "forcing version downgrade: $CURRENT_VERSION -> $NEW_VERSION"
elif ((COMPARISON > 0)); then
    info "Version upgrade: $CURRENT_VERSION -> $NEW_VERSION"
else
    info "Repair version metadata at $CURRENT_VERSION"
fi

FILES=(
    "$VERSION_FILE"
    "$DKMS_CONF_FILE"
    "$DEBIAN_CONTROL_FILE"
    "$DRIVER_VERSION_HEADER"
)
TRANSACTION_DIR=""
MUTATION_ACTIVE=0
ROLLBACK_RUNNING=0
KEEP_TRANSACTION=0
ORIGINALS=()
CANDIDATES=()

rollback_files() {
    local index restore_file
    local rollback_failed=0

    ROLLBACK_RUNNING=1
    warn "version update failed; restoring original metadata"
    for index in "${!FILES[@]}"; do
        restore_file="${FILES[$index]}.version-restore.$$"
        if ! cp -p -- "${ORIGINALS[$index]}" "$restore_file" ||
           ! mv -f -- "$restore_file" "${FILES[$index]}"; then
            rm -f -- "$restore_file"
            rollback_failed=1
        fi
    done
    if ((rollback_failed)); then
        error "automatic version rollback was incomplete; restore the four metadata files with git"
        return 1
    fi
    info "Original version metadata restored"
}

on_exit() {
    local status=$?

    trap - EXIT HUP INT TERM
    if ((status != 0 && MUTATION_ACTIVE && !ROLLBACK_RUNNING)); then
        if ! rollback_files; then
            status=1
            KEEP_TRANSACTION=1
            error "version backups kept at: $TRANSACTION_DIR/original"
        fi
    fi
    if [[ -n "$TRANSACTION_DIR" && -d "$TRANSACTION_DIR" &&
          "$TRANSACTION_DIR" == "$REPO_ROOT"/.version-sync.* ]] &&
       ((!KEEP_TRANSACTION)); then
        rm -rf -- "$TRANSACTION_DIR"
    fi
    exit "$status"
}

trap on_exit EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

TRANSACTION_DIR=$(mktemp -d "$REPO_ROOT/.version-sync.XXXXXX")
mkdir -p "$TRANSACTION_DIR/original" "$TRANSACTION_DIR/candidate"
for index in "${!FILES[@]}"; do
    ORIGINALS[$index]="$TRANSACTION_DIR/original/$index"
    CANDIDATES[$index]="$TRANSACTION_DIR/candidate/$index"
    cp -p -- "${FILES[$index]}" "${ORIGINALS[$index]}"
    cp -p -- "${FILES[$index]}" "${CANDIDATES[$index]}"
done

printf '%s\n' "$NEW_VERSION" >"${CANDIDATES[0]}"
sed -i -E "s/^PACKAGE_VERSION=\"[^\"]+\"$/PACKAGE_VERSION=\"$NEW_VERSION\"/" \
    "${CANDIDATES[1]}"
sed -i -E "s/^Version:[[:space:]]+[^[:space:]]+[[:space:]]*$/Version: $NEW_VERSION/" \
    "${CANDIDATES[2]}"
sed -i -E "s/^(#define AIC_DKMS_VERSION) \"[^\"]+\"$/\1 \"$NEW_VERSION\"/" \
    "${CANDIDATES[3]}"

load_metadata "${CANDIDATES[0]}" "${CANDIDATES[1]}" \
              "${CANDIDATES[2]}" "${CANDIDATES[3]}"
if ! metadata_is_synchronized || [[ "$META_VERSION" != "$NEW_VERSION" ]]; then
    error "candidate version metadata validation failed"
    print_metadata
    exit 1
fi

MUTATION_ACTIVE=1
for index in "${!FILES[@]}"; do
    mv -f -- "${CANDIDATES[$index]}" "${FILES[$index]}"
done

load_metadata "$VERSION_FILE" "$DKMS_CONF_FILE" "$DEBIAN_CONTROL_FILE" \
              "$DRIVER_VERSION_HEADER"
if ! metadata_is_synchronized || [[ "$META_VERSION" != "$NEW_VERSION" ]]; then
    error "installed version metadata validation failed"
    print_metadata
    exit 1
fi
MUTATION_ACTIVE=0

echo "[OK] Version synchronized safely: $CURRENT_VERSION -> $NEW_VERSION"
echo "  - $VERSION_FILE"
echo "  - $DKMS_CONF_FILE"
echo "  - $DEBIAN_CONTROL_FILE"
echo "  - $DRIVER_VERSION_HEADER"
