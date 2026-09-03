#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_SCRIPT="$SCRIPT_DIR/sync-version.sh"
TEST_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/aic-version-test.XXXXXX")
FIXTURE_ROOT="$TEST_ROOT/fixture"
SYNC_SCRIPT="$FIXTURE_ROOT/code/scripts/sync-version.sh"
LOG_FILE="$TEST_ROOT/command.log"
TEST_COUNT=0

cleanup() {
    if [[ -d "$TEST_ROOT" && "$TEST_ROOT" == "${TMPDIR:-/tmp}"/aic-version-test.* ]]; then
        rm -rf -- "$TEST_ROOT"
    fi
}
trap cleanup EXIT

fail() {
    echo "[FAIL] $*" >&2
    if [[ -s "$LOG_FILE" ]]; then
        sed 's/^/  /' "$LOG_FILE" >&2
    fi
    exit 1
}

pass() {
    TEST_COUNT=$((TEST_COUNT + 1))
    echo "[PASS] $1"
}

write_metadata() {
    local version=$1

    printf '%s\n' "$version" >"$FIXTURE_ROOT/code/VERSION"
    printf 'PACKAGE_NAME="aic8800fdrv"\nPACKAGE_VERSION="%s"\n' \
        "$version" >"$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/dkms.conf"
    printf 'Package: aic8800fdrvpackage\nVersion: %s\nArchitecture: amd64\n' \
        "$version" >"$FIXTURE_ROOT/code/src/DEBIAN/control"
    printf '#ifndef _AIC_DKMS_VERSION_H_\n#define _AIC_DKMS_VERSION_H_\n\n#define AIC_DKMS_VERSION "%s"\n\n#endif\n' \
        "$version" >"$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/aic_dkms_version.h"
}

new_fixture() {
    local version=${1:-1.1.9}

    rm -rf -- "$FIXTURE_ROOT"
    mkdir -p "$FIXTURE_ROOT/code/scripts" \
        "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800" \
        "$FIXTURE_ROOT/code/src/DEBIAN"
    cp -p -- "$SOURCE_SCRIPT" "$SYNC_SCRIPT"
    chmod +x "$SYNC_SCRIPT"
    write_metadata "$version"
    : >"$LOG_FILE"
}

content_snapshot() {
    sha256sum \
        "$FIXTURE_ROOT/code/VERSION" \
        "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/dkms.conf" \
        "$FIXTURE_ROOT/code/src/DEBIAN/control" \
        "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/aic_dkms_version.h"
}

state_snapshot() {
    content_snapshot
    stat -c '%n|%i|%y' \
        "$FIXTURE_ROOT/code/VERSION" \
        "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/dkms.conf" \
        "$FIXTURE_ROOT/code/src/DEBIAN/control" \
        "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/aic_dkms_version.h"
}

expect_failure() {
    local expected_status=$1
    local description=$2
    local status
    shift 2

    set +e
    "$@" >"$LOG_FILE" 2>&1
    status=$?
    set -e
    if [[ $status -ne $expected_status ]]; then
        fail "$description returned $status, expected $expected_status"
    fi
}

assert_metadata() {
    local expected=$1
    local actual

    "$SYNC_SCRIPT" --check >"$LOG_FILE" 2>&1 ||
        fail "metadata check failed for $expected"
    actual=$(<"$FIXTURE_ROOT/code/VERSION")
    [[ "$actual" == "$expected" ]] ||
        fail "VERSION is $actual, expected $expected"
    grep -Fxq "PACKAGE_VERSION=\"$expected\"" \
        "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/dkms.conf" ||
        fail "dkms.conf is not synchronized to $expected"
    grep -Fxq "Version: $expected" \
        "$FIXTURE_ROOT/code/src/DEBIAN/control" ||
        fail "DEBIAN/control is not synchronized to $expected"
    grep -Fxq "#define AIC_DKMS_VERSION \"$expected\"" \
        "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/aic_dkms_version.h" ||
        fail "driver header is not synchronized to $expected"
}

new_fixture
before=$(state_snapshot)
"$SYNC_SCRIPT" 1.1.9 >"$LOG_FILE" 2>&1 || fail "same-version update failed"
after=$(state_snapshot)
[[ "$before" == "$after" ]] || fail "same-version update changed metadata files"
grep -Fq 'no files changed' "$LOG_FILE" || fail "same-version update did not report a no-op"
pass "same version is a true no-op"

new_fixture
"$SYNC_SCRIPT" 1.1.10 >"$LOG_FILE" 2>&1 || fail "upgrade failed"
assert_metadata 1.1.10
pass "higher version updates every metadata file"

new_fixture 1.9.0
large_version=1.100000000000000000000.0
"$SYNC_SCRIPT" "$large_version" >"$LOG_FILE" 2>&1 ||
    fail "large numeric version comparison failed"
assert_metadata "$large_version"
pass "numeric comparison does not depend on machine integer width"

new_fixture
before=$(state_snapshot)
expect_failure 2 "downgrade refusal" "$SYNC_SCRIPT" 1.1.8
after=$(state_snapshot)
[[ "$before" == "$after" ]] || fail "refused downgrade changed metadata files"
"$SYNC_SCRIPT" --force 1.1.8 >"$LOG_FILE" 2>&1 || fail "forced downgrade failed"
assert_metadata 1.1.8
pass "downgrade requires --force"

new_fixture
sed -i 's/AIC_DKMS_VERSION "1.1.9"/AIC_DKMS_VERSION "1.1.8"/' \
    "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/aic_dkms_version.h"
before=$(state_snapshot)
expect_failure 2 "metadata drift refusal" "$SYNC_SCRIPT" 1.1.9
after=$(state_snapshot)
[[ "$before" == "$after" ]] || fail "drift refusal changed metadata files"
"$SYNC_SCRIPT" --force 1.1.9 >"$LOG_FILE" 2>&1 || fail "forced drift repair failed"
assert_metadata 1.1.9
pass "metadata drift requires --force and can be repaired"

new_fixture
before=$(state_snapshot)
expect_failure 1 "leading-zero validation" "$SYNC_SCRIPT" 1.01.10
after=$(state_snapshot)
[[ "$before" == "$after" ]] || fail "invalid version changed metadata files"
expect_failure 1 "prerelease validation" "$SYNC_SCRIPT" 1.1.10-rc1
pass "invalid and ambiguous versions are rejected"

new_fixture
printf 'PACKAGE_VERSION="1.1.9"\n' >> \
    "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/dkms.conf"
before=$(content_snapshot)
expect_failure 1 "duplicate-field validation" "$SYNC_SCRIPT" --force 1.1.10
after=$(content_snapshot)
[[ "$before" == "$after" ]] || fail "duplicate field failure changed metadata files"
pass "duplicate metadata fields are rejected"

new_fixture
sed -i '/^Version:/d' "$FIXTURE_ROOT/code/src/DEBIAN/control"
before=$(content_snapshot)
expect_failure 1 "missing-field validation" "$SYNC_SCRIPT" --force 1.1.10
after=$(content_snapshot)
[[ "$before" == "$after" ]] || fail "missing field failure changed metadata files"

new_fixture
before=$(sha256sum \
    "$FIXTURE_ROOT/code/VERSION" \
    "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/dkms.conf" \
    "$FIXTURE_ROOT/code/src/DEBIAN/control")
rm -- "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/aic_dkms_version.h"
expect_failure 1 "missing-file validation" "$SYNC_SCRIPT" --force 1.1.10
after=$(sha256sum \
    "$FIXTURE_ROOT/code/VERSION" \
    "$FIXTURE_ROOT/code/src/AIC8800/drivers/aic8800/dkms.conf" \
    "$FIXTURE_ROOT/code/src/DEBIAN/control")
[[ "$before" == "$after" ]] || fail "missing file failure changed remaining metadata"
pass "missing metadata fields and files are rejected"

new_fixture
exec {test_lock_fd}<"$SYNC_SCRIPT"
flock -n "$test_lock_fd" || fail "could not acquire test version lock"
before=$(state_snapshot)
expect_failure 1 "concurrent-update validation" "$SYNC_SCRIPT" 1.1.10
after=$(state_snapshot)
[[ "$before" == "$after" ]] || fail "lock refusal changed metadata files"
flock -u "$test_lock_fd"
exec {test_lock_fd}>&-
pass "concurrent version updates are rejected"

new_fixture
fake_bin="$TEST_ROOT/fake-bin"
mkdir -p "$fake_bin"
cat >"$fake_bin/mv" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
count=0
if [[ -f "${SYNC_TEST_MV_COUNT_FILE:?}" ]]; then
    count=$(<"$SYNC_TEST_MV_COUNT_FILE")
fi
count=$((count + 1))
printf '%s\n' "$count" >"$SYNC_TEST_MV_COUNT_FILE"
if [[ $count -eq ${SYNC_TEST_FAIL_MV_AT:?} ]]; then
    exit 99
fi
exec "${SYNC_TEST_REAL_MV:?}" "$@"
EOF
chmod +x "$fake_bin/mv"
before=$(content_snapshot)
expect_failure 99 "transaction failure" env \
    PATH="$fake_bin:$PATH" \
    SYNC_TEST_MV_COUNT_FILE="$TEST_ROOT/mv-count" \
    SYNC_TEST_FAIL_MV_AT=3 \
    SYNC_TEST_REAL_MV="$(command -v mv)" \
    "$SYNC_SCRIPT" 1.1.10
after=$(content_snapshot)
[[ "$before" == "$after" ]] || fail "failed transaction did not restore original content"
assert_metadata 1.1.9
if compgen -G "$FIXTURE_ROOT/code/.version-sync.*" >/dev/null; then
    fail "failed transaction left a temporary directory"
fi
pass "mid-commit failure rolls back every metadata file"

echo "[OK] $TEST_COUNT version synchronization tests passed"
