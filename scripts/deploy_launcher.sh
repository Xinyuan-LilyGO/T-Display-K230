#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

SDK_DIR="${SDK_DIR:-${REPO_DIR}/k230_linux_sdk}"
LAUNCHER_DIR="${LAUNCHER_DIR:-${REPO_DIR}/k230_launcher}"
CONF="${CONF:-k230_canmv_t_display_rm69a10_defconfig}"
TARGET_HOST="${K230_TARGET:-}"
REMOTE_APP_DIR="${REMOTE_APP_DIR:-/root/app/k230_phone_ui}"
REMOTE_FACE_DIR="${REMOTE_FACE_DIR:-/root/app/face_detect}"
REMOTE_BOOT="${REMOTE_BOOT:-/boot}"
REMOTE_MUSIC_DIR="${REMOTE_MUSIC_DIR:-/root/music}"
REMOTE_VIDEO_DIR="${REMOTE_VIDEO_DIR:-/root/videos}"
REMOTE_NOTIFICATION_DIR="${REMOTE_NOTIFICATION_DIR:-/root/notification}"
REMOTE_MAP_DIR="${REMOTE_MAP_DIR:-/root/maps}"
LABEL="${LABEL:-launcher}"
CONNECT_TIMEOUT="${CONNECT_TIMEOUT:-8}"
SSH_KEY="${SSH_KEY:-}"
SSH_PORT="${SSH_PORT:-}"

DO_BUILD=1
DEPLOY_APP=1
DEPLOY_FIRMWARE=0
DEPLOY_MEDIA=0
DEPLOY_MAPS=0
DEPLOY_RUNTIME_LIBS=0
RESTART_APP=1
DO_REBOOT=0
DRY_RUN=0
SET_AUDIO_OUTPUT=""

usage() {
    cat <<'USAGE'
Usage:
  deploy_launcher.sh [root@]<ip> [options]

Default behavior:
  - sync k230_launcher into the SDK
  - rebuild k230_phone_ui
  - deploy /root/app/k230_phone_ui and /root/app/face_detect
  - restart the launcher, without rebooting
  - do not update /boot unless --firmware or --full is used

Options:
  --sdk-dir DIR              SDK directory. Default: ./k230_linux_sdk
  --launcher-dir DIR         Launcher directory. Default: ./k230_launcher
  --conf NAME                SDK defconfig/output name.
                             Default: k230_canmv_t_display_rm69a10_defconfig
  --build                    Build before deployment. This is the default.
  --no-build                 Deploy existing SDK output without rebuilding.
  --app-only                 Deploy only launcher app files. This is the default.
  --firmware                 Also deploy /boot/Image and RM69A10 DTB.
  --firmware-only            Deploy only /boot/Image and RM69A10 DTB.
  --full                     Deploy app, face model, firmware, and media resources.
  --media                    Also deploy launcher resources/music, resources/videos, and resources/notification.
  --maps                     Also deploy launcher resources/maps to /root/maps.
                             Map tiles are not part of --media or images by default.
  --runtime-libs             Also update launcher runtime libraries in /usr/lib.
                             Use this only when the target image lacks required libraries.
  --no-restart              Do not restart k230_phone_ui after app deployment.
  --reboot                   Reboot after deployment.
  --no-reboot                Do not reboot after deployment. This is the default.
  --audio-output MODE        Persist launcher audio output: external|headphones.
  --label NAME               Remote backup label. Default: launcher.
  --ssh-key FILE             SSH private key.
  --ssh-port PORT            SSH port.
  --connect-timeout SEC      SSH connect timeout. Default: 8.
  --dry-run                  Print what would be done.
  -h, --help                 Show this help.

Environment:
  K230_TARGET=root@192.168.x.x
  SDK_DIR=/path/to/k230_linux_sdk
  LAUNCHER_DIR=/path/to/k230_launcher
  CONF=k230_canmv_t_display_rm69a10_defconfig

Examples:
  ./deploy_launcher.sh 192.168.36.232
  ./deploy_launcher.sh 192.168.36.232 --no-build
  ./deploy_launcher.sh 192.168.36.232 --full --reboot
USAGE
}

die() {
    echo "error: $*" >&2
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "missing command: $1"
}

run() {
    echo "+ $*"
    if [[ "${DRY_RUN}" -eq 0 ]]; then
        "$@"
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sdk-dir)
            [[ $# -ge 2 ]] || die "--sdk-dir requires a value"
            SDK_DIR="$2"
            shift 2
            ;;
        --launcher-dir)
            [[ $# -ge 2 ]] || die "--launcher-dir requires a value"
            LAUNCHER_DIR="$2"
            shift 2
            ;;
        --conf)
            [[ $# -ge 2 ]] || die "--conf requires a value"
            CONF="$2"
            shift 2
            ;;
        --build)
            DO_BUILD=1
            shift
            ;;
        --no-build)
            DO_BUILD=0
            shift
            ;;
        --app-only)
            DEPLOY_APP=1
            DEPLOY_FIRMWARE=0
            shift
            ;;
        --firmware)
            DEPLOY_FIRMWARE=1
            shift
            ;;
        --firmware-only)
            DEPLOY_APP=0
            DEPLOY_FIRMWARE=1
            RESTART_APP=0
            shift
            ;;
        --full)
            DEPLOY_APP=1
            DEPLOY_FIRMWARE=1
            DEPLOY_MEDIA=1
            shift
            ;;
        --media)
            DEPLOY_MEDIA=1
            shift
            ;;
        --maps)
            DEPLOY_MAPS=1
            shift
            ;;
        --runtime-libs|--with-runtime-libs)
            DEPLOY_RUNTIME_LIBS=1
            shift
            ;;
        --no-restart)
            RESTART_APP=0
            shift
            ;;
        --reboot)
            DO_REBOOT=1
            shift
            ;;
        --no-reboot)
            DO_REBOOT=0
            shift
            ;;
        --audio-output)
            [[ $# -ge 2 ]] || die "--audio-output requires external or headphones"
            SET_AUDIO_OUTPUT="$2"
            shift 2
            ;;
        --label)
            [[ $# -ge 2 ]] || die "--label requires a value"
            LABEL="$2"
            shift 2
            ;;
        --ssh-key)
            [[ $# -ge 2 ]] || die "--ssh-key requires a value"
            SSH_KEY="$2"
            shift 2
            ;;
        --ssh-port)
            [[ $# -ge 2 ]] || die "--ssh-port requires a value"
            SSH_PORT="$2"
            shift 2
            ;;
        --connect-timeout)
            [[ $# -ge 2 ]] || die "--connect-timeout requires a value"
            CONNECT_TIMEOUT="$2"
            shift 2
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            die "unknown option: $1"
            ;;
        *)
            if [[ -n "${TARGET_HOST}" ]]; then
                die "target host already set: ${TARGET_HOST}"
            fi
            TARGET_HOST="$1"
            shift
            ;;
    esac
done

[[ -n "${TARGET_HOST}" ]] || {
    usage >&2
    exit 2
}

if [[ "${TARGET_HOST}" != *@* ]]; then
    TARGET_HOST="root@${TARGET_HOST}"
fi

case "${SET_AUDIO_OUTPUT}" in
    ""|external|headphones) ;;
    *) die "--audio-output must be external or headphones" ;;
esac

require_cmd bash
require_cmd git
require_cmd make
require_cmd ssh
require_cmd tar
require_cmd sha256sum

SDK_DIR="$(cd "${SDK_DIR}" && pwd)"
LAUNCHER_DIR="$(cd "${LAUNCHER_DIR}" && pwd)"
OUT_DIR="${SDK_DIR}/output/${CONF}"
IMAGE_DIR="${OUT_DIR}/images"
IMAGE="${IMAGE_DIR}/Image"
DTB="${IMAGE_DIR}/k230-canmv-rm69a10.dtb"
HDMI_DTB="${IMAGE_DIR}/k230-canmv-rm69a10-hdmi.dtb"
APP_DIR="${OUT_DIR}/target/root/app/k230_phone_ui"
FACE_DIR="${OUT_DIR}/target/root/app/face_detect"
MUSIC_SRC="${LAUNCHER_DIR}/resources/music"
VIDEO_SRC="${LAUNCHER_DIR}/resources/videos"
NOTIFICATION_SRC="${LAUNCHER_DIR}/resources/notification"
MAP_SRC="${LAUNCHER_DIR}/resources/maps"
INSTALL_SCRIPT="${LAUNCHER_DIR}/scripts/install_to_sdk.sh"
RUNTIME_LIB_DIR="${OUT_DIR}/target/usr/lib"
RUNTIME_LIB_PATTERNS=(
    "libcodec2.so*"
    "libasound.so*"
    "libopus.so*"
    "libssl.so.3"
    "libcrypto.so.3"
)

[[ -x "${INSTALL_SCRIPT}" ]] || die "missing launcher install script: ${INSTALL_SCRIPT}"
git -C "${SDK_DIR}" rev-parse --is-inside-work-tree >/dev/null 2>&1 || die "SDK is not a git checkout: ${SDK_DIR}"
[[ -d "${LAUNCHER_DIR}" ]] || die "missing launcher directory: ${LAUNCHER_DIR}"

if [[ "${DO_BUILD}" -eq 1 ]]; then
    if [[ "${DEPLOY_APP}" -eq 1 || "${DEPLOY_MEDIA}" -eq 1 ]]; then
        run bash "${INSTALL_SCRIPT}" "${SDK_DIR}" "${CONF}"
    fi
    if [[ "${DEPLOY_APP}" -eq 1 ]]; then
        run make -C "${SDK_DIR}" "CONF=${CONF}" k230_phone_ui-rebuild
    fi
    if [[ "${DEPLOY_FIRMWARE}" -eq 1 ]]; then
        run make -C "${SDK_DIR}" "CONF=${CONF}" linux-rebuild
    fi
fi

if [[ "${DEPLOY_APP}" -eq 1 ]]; then
    [[ -d "${APP_DIR}" ]] || die "missing app directory: ${APP_DIR}"
    [[ -x "${APP_DIR}/k230_phone_ui" ]] || die "missing app binary: ${APP_DIR}/k230_phone_ui"
fi

if [[ "${DEPLOY_FIRMWARE}" -eq 1 ]]; then
    [[ -f "${IMAGE}" ]] || die "missing Image: ${IMAGE}"
    [[ -f "${DTB}" ]] || die "missing DTB: ${DTB}"
fi

SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o ConnectTimeout="${CONNECT_TIMEOUT}")
if [[ -n "${SSH_KEY}" ]]; then
    SSH_OPTS+=(-i "${SSH_KEY}")
fi
if [[ -n "${SSH_PORT}" ]]; then
    SSH_OPTS+=(-p "${SSH_PORT}")
fi

TS="$(date -u +%Y%m%d_%H%M%S)"
REMOTE_TMP="/tmp/k230_launcher_deploy_${TS}_$$"
REMOTE_BACKUP="/root/deploy_backups/${LABEL}_${TS}"

cat <<EOF
Target      : ${TARGET_HOST}
SDK output  : ${OUT_DIR}
Launcher    : ${LAUNCHER_DIR}
Remote tmp  : ${REMOTE_TMP}
Backup      : ${REMOTE_BACKUP}
Deploy app  : ${DEPLOY_APP}
Deploy boot : ${DEPLOY_FIRMWARE}
Deploy media: ${DEPLOY_MEDIA}
Deploy maps : ${DEPLOY_MAPS}
Deploy libs : ${DEPLOY_RUNTIME_LIBS}
Restart app : ${RESTART_APP}
Reboot      : ${DO_REBOOT}
EOF

if [[ "${DRY_RUN}" -eq 1 ]]; then
    echo "Dry run enabled; no target changes will be made."
    exit 0
fi

echo
echo "[1/5] Prepare target and backup current files"
ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" \
    "REMOTE_TMP='${REMOTE_TMP}' REMOTE_BACKUP='${REMOTE_BACKUP}' REMOTE_APP_DIR='${REMOTE_APP_DIR}' REMOTE_FACE_DIR='${REMOTE_FACE_DIR}' REMOTE_BOOT='${REMOTE_BOOT}' DEPLOY_APP='${DEPLOY_APP}' DEPLOY_FIRMWARE='${DEPLOY_FIRMWARE}' DEPLOY_MEDIA='${DEPLOY_MEDIA}' DEPLOY_MAPS='${DEPLOY_MAPS}' DEPLOY_RUNTIME_LIBS='${DEPLOY_RUNTIME_LIBS}' sh -s" <<'REMOTE'
set -e
mkdir -p "${REMOTE_TMP}/app" "${REMOTE_TMP}/face_detect" \
         "${REMOTE_TMP}/boot" "${REMOTE_TMP}/music" "${REMOTE_TMP}/videos" \
         "${REMOTE_TMP}/notification" "${REMOTE_TMP}/maps" \
         "${REMOTE_TMP}/lib" "${REMOTE_BACKUP}/app" \
         "${REMOTE_BACKUP}/boot" "${REMOTE_BACKUP}/media" \
         "${REMOTE_BACKUP}/lib"

if [ "${DEPLOY_APP}" = "1" ]; then
    if [ -d "${REMOTE_APP_DIR}" ]; then
        cp -a "${REMOTE_APP_DIR}" "${REMOTE_BACKUP}/app/k230_phone_ui" 2>/dev/null || true
    fi
    if [ -d "${REMOTE_FACE_DIR}" ]; then
        cp -a "${REMOTE_FACE_DIR}" "${REMOTE_BACKUP}/app/face_detect" 2>/dev/null || true
    fi
fi

if [ "${DEPLOY_RUNTIME_LIBS}" = "1" ]; then
    for pattern in libcodec2.so* libasound.so* libopus.so* libssl.so.3 libcrypto.so.3; do
        cp -a /usr/lib/${pattern} "${REMOTE_BACKUP}/lib/" 2>/dev/null || true
    done
fi

if [ "${DEPLOY_FIRMWARE}" = "1" ] && [ -d "${REMOTE_BOOT}" ]; then
    cp -a "${REMOTE_BOOT}/Image" \
          "${REMOTE_BOOT}/k.dtb" \
          "${REMOTE_BOOT}/k230-canmv-rm69a10.dtb" \
          "${REMOTE_BOOT}/k230-canmv-rm69a10-hdmi.dtb" \
          "${REMOTE_BACKUP}/boot/" 2>/dev/null || true
fi

if [ "${DEPLOY_MEDIA}" = "1" ]; then
    [ -d /root/music ] && cp -a /root/music "${REMOTE_BACKUP}/media/music" 2>/dev/null || true
    [ -d /root/videos ] && cp -a /root/videos "${REMOTE_BACKUP}/media/videos" 2>/dev/null || true
    [ -d /root/notification ] && cp -a /root/notification "${REMOTE_BACKUP}/media/notification" 2>/dev/null || true
fi
if [ "${DEPLOY_MAPS}" = "1" ]; then
    [ -d /root/maps ] && cp -a /root/maps "${REMOTE_BACKUP}/media/maps" 2>/dev/null || true
fi
REMOTE

echo "[2/5] Upload artifacts"
if [[ "${DEPLOY_APP}" -eq 1 ]]; then
    tar -C "${APP_DIR}" -cf - . | ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" \
        "tar -C '${REMOTE_TMP}/app' -xf -"
    if [[ -d "${FACE_DIR}" ]]; then
        tar -C "${FACE_DIR}" -cf - . | ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" \
            "tar -C '${REMOTE_TMP}/face_detect' -xf -"
    fi
    if [[ "${DEPLOY_RUNTIME_LIBS}" -eq 1 ]]; then
        runtime_libs=()
        for pattern in "${RUNTIME_LIB_PATTERNS[@]}"; do
            while IFS= read -r -d '' lib; do
                runtime_libs+=("$(basename "${lib}")")
            done < <(find "${RUNTIME_LIB_DIR}" -maxdepth 1 -name "${pattern}" -print0)
        done
        if [[ "${#runtime_libs[@]}" -gt 0 ]]; then
            (cd "${RUNTIME_LIB_DIR}" && tar -cf - "${runtime_libs[@]}") | ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" \
                "tar -C '${REMOTE_TMP}/lib' -xf -"
        fi
    fi
fi

if [[ "${DEPLOY_FIRMWARE}" -eq 1 ]]; then
    boot_files=(Image k230-canmv-rm69a10.dtb)
    if [[ -f "${HDMI_DTB}" ]]; then
        boot_files+=(k230-canmv-rm69a10-hdmi.dtb)
    fi
    tar -C "${IMAGE_DIR}" -cf - "${boot_files[@]}" | ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" \
        "tar -C '${REMOTE_TMP}/boot' -xf -"
fi

if [[ "${DEPLOY_MEDIA}" -eq 1 ]]; then
    if [[ -d "${MUSIC_SRC}" ]]; then
        tar -C "${MUSIC_SRC}" -cf - . | ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" \
            "tar -C '${REMOTE_TMP}/music' -xf -"
    else
        echo "No music resource directory: ${MUSIC_SRC}"
    fi
    if [[ -d "${VIDEO_SRC}" ]]; then
        tar -C "${VIDEO_SRC}" -cf - . | ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" \
            "tar -C '${REMOTE_TMP}/videos' -xf -"
    else
        echo "No video resource directory: ${VIDEO_SRC}"
    fi
    if [[ -d "${NOTIFICATION_SRC}" ]]; then
        tar -C "${NOTIFICATION_SRC}" -cf - . | ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" \
            "tar -C '${REMOTE_TMP}/notification' -xf -"
    else
        echo "No notification resource directory: ${NOTIFICATION_SRC}"
    fi
fi

if [[ "${DEPLOY_MAPS}" -eq 1 ]]; then
    if [[ -d "${MAP_SRC}" ]]; then
        tar -C "${MAP_SRC}" -cf - . | ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" \
            "tar -C '${REMOTE_TMP}/maps' -xf -"
    else
        echo "No map resource directory: ${MAP_SRC}"
    fi
fi

echo "[3/5] Install on target"
ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" \
    "REMOTE_TMP='${REMOTE_TMP}' REMOTE_APP_DIR='${REMOTE_APP_DIR}' REMOTE_FACE_DIR='${REMOTE_FACE_DIR}' REMOTE_BOOT='${REMOTE_BOOT}' REMOTE_MUSIC_DIR='${REMOTE_MUSIC_DIR}' REMOTE_VIDEO_DIR='${REMOTE_VIDEO_DIR}' REMOTE_NOTIFICATION_DIR='${REMOTE_NOTIFICATION_DIR}' REMOTE_MAP_DIR='${REMOTE_MAP_DIR}' DEPLOY_APP='${DEPLOY_APP}' DEPLOY_FIRMWARE='${DEPLOY_FIRMWARE}' DEPLOY_MEDIA='${DEPLOY_MEDIA}' DEPLOY_MAPS='${DEPLOY_MAPS}' DEPLOY_RUNTIME_LIBS='${DEPLOY_RUNTIME_LIBS}' RESTART_APP='${RESTART_APP}' SET_AUDIO_OUTPUT='${SET_AUDIO_OUTPUT}' sh -s" <<'REMOTE'
set -e

if [ "${DEPLOY_APP}" = "1" ]; then
    if [ -x /etc/init.d/S99zz_k230_phone_ui ]; then
        /etc/init.d/S99zz_k230_phone_ui stop >/tmp/k230_phone_ui_deploy_stop.log 2>&1 || true
    fi
    killall -q k230_phone_ui k230_phone_ui_fullswitch_test \
        k230_meshtastic_probe k230_xiaozhi_probe k230_xiaozhi_kws \
        ffmpeg aplay || true
    for _ in 1 2 3 4 5; do
        if ! pidof k230_phone_ui >/dev/null 2>&1 &&
           ! pidof k230_phone_ui_fullswitch_test >/dev/null 2>&1; then
            break
        fi
        sleep 1
    done
    rm -f /var/run/k230_phone_ui.pid
    mkdir -p /root/app
    rm -rf "${REMOTE_APP_DIR}"
    mkdir -p "${REMOTE_APP_DIR}"
    cp -a "${REMOTE_TMP}/app/." "${REMOTE_APP_DIR}/"
    if [ "$(find "${REMOTE_TMP}/face_detect" -mindepth 1 -print -quit 2>/dev/null)" ]; then
        rm -rf "${REMOTE_FACE_DIR}"
        mkdir -p "${REMOTE_FACE_DIR}"
        cp -a "${REMOTE_TMP}/face_detect/." "${REMOTE_FACE_DIR}/"
    fi
    if [ "${DEPLOY_RUNTIME_LIBS}" = "1" ] &&
       [ "$(find "${REMOTE_TMP}/lib" -mindepth 1 -print -quit 2>/dev/null)" ]; then
        mkdir -p /usr/lib
        cp -a "${REMOTE_TMP}/lib/." /usr/lib/
    fi
    chmod 755 "${REMOTE_APP_DIR}/k230_phone_ui" \
              "${REMOTE_APP_DIR}/k230_phone_ui_fullswitch_test" \
              "${REMOTE_APP_DIR}/k230_camera_capture" \
              "${REMOTE_APP_DIR}/k230_pcm_volume" \
              "${REMOTE_APP_DIR}/drm_motion_probe" \
              "${REMOTE_APP_DIR}/drm_motion_probe_rgb565" 2>/dev/null || true
    mkdir -p /root/qrcode
fi

if [ "${DEPLOY_FIRMWARE}" = "1" ]; then
    mount -o remount,rw "${REMOTE_BOOT}" 2>/dev/null || true
    install -m 0644 "${REMOTE_TMP}/boot/Image" "${REMOTE_BOOT}/Image"
    install -m 0644 "${REMOTE_TMP}/boot/k230-canmv-rm69a10.dtb" "${REMOTE_BOOT}/k230-canmv-rm69a10.dtb"
    install -m 0644 "${REMOTE_TMP}/boot/k230-canmv-rm69a10.dtb" "${REMOTE_BOOT}/k.dtb"
    if [ -f "${REMOTE_TMP}/boot/k230-canmv-rm69a10-hdmi.dtb" ]; then
        install -m 0644 "${REMOTE_TMP}/boot/k230-canmv-rm69a10-hdmi.dtb" "${REMOTE_BOOT}/k230-canmv-rm69a10-hdmi.dtb"
    fi
fi

if [ "${DEPLOY_MEDIA}" = "1" ]; then
    mkdir -p "${REMOTE_MUSIC_DIR}" "${REMOTE_VIDEO_DIR}" "${REMOTE_NOTIFICATION_DIR}" /root/nes /root/photos /root/screenshots /root/qrcode /root/recordings /root/lorawan
    if [ "$(find "${REMOTE_TMP}/music" -mindepth 1 -print -quit 2>/dev/null)" ]; then
        rm -rf "${REMOTE_MUSIC_DIR}"
        mkdir -p "${REMOTE_MUSIC_DIR}"
        cp -a "${REMOTE_TMP}/music/." "${REMOTE_MUSIC_DIR}/"
    fi
    if [ "$(find "${REMOTE_TMP}/videos" -mindepth 1 -print -quit 2>/dev/null)" ]; then
        rm -rf "${REMOTE_VIDEO_DIR}"
        mkdir -p "${REMOTE_VIDEO_DIR}"
        cp -a "${REMOTE_TMP}/videos/." "${REMOTE_VIDEO_DIR}/"
    fi
    if [ "$(find "${REMOTE_TMP}/notification" -mindepth 1 -print -quit 2>/dev/null)" ]; then
        rm -rf "${REMOTE_NOTIFICATION_DIR}"
        mkdir -p "${REMOTE_NOTIFICATION_DIR}"
        cp -a "${REMOTE_TMP}/notification/." "${REMOTE_NOTIFICATION_DIR}/"
    fi
fi

if [ "${DEPLOY_MAPS}" = "1" ]; then
    mkdir -p "${REMOTE_MAP_DIR}"
    if [ "$(find "${REMOTE_TMP}/maps" -mindepth 1 -print -quit 2>/dev/null)" ]; then
        rm -rf "${REMOTE_MAP_DIR}"
        mkdir -p "${REMOTE_MAP_DIR}"
        cp -a "${REMOTE_TMP}/maps/." "${REMOTE_MAP_DIR}/"
    fi
fi

if [ -n "${SET_AUDIO_OUTPUT}" ]; then
    mkdir -p /root/.config/k230_phone_ui
    touch /root/.config/k230_phone_ui/settings.conf
    if grep -q '^audio.output=' /root/.config/k230_phone_ui/settings.conf; then
        sed -i "s/^audio.output=.*/audio.output=${SET_AUDIO_OUTPUT}/" \
            /root/.config/k230_phone_ui/settings.conf
    else
        printf '\naudio.output=%s\n' "${SET_AUDIO_OUTPUT}" >> \
            /root/.config/k230_phone_ui/settings.conf
    fi
fi

if [ "${DEPLOY_APP}" = "1" ] && [ "${RESTART_APP}" = "1" ] && [ "${DEPLOY_FIRMWARE}" != "1" ]; then
    if [ -x /etc/init.d/S99zz_k230_phone_ui ]; then
        /etc/init.d/S99zz_k230_phone_ui start >/tmp/k230_phone_ui_deploy_start.log 2>&1 || true
    elif [ -x "${REMOTE_APP_DIR}/k230_phone_ui" ]; then
        mkdir -p /var/log
        (cd "${REMOTE_APP_DIR}" && HOME=/root nohup ./k230_phone_ui >>/var/log/k230_phone_ui.log 2>&1 &)
    fi
fi

sync
REMOTE

echo "[4/5] Verify target"
ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" \
    "DEPLOY_APP='${DEPLOY_APP}' DEPLOY_FIRMWARE='${DEPLOY_FIRMWARE}' REMOTE_APP_DIR='${REMOTE_APP_DIR}' REMOTE_FACE_DIR='${REMOTE_FACE_DIR}' REMOTE_BOOT='${REMOTE_BOOT}' sh -s" <<'REMOTE'
set -e
if [ "${DEPLOY_APP}" = "1" ]; then
    sha256sum "${REMOTE_APP_DIR}/k230_phone_ui" \
              "${REMOTE_APP_DIR}/k230_phone_ui_fullswitch_test" \
              "${REMOTE_APP_DIR}/k230_pcm_volume" \
              "${REMOTE_APP_DIR}/k230_xiaozhi_probe" \
              "${REMOTE_APP_DIR}/k230_xiaozhi_kws" 2>/dev/null || true
    sha256sum /usr/lib/libasound.so.2.0.0 \
              /usr/lib/libopus.so.0.9.0 \
              /usr/lib/libssl.so.3 \
              /usr/lib/libcrypto.so.3 2>/dev/null || true
    if [ -f "${REMOTE_FACE_DIR}/face_detection_320.kmodel" ]; then
        sha256sum "${REMOTE_FACE_DIR}/face_detection_320.kmodel"
    fi
    pidof k230_phone_ui >/dev/null 2>&1 && echo "k230_phone_ui: running" || echo "k230_phone_ui: not running"
fi
if [ "${DEPLOY_FIRMWARE}" = "1" ]; then
    sha256sum "${REMOTE_BOOT}/Image" \
              "${REMOTE_BOOT}/k230-canmv-rm69a10.dtb" \
              "${REMOTE_BOOT}/k.dtb"
fi
REMOTE

echo "[5/5] Cleanup"
ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" "rm -rf '${REMOTE_TMP}'; sync"

if [[ "${DO_REBOOT}" -eq 1 ]]; then
    echo "Rebooting target..."
    ssh "${SSH_OPTS[@]}" "${TARGET_HOST}" "reboot" || true
else
    echo "No reboot requested."
fi

echo "Done. Backup: ${TARGET_HOST}:${REMOTE_BACKUP}"
