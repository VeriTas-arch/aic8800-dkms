#!/usr/bin/env bash

aic_stage_driver_source() {
    local source_dir=$1
    local destination_dir=$2
    local extract_mode=${3:-direct}
    local generated
    local existing
    local -a excludes=(
        '--exclude=*.o'
        '--exclude=*.ko'
        '--exclude=*.mod'
        '--exclude=*.mod.c'
        '--exclude=*.mod.o'
        '--exclude=*.symvers'
        '--exclude=Module.symvers'
        '--exclude=modules.order'
        '--exclude=modules.builtin'
        '--exclude=modules.builtin.modinfo'
        '--exclude=Module.markers'
        '--exclude=.*.cmd'
        '--exclude=.tmp_versions'
        '--exclude=.cache.mk'
    )

    if [[ ! -d "$source_dir" || ! -d "$destination_dir" ]]; then
        printf '[ERROR] source staging directories are unavailable\n' >&2
        return 1
    fi

    existing=$(find "$destination_dir" -mindepth 1 -print -quit)
    if [[ -n "$existing" ]]; then
        printf '[ERROR] source staging destination is not empty: %s\n' \
            "$destination_dir" >&2
        return 1
    fi

    case "$extract_mode" in
        direct)
            tar -C "$source_dir" "${excludes[@]}" -cpf - . |
                tar -C "$destination_dir" --no-same-owner -xpf -
            ;;
        sudo)
            tar -C "$source_dir" "${excludes[@]}" -cpf - . |
                sudo tar -C "$destination_dir" --no-same-owner -xpf -
            ;;
        *)
            printf '[ERROR] invalid source staging mode: %s\n' \
                "$extract_mode" >&2
            return 2
            ;;
    esac

    generated=$(find "$destination_dir" \
        \( -type d -name '.tmp_versions' -o \
           -type f \( -name '*.o' -o -name '*.ko' -o \
                       -name '*.mod' -o -name '*.mod.c' -o \
                       -name '*.mod.o' -o -name '*.symvers' -o \
                       -name 'Module.symvers' -o -name 'modules.order' -o \
                       -name 'modules.builtin' -o \
                       -name 'modules.builtin.modinfo' -o \
                       -name 'Module.markers' -o -name '.*.cmd' -o \
                       -name '.cache.mk' \) \) \
        -print -quit)
    if [[ -n "$generated" ]]; then
        printf '[ERROR] generated Kbuild artifact reached source stage: %s\n' \
            "$generated" >&2
        return 1
    fi
}
