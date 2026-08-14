#!/bin/sh
set -eu

ROLE="rx"
PEER=""
LOCAL_IP=""
NETMASK="255.255.255.0"
PORT="5600"
PRESET="320x240"
FPS="12"
JPEG_QUALITY="45"
PAYLOAD="1200"
PACKET_GAP_US="0"
REPEAT="1"
DURATION="3600"
CAMERA_DEV="/dev/video1"
CAPTURE_WIDTH="1920"
CAPTURE_HEIGHT="1080"
ENCODE_WIDTH="320"
ENCODE_HEIGHT="240"
PREVIEW_WIDTH="640"
PREVIEW_HEIGHT="360"
CAMERA_ROTATE="90"
CAMERA_FLIP_X="0"
CAMERA_FLIP_Y="0"
APP_DIR="/root/app/k230_phone_ui"
CAPTURE_BIN="${APP_DIR}/k230_camera_capture"
UDP_BIN="${APP_DIR}/k230_halow_udp_stream"
TX_DIR="/tmp/k230_halow_tx_queue"
RX_DIR="/root/videos/halow_rx"
PREVIEW_FILE="/tmp/k230_halow_preview.rgb565"
PREVIEW_META="/tmp/k230_halow_preview.meta"
STOP_FILE="/tmp/k230_halow_camera_stream.stop"
CAPTURE_LOG="/tmp/k230_halow_camera_capture.log"
UDP_LOG="/tmp/k230_halow_udp_inner.log"
CAPTURE_PID=""
UDP_PID=""

usage() {
    cat <<'USAGE'
Usage:
  k230_halow_camera_stream.sh --role tx|rx [options]

Halow camera video transport uses Ethernet IP networking. Set a static local
IP on the Halow Ethernet side, enter the peer board IP, then start TX on one
board and RX on the other.

Options:
  --role tx|rx           Transmit camera video or receive video.
  --peer IP              Peer receiver IP for TX mode.
  --local-ip IP          Configure eth0 with this local IP before starting.
  --netmask MASK         Netmask for --local-ip. Default 255.255.255.0.
  --port N               UDP port. Default 5600.
  --preset 320x240|640x480|720p
                          Transmit resolution. Default 320x240.
  --fps N                Capture/send FPS. Default 12.
  --jpeg-quality N       JPEG quality 5..95. Default 45.
  --payload N            UDP payload bytes. Default 1200.
  --packet-gap-us N      Delay between UDP packets. Default 0.
  --repeat N             Repeat each UDP frame 1..4. Default 1.
  --duration SEC         Run limit. Default 3600.
  --camera-rotate N      Camera rotation 0, 90, 180, or 270. Default 90.
  --camera-flip-x        Mirror camera horizontally after rotation.
  --camera-flip-y        Mirror camera vertically after rotation.
USAGE
}

log() {
    printf '[halow-camera] %s\n' "$*"
}

cleanup() {
    if [ -n "${CAPTURE_PID}" ]; then
        kill "${CAPTURE_PID}" >/dev/null 2>&1 || true
        wait "${CAPTURE_PID}" >/dev/null 2>&1 || true
    fi
    if [ -n "${UDP_PID}" ]; then
        kill "${UDP_PID}" >/dev/null 2>&1 || true
        wait "${UDP_PID}" >/dev/null 2>&1 || true
    fi
}

need_bin() {
    if [ ! -x "$1" ]; then
        log "missing executable: $1"
        exit 127
    fi
}

camera_device_index() {
    case "${CAMERA_DEV}" in
        /dev/video*) printf '%s\n' "${CAMERA_DEV#/dev/video}" ;;
        *) printf '%s\n' "${CAMERA_DEV}" ;;
    esac
}

apply_preset() {
    case "${PRESET}" in
        320x240|qvga)
            ENCODE_WIDTH="320"
            ENCODE_HEIGHT="240"
            PREVIEW_WIDTH="640"
            PREVIEW_HEIGHT="360"
            ;;
        640x480|vga)
            ENCODE_WIDTH="640"
            ENCODE_HEIGHT="480"
            PREVIEW_WIDTH="640"
            PREVIEW_HEIGHT="360"
            ;;
        720p)
            ENCODE_WIDTH="1280"
            ENCODE_HEIGHT="720"
            PREVIEW_WIDTH="640"
            PREVIEW_HEIGHT="360"
            ;;
        *)
            log "invalid preset: ${PRESET}"
            exit 2
            ;;
    esac
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --role) ROLE="$2"; shift 2 ;;
        --peer) PEER="$2"; shift 2 ;;
        --local-ip) LOCAL_IP="$2"; shift 2 ;;
        --netmask) NETMASK="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --preset) PRESET="$2"; shift 2 ;;
        --fps) FPS="$2"; shift 2 ;;
        --jpeg-quality) JPEG_QUALITY="$2"; shift 2 ;;
        --payload) PAYLOAD="$2"; shift 2 ;;
        --packet-gap-us) PACKET_GAP_US="$2"; shift 2 ;;
        --repeat) REPEAT="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --camera-rotate) CAMERA_ROTATE="$2"; shift 2 ;;
        --camera-flip-x) CAMERA_FLIP_X="1"; shift ;;
        --camera-flip-y) CAMERA_FLIP_Y="1"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) log "unknown argument: $1"; usage; exit 2 ;;
    esac
done

case "${ROLE}" in
    tx|rx) ;;
    *) log "invalid role: ${ROLE}"; usage; exit 2 ;;
esac
case "${CAMERA_ROTATE}" in
    0|90|180|270) ;;
    *) log "invalid camera rotate: ${CAMERA_ROTATE}"; exit 2 ;;
esac
apply_preset

trap cleanup INT TERM EXIT
rm -f "${STOP_FILE}" "${PREVIEW_FILE}" "${PREVIEW_FILE}.tmp" "${PREVIEW_META}" \
      "${CAPTURE_LOG}" "${UDP_LOG}"

need_bin "${UDP_BIN}"
mkdir -p "${RX_DIR}"

if [ -n "${LOCAL_IP}" ]; then
    log "configure eth0 ${LOCAL_IP}/${NETMASK}"
    ifconfig eth0 up >> "${UDP_LOG}" 2>&1 || true
    ifconfig eth0 "${LOCAL_IP}" netmask "${NETMASK}" >> "${UDP_LOG}" 2>&1 || true
fi

log "role=${ROLE} peer=${PEER:-none} port=${PORT} preset=${PRESET} stream=${ENCODE_WIDTH}x${ENCODE_HEIGHT}@${FPS} q=${JPEG_QUALITY} preview=${PREVIEW_WIDTH}x${PREVIEW_HEIGHT} rotate=${CAMERA_ROTATE} flip_x=${CAMERA_FLIP_X} flip_y=${CAMERA_FLIP_Y}"

if [ "${ROLE}" = "rx" ]; then
    exec "${UDP_BIN}" --role rx --port "${PORT}" --out-dir "${RX_DIR}" \
        --payload "${PAYLOAD}" --preview-file "${PREVIEW_FILE}" \
        --meta-file "${PREVIEW_META}" --preview-width "${PREVIEW_WIDTH}" \
        --preview-height "${PREVIEW_HEIGHT}" --stream-width "${ENCODE_WIDTH}" \
        --stream-height "${ENCODE_HEIGHT}" --stop-file "${STOP_FILE}"
fi

if [ -z "${PEER}" ]; then
    log "TX requires --peer"
    exit 2
fi
if [ ! -e "${CAMERA_DEV}" ]; then
    log "missing camera device: ${CAMERA_DEV}"
    exit 1
fi
need_bin "${CAPTURE_BIN}"

rm -rf "${TX_DIR}"
mkdir -p "${TX_DIR}"

transform_args="--rotate ${CAMERA_ROTATE}"
if [ "${CAMERA_FLIP_X}" = "1" ]; then
    transform_args="${transform_args} --flip-x"
fi
if [ "${CAMERA_FLIP_Y}" = "1" ]; then
    transform_args="${transform_args} --flip-y"
fi

camera_index="$(camera_device_index)"
"${CAPTURE_BIN}" -d "${camera_index}" \
    -w "${CAPTURE_WIDTH}" -h "${CAPTURE_HEIGHT}" -f NV16 \
    --thumb-width "${ENCODE_WIDTH}" --thumb-height "${ENCODE_HEIGHT}" \
    --skip 1 --stream-dir "${TX_DIR}" --stream-prefix halow_cam \
    --stream-output jpeg --jpeg-quality "${JPEG_QUALITY}" \
    --stream-duration "${DURATION}" --stream-fps "${FPS}" \
    --stream-max-files 4 ${transform_args} > "${CAPTURE_LOG}" 2>&1 &
CAPTURE_PID="$!"

"${UDP_BIN}" --role tx --host "${PEER}" --port "${PORT}" \
    --in-dir "${TX_DIR}" --payload "${PAYLOAD}" \
    --preview-file "${PREVIEW_FILE}" --meta-file "${PREVIEW_META}" \
    --preview-width "${PREVIEW_WIDTH}" --preview-height "${PREVIEW_HEIGHT}" \
    --stream-width "${ENCODE_WIDTH}" --stream-height "${ENCODE_HEIGHT}" \
    --stop-file "${STOP_FILE}" --packet-gap-us "${PACKET_GAP_US}" \
    --repeat "${REPEAT}" > "${UDP_LOG}" 2>&1 &
UDP_PID="$!"

start_s="$(date +%s)"
while [ ! -e "${STOP_FILE}" ]; do
    now_s="$(date +%s)"
    if [ $((now_s - start_s)) -ge "${DURATION}" ]; then
        break
    fi
    if ! kill -0 "${CAPTURE_PID}" >/dev/null 2>&1; then
        log "capture process exited"
        tail -20 "${CAPTURE_LOG}" 2>/dev/null || true
        break
    fi
    if ! kill -0 "${UDP_PID}" >/dev/null 2>&1; then
        log "udp process exited"
        tail -20 "${UDP_LOG}" 2>/dev/null || true
        break
    fi
    sleep 1
done

log "stop"
exit 0
