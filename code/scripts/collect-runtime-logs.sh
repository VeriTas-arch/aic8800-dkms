#!/usr/bin/env bash

set -u

minutes=30
redact=1
output="${TMPDIR:-/tmp}/aic8800-runtime-$(date +%Y%m%d-%H%M%S).log"

usage() {
    cat <<'EOF'
Usage: collect-runtime-logs.sh [--minutes N] [--output FILE] [--no-redact]

Collect read-only AIC8800, USB, network, DKMS, and journal diagnostics.
MAC addresses, SSIDs, IPv4 addresses, and MAC-derived interface names are
redacted by default. --no-redact may expose network identifiers.
EOF
}

while (($#)); do
    case "$1" in
        --minutes)
            [[ $# -ge 2 ]] || { echo "missing value for --minutes" >&2; exit 2; }
            minutes=$2
            shift 2
            ;;
        --output)
            [[ $# -ge 2 ]] || { echo "missing value for --output" >&2; exit 2; }
            output=$2
            shift 2
            ;;
        --no-redact)
            redact=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

[[ $minutes =~ ^[1-9][0-9]*$ ]] || {
    echo "--minutes must be a positive integer" >&2
    exit 2
}

if [[ -e $output ]]; then
    echo "refusing to overwrite existing output: $output" >&2
    exit 1
fi

output_dir=$(dirname -- "$output")
mkdir -p -- "$output_dir"
tmp_output=$(mktemp --tmpdir="$output_dir" .aic8800-runtime.XXXXXX)
trap 'rm -f -- "$tmp_output"' EXIT

redact_stream() {
    if ((redact == 0)); then
        cat
        return
    fi

    sed -E \
        -e 's/([[:xdigit:]]{2}:){5}[[:xdigit:]]{2}/<MAC>/g' \
        -e 's/([[:xdigit:]]{2}[[:space:]]){5}[[:xdigit:]]{2}/<MAC>/g' \
        -e 's/wlx[[:xdigit:]]{12}/wlx<MAC>/g' \
        -e 's/^([[:space:]]*[Ss][Ss][Ii][Dd]:).*/\1 <SSID>/' \
        -e 's/(connect to )[^(]*/\1<SSID>/g' \
        -e 's/([0-9]{1,3}\.){3}[0-9]{1,3}/<IPv4>/g'
}

section() {
    local title=$1
    shift
    printf '\n===== %s =====\n' "$title"
    "$@" 2>&1 || printf '[unavailable or failed: exit=%d]\n' "$?"
}

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(git -C "$script_dir" rev-parse --show-toplevel 2>/dev/null || true)

{
    printf 'aic8800_runtime_diagnostics_version=1\n'
    printf 'collected_at=%s\n' "$(date --iso-8601=seconds)"
    printf 'window_minutes=%s\n' "$minutes"
    printf 'redacted=%s\n' "$redact"

    section "host" sh -c 'hostname; uname -a; uptime; printf "boot_id="; cat /proc/sys/kernel/random/boot_id'

    if [[ -n $repo_root ]]; then
        printf '\n===== source repository =====\n'
        git -C "$repo_root" rev-parse HEAD 2>&1 || true
        git -C "$repo_root" status --short --branch 2>&1 || true
    fi

    section "loaded modules" sh -c 'lsmod | grep -E "^(aic8800|aic_load_fw)" || true'
    section "DKMS status" sh -c 'dkms status -m aic8800fdrv 2>/dev/null || dkms status 2>/dev/null | grep -i aic || true'

    printf '\n===== module metadata and log masks =====\n'
    for module in aic8800_fdrv aic_load_fw; do
        printf -- '--- %s ---\n' "$module"
        modinfo "$module" 2>&1 | grep -E '^(filename|version|srcversion|vermagic|signer|description):' || true
        param="/sys/module/$module/parameters/aicwf_dbg_level"
        if [[ -r $param ]]; then
            printf 'aicwf_dbg_level=%s\n' "$(<"$param")"
        else
            printf 'aicwf_dbg_level=[not readable]\n'
        fi
    done

    section "USB topology" lsusb -t
    section "network links" ip -s link
    section "wireless devices" iw dev

    if command -v iw >/dev/null 2>&1; then
        while IFS= read -r iface; do
            [[ -n $iface ]] || continue
            section "wireless link $iface" iw dev "$iface" link
        done < <(iw dev 2>/dev/null | awk '$1 == "Interface" {print $2}')
    fi

    printf '\n===== driver runtime counters =====\n'
    found_stats=0
    while IFS= read -r stats_file; do
        [[ -n $stats_file ]] || continue
        found_stats=1
        printf -- '--- %s ---\n' "$stats_file"
        cat "$stats_file" 2>&1 || true
    done < <(find /sys/kernel/debug/ieee80211 -type f \
        -path '*/aic8800*/diags/runtime_stats' -readable 2>/dev/null)
    ((found_stats)) || printf '[runtime_stats unavailable; debugfs may be unmounted or unreadable]\n'

    printf '\n===== kernel journal =====\n'
    if command -v journalctl >/dev/null 2>&1; then
        journalctl -k -b --since "-$minutes min" --no-pager -o short-precise 2>&1 |
            grep -Ei 'AICWFDBG|aic8800|aic_load_fw|usb|xhci|cfg80211|wlan|wlx' || true
    else
        printf '[journalctl unavailable]\n'
    fi

    printf '\n===== NetworkManager journal =====\n'
    if command -v journalctl >/dev/null 2>&1; then
        journalctl -u NetworkManager -b --since "-$minutes min" \
            --no-pager -o short-precise 2>&1 || true
    else
        printf '[journalctl unavailable]\n'
    fi
} | redact_stream >"$tmp_output"

chmod 0600 "$tmp_output"
mv -- "$tmp_output" "$output"
trap - EXIT
printf 'Wrote %s\n' "$output"
