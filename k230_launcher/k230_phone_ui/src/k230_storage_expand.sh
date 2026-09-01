#!/bin/sh

set -u

PATH=/sbin:/bin:/usr/sbin:/usr/bin

STATUS_FILE=/tmp/k230_storage_expand.status
LOG_FILE=/tmp/k230_storage_expand.log
PENDING_FILE=/etc/k230_storage_expand_pending
MIN_EXPAND_GAP_MB=256

log()
{
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG_FILE"
}

read_rootdev()
{
    rootdev="$(awk '$2 == "/" { print $1; exit }' /proc/mounts 2>/dev/null || true)"

    if [ "${rootdev:-}" = "/dev/root" ] || [ -z "${rootdev:-}" ]; then
        cmd_root="$(sed -n 's/.*root=\([^ ]*\).*/\1/p' /proc/cmdline 2>/dev/null | head -n 1)"
        case "$cmd_root" in
        /dev/*)
            rootdev="$cmd_root"
            ;;
        *)
            if [ -b /dev/mmcblk1p2 ]; then
                rootdev=/dev/mmcblk1p2
            elif [ -b /dev/mmcblk0p2 ]; then
                rootdev=/dev/mmcblk0p2
            fi
            ;;
        esac
    fi

    if [ -n "${rootdev:-}" ]; then
        resolved="$(readlink -f "$rootdev" 2>/dev/null || true)"
        if [ -n "$resolved" ]; then
            rootdev="$resolved"
        fi
    fi

    [ -b "${rootdev:-}" ]
}

read_disk_part()
{
    root_base="$(basename "$rootdev")"

    case "$root_base" in
    mmcblk*p[0-9]*)
        partnum="${root_base##*p}"
        disk="${rootdev%p$partnum}"
        ;;
    *[0-9])
        partnum="$(printf '%s\n' "$root_base" | sed 's/.*[^0-9]\([0-9][0-9]*\)$/\1/')"
        disk="${rootdev%$partnum}"
        ;;
    *)
        return 1
        ;;
    esac

    disk_base="$(basename "$disk")"
    [ -b "$disk" ] && [ -n "$partnum" ]
}

sys_block_mb()
{
    block="$1"
    sectors="$(cat "/sys/class/block/$block/size" 2>/dev/null || printf 0)"
    case "$sectors" in
    ''|*[!0-9]*)
        sectors=0
        ;;
    esac
    printf '%s\n' "$((sectors / 2048))"
}

df_total_mb()
{
    df -Pm / 2>/dev/null | awk 'NR == 2 { print $2 + 0 }'
}

df_free_mb()
{
    df -Pm / 2>/dev/null | awk 'NR == 2 { print $4 + 0 }'
}

write_status()
{
    state="$1"
    message="$2"
    disk_mb=0
    part_mb=0
    fs_total_mb="$(df_total_mb)"
    fs_free_mb="$(df_free_mb)"

    if read_rootdev && read_disk_part; then
        disk_mb="$(sys_block_mb "$disk_base")"
        part_mb="$(sys_block_mb "$root_base")"
    fi

    {
        printf 'state=%s\n' "$state"
        printf 'message=%s\n' "$message"
        printf 'rootdev=%s\n' "${rootdev:-}"
        printf 'disk=%s\n' "${disk:-}"
        printf 'partnum=%s\n' "${partnum:-}"
        printf 'disk_mb=%s\n' "${disk_mb:-0}"
        printf 'part_mb=%s\n' "${part_mb:-0}"
        printf 'fs_total_mb=%s\n' "${fs_total_mb:-0}"
        printf 'fs_free_mb=%s\n' "${fs_free_mb:-0}"
    } > "${STATUS_FILE}.tmp"
    mv "${STATUS_FILE}.tmp" "$STATUS_FILE"
}

run_resize2fs()
{
    write_status resizing "Growing ext4 filesystem"
    log "resize2fs $rootdev"
    if resize2fs "$rootdev" >> "$LOG_FILE" 2>&1; then
        rm -f "$PENDING_FILE"
        sync
        write_status done "Storage expansion complete"
        return 0
    fi

    write_status failed "resize2fs failed"
    return 1
}

status_only()
{
    if ! read_rootdev || ! read_disk_part; then
        write_status failed "Root partition not detected"
        return 1
    fi

    disk_mb="$(sys_block_mb "$disk_base")"
    part_mb="$(sys_block_mb "$root_base")"
    gap_mb=$((disk_mb - part_mb))
    if [ "$gap_mb" -lt 0 ]; then
        gap_mb=0
    fi

    if [ "$gap_mb" -lt "$MIN_EXPAND_GAP_MB" ]; then
        rm -f "$PENDING_FILE"
        write_status already "Storage already uses the SD card"
    elif [ -f "$PENDING_FILE" ]; then
        write_status reboot_required "Reboot required to reload partition table"
    else
        write_status ready "Ready to expand storage"
    fi
}

finish_pending()
{
    if [ ! -f "$PENDING_FILE" ]; then
        status_only
        return $?
    fi

    log "finish pending resize"
    if ! read_rootdev || ! read_disk_part; then
        write_status failed "Root partition not detected"
        return 1
    fi

    run_resize2fs
}

expand_now()
{
    : > "$LOG_FILE"
    log "storage expand start"

    if ! read_rootdev || ! read_disk_part; then
        write_status failed "Root partition not detected"
        return 1
    fi

    disk_mb="$(sys_block_mb "$disk_base")"
    part_mb="$(sys_block_mb "$root_base")"
    gap_mb=$((disk_mb - part_mb))
    if [ "$gap_mb" -lt 0 ]; then
        gap_mb=0
    fi

    log "rootdev=$rootdev disk=$disk partnum=$partnum disk_mb=$disk_mb part_mb=$part_mb gap_mb=$gap_mb"

    if [ "$gap_mb" -lt "$MIN_EXPAND_GAP_MB" ]; then
        rm -f "$PENDING_FILE"
        write_status already "Storage already uses the SD card"
        return 0
    fi

    if ! command -v parted >/dev/null 2>&1; then
        write_status failed "parted is missing"
        return 1
    fi

    write_status partitioning "Expanding root partition"
    log "parted resizepart $disk $partnum 100%"

    if ! printf 'Fix\nYes\n' | parted ---pretend-input-tty "$disk" resizepart "$partnum" 100% >> "$LOG_FILE" 2>&1; then
        log "interactive parted failed, retry non-interactive"
        if ! parted -s "$disk" resizepart "$partnum" 100% >> "$LOG_FILE" 2>&1; then
            write_status failed "Partition resize failed"
            return 1
        fi
    fi

    sync
    partprobe "$disk" >> "$LOG_FILE" 2>&1 || true
    sleep 1

    new_part_mb="$(sys_block_mb "$root_base")"
    log "partition after partprobe part_mb=$new_part_mb"

    if [ "$new_part_mb" -le "$((part_mb + 16))" ]; then
        printf '%s\n' "$rootdev" > "$PENDING_FILE"
        sync
        write_status reboot_required "Reboot required to reload partition table"
        return 0
    fi

    run_resize2fs
}

case "${1:-status}" in
status)
    status_only
    ;;
expand)
    expand_now
    ;;
finish)
    finish_pending
    ;;
*)
    printf 'Usage: %s {status|expand|finish}\n' "$0" >&2
    exit 2
    ;;
esac
