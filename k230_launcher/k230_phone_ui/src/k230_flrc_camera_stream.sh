#!/bin/sh
set -eu

ROLE="tx"
CODEC="h264"
STREAM_FORMAT="image"
WIDTH="320"
HEIGHT="240"
CAPTURE_WIDTH="320"
CAPTURE_HEIGHT="240"
ENCODE_WIDTH="80"
ENCODE_HEIGHT="60"
BITRATE_KBPS="48"
JPEG_QUALITY="28"
SEGMENT_SECONDS="2"
FPS="6"
COUNT="0"
DURATION="3600"
FREQ_MHZ="2400"
BR_KBPS="2600"
SPI_HZ="16000000"
PAYLOAD_LEN="252"
POWER_DBM="8"
RX_POLL_US="50"
RETRIES="10"
ACK_WAIT_MS="5000"
APP_DIR="/root/app/k230_phone_ui"
TX_DIR="/root/videos/flrc_tx"
RX_DIR="/root/videos/flrc_rx"
CAMERA_DEV="/dev/video1"
CAPTURE_BIN="${APP_DIR}/k230_camera_capture"
CAPTURE_FORMAT="NV16"
FFMPEG_BIN="/usr/bin/ffmpeg"
FLRC_BIN="/root/app/k230_phone_ui/k230_lora_flrc_video"
FLRC_TILE_BIN="/root/app/k230_phone_ui/k230_lora_flrc_tile_stream"
STOP_FILE="/tmp/k230_flrc_camera_stream.stop"
FFMPEG_LOG="/tmp/k230_flrc_camera_stream_ffmpeg.log"
RX_INNER_LOG="/tmp/k230_flrc_camera_stream_rx_inner.log"
TX_INNER_LOG="/tmp/k230_flrc_camera_stream_tx_inner.log"
PREVIEW_FILE="/tmp/k230_flrc_camera_preview.rgb565"
PREVIEW_META="/tmp/k230_flrc_camera_preview.meta"
PREVIEW_DIR="/tmp/k230_flrc_camera_preview_frames"
TX_QUEUE_DIR="/tmp/k230_flrc_camera_tx_queue"
PREVIEW_WIDTH="80"
PREVIEW_HEIGHT="60"
PREVIEW_FPS="4"
PREVIEW_BUFFER_MIN="3"
PREVIEW_MAX_FRAMES="72"
FAST_FRAME="0"
TILE_STREAM="1"
COMPRESSED_IMAGE_STREAM="0"
STREAM2_FRAME="1"
CAMERA_ROTATE="90"
CAMERA_FLIP_X="0"
CAMERA_FLIP_Y="0"
TILE_WIDTH="20"
TILE_HEIGHT="5"
TILE_REPEAT="1"
TILE_GAP_US="1200"
TILE_FRAME_GAP_US="5000"
RX_PID=""
TX_SENDER_PID=""
TAIL_PID=""
PREVIEW_PLAYER_PID=""
CAPTURE_PID=""
PREVIEW_QUEUE_SEQ="0"

usage() {
    cat <<'USAGE'
Usage:
  k230_flrc_camera_stream.sh --role tx|rx|capture [options]

TX records short low-bitrate camera clips through ffmpeg + the K230 encoder,
then sends each clip through k230_lora_flrc_video reliable FLRC transfer.
RX keeps listening, saves received clips to /root/videos/flrc_rx, and decodes
received segments into a RGB565 preview frame consumed by the Launcher UI.

Options:
  --role ROLE         tx, rx, or capture.
  --codec h264|h265  Video mode codec. Default: h264.
  --stream-format image|video
                     Default: image. Image mode sends small JPEG frames.
  --width N          Camera capture width. Default: 320.
  --height N         Camera capture height. Default: 240.
  --capture-width N  v4l2-drm still capture width. Default: 320.
  --capture-height N v4l2-drm still capture height. Default: 240.
  --encode-width N   Encoded stream width. Default: 80.
  --encode-height N  Encoded stream height. Default: 60.
  --jpeg-quality N   Image mode JPEG q:v value. Default: 28.
  --fps N            Default: 6.
  --bitrate KBPS     Camera encoder bitrate. Default: 48.
  --segment SEC      Segment length. Default: 2.
  --count N          TX/capture segment count. 0 means loop until stopped.
  --duration SEC     RX listen duration. Default: 3600.
  --freq MHz         FLRC frequency. Default: 2400.
  --br KBPS          FLRC bitrate. Default: 2600.
  --spi-hz HZ        FLRC SPI clock. Default: 16000000.
  --out-dir DIR      RX output directory.
  --tx-dir DIR       TX segment directory.
  --device DEV       Camera device. Default: /dev/video1.
  --preview-file P   RGB565 preview output. Default: /tmp/k230_flrc_camera_preview.rgb565.
  --preview-width N  Tile preview width. Default: 80.
  --preview-height N Tile preview height. Default: 60.
  --preview-fps N    Buffered preview playback FPS. Default: 4.
  --preview-buffer N Frames to buffer before playback. Default: 3.
  --tile-w N         Tile width. Default: 20.
  --tile-h N         Tile height. Default: 5.
  --tile-repeat N    Per-tile repeat count. Default: 1.
  --tile-gap-us N    Gap between FLRC tile packets. Default: 1200.
  --tile-frame-gap-us N
                     Gap between tile frames. Default: 5000.
  --stop-file PATH   Stop marker. Default: /tmp/k230_flrc_camera_stream.stop.
  --fast-frame       Low-latency JPEG FLRC mode. Drops broken frames instead
                     of waiting for per-frame ACK/NACK repair.
  --tile-stream      RGB565 tile update mode. Default on for camera streaming.
  --no-tile-stream   Use the older JPEG frame transfer mode.
  --compressed-image-stream
                     Continuous JPEG still stream. Uses stream v2 by default.
  --legacy-frame-stream
                     Use the older per-frame file transfer instead of stream v2.
  --camera-rotate N  Rotate camera output: 0, 90, 180, or 270. Default: 90.
  --camera-flip-x    Horizontally mirror camera output after rotation.
  --camera-flip-y    Vertically flip camera output after rotation.
USAGE
}

log() {
    printf '[flrc-camera] %s\n' "$*"
}

cleanup() {
    if [ -n "${TAIL_PID}" ]; then
        kill "${TAIL_PID}" >/dev/null 2>&1 || true
        wait "${TAIL_PID}" >/dev/null 2>&1 || true
    fi
    if [ -n "${PREVIEW_PLAYER_PID}" ]; then
        kill "${PREVIEW_PLAYER_PID}" >/dev/null 2>&1 || true
        wait "${PREVIEW_PLAYER_PID}" >/dev/null 2>&1 || true
    fi
    if [ -n "${CAPTURE_PID}" ]; then
        kill "${CAPTURE_PID}" >/dev/null 2>&1 || true
        wait "${CAPTURE_PID}" >/dev/null 2>&1 || true
    fi
    if [ -n "${RX_PID}" ]; then
        kill "${RX_PID}" >/dev/null 2>&1 || true
        wait "${RX_PID}" >/dev/null 2>&1 || true
    fi
    if [ -n "${TX_SENDER_PID}" ]; then
        kill "${TX_SENDER_PID}" >/dev/null 2>&1 || true
        wait "${TX_SENDER_PID}" >/dev/null 2>&1 || true
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

preview_sleep() {
    if [ "${PREVIEW_FPS}" -le 0 ]; then
        PREVIEW_FPS="1"
    fi
    interval_us=$((1000000 / PREVIEW_FPS))
    if [ "${interval_us}" -lt 20000 ]; then
        interval_us=20000
    fi
    if command -v usleep >/dev/null 2>&1; then
        usleep "${interval_us}"
    else
        sleep 1
    fi
}

queue_count() {
    find "${PREVIEW_DIR}/ready" -maxdepth 1 -type f -name '*.rgb565' \
        2>/dev/null | wc -l | tr -d ' '
}

queue_first() {
    find "${PREVIEW_DIR}/ready" -maxdepth 1 -type f -name '*.rgb565' \
        2>/dev/null | sort | head -n 1
}

queue_trim() {
    count="$(queue_count)"
    while [ "${count}" -gt "${PREVIEW_MAX_FRAMES}" ]; do
        first="$(queue_first)"
        [ -n "${first}" ] || break
        rm -f "${first}"
        count=$((count - 1))
    done
}

tx_queue_count() {
    find "${TX_QUEUE_DIR}" -maxdepth 1 -type f \( -name '*.jpg' -o -name '*.raw' \) \
        2>/dev/null | wc -l | tr -d ' '
}

tx_queue_first() {
    find "${TX_QUEUE_DIR}" -maxdepth 1 -type f \( -name '*.jpg' -o -name '*.raw' \) \
        2>/dev/null | sort | head -n 1
}

tx_queue_trim() {
    count="$(tx_queue_count)"
    while [ "${count}" -gt 3 ]; do
        first="$(tx_queue_first)"
        [ -n "${first}" ] || break
        rm -f "${first}"
        count=$((count - 1))
    done
}

tx_sender_start() {
    rm -rf "${TX_QUEUE_DIR}"
    mkdir -p "${TX_QUEUE_DIR}"
    rm -f "${TX_INNER_LOG}"

    if [ "${TILE_STREAM}" = "1" ]; then
        log "TX tile sender start dir=${TX_QUEUE_DIR} duration=${DURATION}s freq=${FREQ_MHZ} br=${BR_KBPS} spi=${SPI_HZ} canvas=${PREVIEW_WIDTH}x${PREVIEW_HEIGHT} tile=${TILE_WIDTH}x${TILE_HEIGHT} repeat=${TILE_REPEAT}"
        "${FLRC_TILE_BIN}" --role tile-tx \
            --in-dir "${TX_QUEUE_DIR}" --preview "${PREVIEW_FILE}" \
            --width "${PREVIEW_WIDTH}" --height "${PREVIEW_HEIGHT}" \
            --tile-w "${TILE_WIDTH}" --tile-h "${TILE_HEIGHT}" \
            --repeat "${TILE_REPEAT}" --tile-gap-us "${TILE_GAP_US}" \
            --frame-gap-us "${TILE_FRAME_GAP_US}" \
            --freq "${FREQ_MHZ}" --br "${BR_KBPS}" --duration "${DURATION}" \
            --len "${PAYLOAD_LEN}" --spi-hz "${SPI_HZ}" --power "${POWER_DBM}" \
            --rx-poll-us "${RX_POLL_US}" \
            > "${TX_INNER_LOG}" 2>&1 &
    else
        if [ "${STREAM2_FRAME}" = "1" ]; then
            log "TX stream2 sender start dir=${TX_QUEUE_DIR} duration=${DURATION}s freq=${FREQ_MHZ} br=${BR_KBPS} spi=${SPI_HZ} ${PREVIEW_WIDTH}x${PREVIEW_HEIGHT}"
            "${FLRC_BIN}" --role frame-stream-tx \
                --in-dir "${TX_QUEUE_DIR}" \
                --freq "${FREQ_MHZ}" --br "${BR_KBPS}" --duration "${DURATION}" \
                --len "${PAYLOAD_LEN}" --spi-hz "${SPI_HZ}" --power "${POWER_DBM}" \
                --rx-poll-us "${RX_POLL_US}" --frame-width "${PREVIEW_WIDTH}" \
                --frame-height "${PREVIEW_HEIGHT}" --frame-gap-us 900 \
                > "${TX_INNER_LOG}" 2>&1 &
        else
            log "TX queue sender start dir=${TX_QUEUE_DIR} duration=${DURATION}s freq=${FREQ_MHZ} br=${BR_KBPS} spi=${SPI_HZ} fast=${FAST_FRAME}"
            "${FLRC_BIN}" --role stream-queue-tx \
                --in-dir "${TX_QUEUE_DIR}" \
                --freq "${FREQ_MHZ}" --br "${BR_KBPS}" --duration "${DURATION}" \
                --len "${PAYLOAD_LEN}" --spi-hz "${SPI_HZ}" --power "${POWER_DBM}" \
                --rx-poll-us "${RX_POLL_US}" --retries "${RETRIES}" \
                --ack-wait-ms "${ACK_WAIT_MS}" --fast-frame \
                > "${TX_INNER_LOG}" 2>&1 &
        fi
    fi
    TX_SENDER_PID="$!"
    tail -n +1 -f "${TX_INNER_LOG}" &
    TAIL_PID="$!"
}

preview_player_loop() {
    frame_bytes=$((PREVIEW_WIDTH * PREVIEW_HEIGHT * 2))
    tmp="${PREVIEW_FILE}.tmp"
    buffered="0"

    log "preview player start ${PREVIEW_WIDTH}x${PREVIEW_HEIGHT}@${PREVIEW_FPS} buffer=${PREVIEW_BUFFER_MIN}"
    while [ ! -e "${STOP_FILE}" ]; do
        count="$(queue_count)"
        if [ "${count}" -le 0 ]; then
            buffered="0"
            if command -v usleep >/dev/null 2>&1; then
                usleep 80000
            else
                sleep 1
            fi
            continue
        fi
        if [ "${buffered}" = "0" ] && [ "${count}" -lt "${PREVIEW_BUFFER_MIN}" ]; then
            if command -v usleep >/dev/null 2>&1; then
                usleep 80000
            else
                sleep 1
            fi
            continue
        fi
        buffered="1"
        frame="$(queue_first)"
        if [ -z "${frame}" ]; then
            buffered="0"
            continue
        fi
        if [ "$(wc -c < "${frame}" 2>/dev/null || echo 0)" = "${frame_bytes}" ]; then
            cp "${frame}" "${tmp}" 2>/dev/null && mv "${tmp}" "${PREVIEW_FILE}"
            printf 'frame_file=%s\nframes_ready=%s\nwidth=%s\nheight=%s\nrole=%s\nbuffered=1\n' \
                "${frame}" "${count}" "${PREVIEW_WIDTH}" "${PREVIEW_HEIGHT}" \
                "${ROLE}" > "${PREVIEW_META}"
        fi
        rm -f "${frame}" "${tmp}"
        preview_sleep
    done
    rm -f "${tmp}"
}

preview_queue_reset() {
    rm -rf "${PREVIEW_DIR}"
    mkdir -p "${PREVIEW_DIR}/ready" "${PREVIEW_DIR}/stage"
    rm -f "${PREVIEW_FILE}" "${PREVIEW_FILE}.tmp" "${PREVIEW_FILE}.raw" \
          "${PREVIEW_META}"
}

preview_player_start() {
    preview_queue_reset
    preview_player_loop &
    PREVIEW_PLAYER_PID="$!"
}

queue_raw_frame() {
    raw="$1"
    frame_index="$2"
    frame_bytes=$((PREVIEW_WIDTH * PREVIEW_HEIGHT * 2))
    PREVIEW_QUEUE_SEQ=$((PREVIEW_QUEUE_SEQ + 1))
    staged="$(printf '%s/stage/%010d.rgb565' "${PREVIEW_DIR}" "${PREVIEW_QUEUE_SEQ}")"
    ready="$(printf '%s/ready/%010d.rgb565' "${PREVIEW_DIR}" "${PREVIEW_QUEUE_SEQ}")"

    dd if="${raw}" of="${staged}" bs="${frame_bytes}" count=1 \
        skip="${frame_index}" 2>/dev/null || return 1
    if [ "$(wc -c < "${staged}" 2>/dev/null || echo 0)" = "${frame_bytes}" ]; then
        mv "${staged}" "${ready}"
        queue_trim
        return 0
    fi
    rm -f "${staged}"
    return 1
}

queue_single_frame() {
    raw="$1"
    frame_bytes=$((PREVIEW_WIDTH * PREVIEW_HEIGHT * 2))
    PREVIEW_QUEUE_SEQ=$((PREVIEW_QUEUE_SEQ + 1))
    staged="$(printf '%s/stage/%010d.rgb565' "${PREVIEW_DIR}" "${PREVIEW_QUEUE_SEQ}")"
    ready="$(printf '%s/ready/%010d.rgb565' "${PREVIEW_DIR}" "${PREVIEW_QUEUE_SEQ}")"

    if [ "$(wc -c < "${raw}" 2>/dev/null || echo 0)" = "${frame_bytes}" ]; then
        cp "${raw}" "${staged}" 2>/dev/null && mv "${staged}" "${ready}"
        queue_trim
        return 0
    fi
    rm -f "${staged}"
    return 1
}

decode_preview_segment() {
    segment="$1"
    tag="${2:-preview}"
    frame_bytes=$((PREVIEW_WIDTH * PREVIEW_HEIGHT * 2))
    raw="${PREVIEW_FILE}.raw"
    filter="fps=${PREVIEW_FPS},scale=${PREVIEW_WIDTH}:${PREVIEW_HEIGHT}:flags=fast_bilinear"

    case "${segment}" in
        *.jpg|*.jpeg|*.JPG|*.JPEG)
            filter="scale=${PREVIEW_WIDTH}:${PREVIEW_HEIGHT}:flags=fast_bilinear"
            ;;
    esac

    rm -f "${raw}"
    log "${tag} buffer segment $(basename "${segment}")"
    if "${FFMPEG_BIN}" -y -nostdin -hide_banner -loglevel error \
        -i "${segment}" \
        -vf "${filter}" \
        -pix_fmt rgb565le -f rawvideo "${raw}" >> "${FFMPEG_LOG}" 2>&1; then
        raw_bytes="$(wc -c < "${raw}" 2>/dev/null || echo 0)"
        frames=$((raw_bytes / frame_bytes))
        i=0
        while [ "${i}" -lt "${frames}" ] && [ ! -e "${STOP_FILE}" ]; do
            queue_raw_frame "${raw}" "${i}" || break
            i=$((i + 1))
        done
        log "${tag} preview queued frames=${frames} ready=$(queue_count)"
    else
        log "${tag} preview decode failed"
        tail -20 "${FFMPEG_LOG}" 2>/dev/null || true
    fi
    rm -f "${raw}"
}

capture_image_segment() {
    segment="$1"
    base="${segment%.*}"
    ppm="${base}.ppm"
    raw="${base}.rgb565"
    tmp="${segment}.tmp.jpg"
    camera_index="$(camera_device_index)"

    rm -f "${ppm}" "${raw}" "${tmp}"
    transform_args="--rotate ${CAMERA_ROTATE}"
    if [ "${CAMERA_FLIP_X}" = "1" ]; then
        transform_args="${transform_args} --flip-x"
    fi
    if [ "${CAMERA_FLIP_Y}" = "1" ]; then
        transform_args="${transform_args} --flip-y"
    fi
    if ! "${CAPTURE_BIN}" -d "${camera_index}" \
        -w "${CAPTURE_WIDTH}" -h "${CAPTURE_HEIGHT}" -f "${CAPTURE_FORMAT}" \
        -o "${ppm}" -t "${raw}" \
        --thumb-width "${PREVIEW_WIDTH}" --thumb-height "${PREVIEW_HEIGHT}" \
        --skip 1 ${transform_args} > "${FFMPEG_LOG}" 2>&1; then
        log "camera capture helper failed"
        tail -20 "${FFMPEG_LOG}" 2>/dev/null || true
        rm -f "${ppm}" "${raw}" "${tmp}"
        return 1
    fi

    queue_single_frame "${raw}" || log "local preview queue failed"

    if [ "${TILE_STREAM}" = "1" ] && [ "${ROLE}" = "tx" ]; then
        mv "${raw}" "${segment}"
        rm -f "${ppm}" "${tmp}"
        return 0
    fi

    if ! "${FFMPEG_BIN}" -y -nostdin -hide_banner -loglevel error \
        -i "${ppm}" \
        -vf "scale=${ENCODE_WIDTH}:${ENCODE_HEIGHT}:force_original_aspect_ratio=decrease:flags=fast_bilinear,pad=${ENCODE_WIDTH}:${ENCODE_HEIGHT}:(ow-iw)/2:(oh-ih)/2" \
        -frames:v 1 -q:v "${JPEG_QUALITY}" "${tmp}" \
        >> "${FFMPEG_LOG}" 2>&1; then
        log "jpeg encode failed"
        tail -20 "${FFMPEG_LOG}" 2>/dev/null || true
        rm -f "${ppm}" "${raw}" "${tmp}"
        return 1
    fi

    mv "${tmp}" "${segment}"
    rm -f "${ppm}" "${raw}" "${tmp}"
    return 0
}

capture_with_encoder() {
    encoder="$1"
    segment="$2"
    gop=$((FPS * SEGMENT_SECONDS))

    if [ "${gop}" -lt "${FPS}" ]; then
        gop="${FPS}"
    fi
    "${FFMPEG_BIN}" -y -nostdin -hide_banner -loglevel error \
        -use_wallclock_as_timestamps 1 -fflags +genpts \
        -f v4l2 -framerate "${FPS}" -video_size "${WIDTH}x${HEIGHT}" \
        -pixel_format nv12 -i "${CAMERA_DEV}" -t "${SEGMENT_SECONDS}" \
        -vf "scale=${ENCODE_WIDTH}:${ENCODE_HEIGHT}:flags=fast_bilinear" \
        -an -c:v "${encoder}" -b:v "${BITRATE_KBPS}k" \
        -maxrate "${BITRATE_KBPS}k" -bufsize "$((BITRATE_KBPS * 2))k" \
        -g "${gop}" -bf 0 -r "${FPS}" -vsync cfr \
        -movflags +faststart "${segment}" > "${FFMPEG_LOG}" 2>&1
}

capture_segment() {
    segment="$1"

    case "${CODEC}" in
        h265) ENCODER="hevc_v4l2m2m" ;;
        h264) ENCODER="h264_v4l2m2m" ;;
    esac

    if capture_with_encoder "${ENCODER}" "${segment}"; then
        return 0
    fi
    if [ "${CODEC}" = "h265" ]; then
        log "h265 capture failed, retry h264 fallback"
        tail -12 "${FFMPEG_LOG}" 2>/dev/null || true
        CODEC="h264"
        ENCODER="h264_v4l2m2m"
        capture_with_encoder "${ENCODER}" "${segment}"
        return "$?"
    fi
    return 1
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --role) ROLE="$2"; shift 2 ;;
        --codec) CODEC="$2"; shift 2 ;;
        --stream-format) STREAM_FORMAT="$2"; shift 2 ;;
        --width) WIDTH="$2"; shift 2 ;;
        --height) HEIGHT="$2"; shift 2 ;;
        --capture-width) CAPTURE_WIDTH="$2"; shift 2 ;;
        --capture-height) CAPTURE_HEIGHT="$2"; shift 2 ;;
        --encode-width) ENCODE_WIDTH="$2"; shift 2 ;;
        --encode-height) ENCODE_HEIGHT="$2"; shift 2 ;;
        --jpeg-quality) JPEG_QUALITY="$2"; shift 2 ;;
        --fps) FPS="$2"; shift 2 ;;
        --bitrate) BITRATE_KBPS="$2"; shift 2 ;;
        --segment) SEGMENT_SECONDS="$2"; shift 2 ;;
        --count) COUNT="$2"; shift 2 ;;
        --duration) DURATION="$2"; shift 2 ;;
        --freq) FREQ_MHZ="$2"; shift 2 ;;
        --br) BR_KBPS="$2"; shift 2 ;;
        --spi-hz) SPI_HZ="$2"; shift 2 ;;
        --out-dir) RX_DIR="$2"; shift 2 ;;
        --tx-dir) TX_DIR="$2"; shift 2 ;;
        --device) CAMERA_DEV="$2"; shift 2 ;;
        --preview-file) PREVIEW_FILE="$2"; shift 2 ;;
        --preview-width) PREVIEW_WIDTH="$2"; shift 2 ;;
        --preview-height) PREVIEW_HEIGHT="$2"; shift 2 ;;
        --preview-fps) PREVIEW_FPS="$2"; shift 2 ;;
        --preview-buffer) PREVIEW_BUFFER_MIN="$2"; shift 2 ;;
        --tile-w) TILE_WIDTH="$2"; shift 2 ;;
        --tile-h) TILE_HEIGHT="$2"; shift 2 ;;
        --tile-repeat) TILE_REPEAT="$2"; shift 2 ;;
        --tile-gap-us) TILE_GAP_US="$2"; shift 2 ;;
        --tile-frame-gap-us) TILE_FRAME_GAP_US="$2"; shift 2 ;;
        --stop-file) STOP_FILE="$2"; shift 2 ;;
        --camera-rotate) CAMERA_ROTATE="$2"; shift 2 ;;
        --camera-flip-x) CAMERA_FLIP_X="1"; shift ;;
        --camera-flip-y) CAMERA_FLIP_Y="1"; shift ;;
        --fast-frame) FAST_FRAME="1"; shift ;;
        --compressed-image-stream)
            COMPRESSED_IMAGE_STREAM="1"
            FAST_FRAME="1"
            TILE_STREAM="0"
            STREAM2_FRAME="1"
            shift ;;
        --legacy-frame-stream) STREAM2_FRAME="0"; shift ;;
        --tile-stream) TILE_STREAM="1"; shift ;;
        --no-tile-stream) TILE_STREAM="0"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) log "unknown argument: $1"; usage; exit 2 ;;
    esac
done

case "${ROLE}" in
    tx|rx|capture) ;;
    *) log "invalid role: ${ROLE}"; usage; exit 2 ;;
esac

case "${CODEC}" in
    h264|h265) ;;
    *) log "invalid codec: ${CODEC}"; exit 2 ;;
esac

case "${STREAM_FORMAT}" in
    image|video) ;;
    *) log "invalid stream format: ${STREAM_FORMAT}"; exit 2 ;;
esac

case "${CAMERA_ROTATE}" in
    0|90|180|270) ;;
    *) log "invalid camera rotate: ${CAMERA_ROTATE}"; exit 2 ;;
esac

trap cleanup INT TERM EXIT
rm -f "${STOP_FILE}"

if [ "${ROLE}" = "rx" ]; then
    if [ "${TILE_STREAM}" = "1" ]; then
        need_bin "${FLRC_TILE_BIN}"
    else
        need_bin "${FLRC_BIN}"
    fi
    need_bin "${FFMPEG_BIN}"
    mkdir -p "${RX_DIR}"
    rm -f "${RX_DIR}"/flrc_cam_* "${RX_DIR}"/flrc_cam_*.flrc.part \
          "${RX_DIR}"/flrc_rx_* "${RX_DIR}"/flrc_rx_*.flrc.part \
          "${RX_DIR}"/*.jpg "${RX_DIR}"/*.jpg.tmp
    rm -f "${RX_INNER_LOG}"
    if [ "${TILE_STREAM}" = "1" ]; then
        rm -f "${PREVIEW_FILE}" "${PREVIEW_FILE}.tmp" "${PREVIEW_META}"
        log "RX tile listen duration=${DURATION}s freq=${FREQ_MHZ} br=${BR_KBPS} spi=${SPI_HZ} canvas=${PREVIEW_WIDTH}x${PREVIEW_HEIGHT} tile=${TILE_WIDTH}x${TILE_HEIGHT}"
        "${FLRC_TILE_BIN}" --role tile-rx \
            --preview "${PREVIEW_FILE}" \
            --width "${PREVIEW_WIDTH}" --height "${PREVIEW_HEIGHT}" \
            --tile-w "${TILE_WIDTH}" --tile-h "${TILE_HEIGHT}" \
            --freq "${FREQ_MHZ}" --br "${BR_KBPS}" --duration "${DURATION}" \
            --len "${PAYLOAD_LEN}" --spi-hz "${SPI_HZ}" --power "${POWER_DBM}" \
            --rx-poll-us "${RX_POLL_US}" \
            > "${RX_INNER_LOG}" 2>&1 &
        RX_PID="$!"
        tail -n +1 -f "${RX_INNER_LOG}" &
        TAIL_PID="$!"
        start_s="$(date +%s)"
        while [ ! -e "${STOP_FILE}" ]; do
            now_s="$(date +%s)"
            if [ $((now_s - start_s)) -ge "${DURATION}" ]; then
                break
            fi
            if ! kill -0 "${RX_PID}" >/dev/null 2>&1; then
                break
            fi
            printf 'frame_file=%s\nframes_ready=tile\nwidth=%s\nheight=%s\nrole=%s\ntile=1\n' \
                "${PREVIEW_FILE}" "${PREVIEW_WIDTH}" "${PREVIEW_HEIGHT}" \
                "${ROLE}" > "${PREVIEW_META}"
            sleep 0.5
        done
        log "RX stop"
        exit 0
    fi

    preview_player_start
    log "RX listen dir=${RX_DIR} duration=${DURATION}s freq=${FREQ_MHZ} br=${BR_KBPS} spi=${SPI_HZ} fast=${FAST_FRAME}"
    if [ "${STREAM2_FRAME}" = "1" ]; then
        "${FLRC_BIN}" --role frame-stream-rx \
            --freq "${FREQ_MHZ}" --br "${BR_KBPS}" --duration "${DURATION}" \
            --len "${PAYLOAD_LEN}" --spi-hz "${SPI_HZ}" --power "${POWER_DBM}" \
            --rx-poll-us "${RX_POLL_US}" --out-dir "${RX_DIR}" \
            --frame-width "${PREVIEW_WIDTH}" --frame-height "${PREVIEW_HEIGHT}" \
            > "${RX_INNER_LOG}" 2>&1 &
    else
        fast_arg=""
        if [ "${FAST_FRAME}" = "1" ]; then
            fast_arg="--fast-frame"
        fi
        "${FLRC_BIN}" --role stream-rx \
            --freq "${FREQ_MHZ}" --br "${BR_KBPS}" --duration "${DURATION}" \
            --len "${PAYLOAD_LEN}" --spi-hz "${SPI_HZ}" --power "${POWER_DBM}" \
            --rx-poll-us "${RX_POLL_US}" --retries "${RETRIES}" \
            --ack-wait-ms "${ACK_WAIT_MS}" --out-dir "${RX_DIR}" --keep-listening \
            ${fast_arg} \
            > "${RX_INNER_LOG}" 2>&1 &
    fi
    RX_PID="$!"
    tail -n +1 -f "${RX_INNER_LOG}" &
    TAIL_PID="$!"

    start_s="$(date +%s)"
    last_segment=""
    while [ ! -e "${STOP_FILE}" ]; do
        now_s="$(date +%s)"
        if [ $((now_s - start_s)) -ge "${DURATION}" ]; then
            break
        fi
        if ! kill -0 "${RX_PID}" >/dev/null 2>&1; then
            break
        fi

        segment="$(find "${RX_DIR}" -maxdepth 1 -type f \( -name 'flrc_cam_*' -o -name 'flrc_rx_*' -o -name '*.jpg' \) 2>/dev/null | grep -Ev '(\.flrc\.part|\.tmp)$' | sort | tail -n 1 || true)"
        if [ -n "${segment}" ] && [ "${segment}" != "${last_segment}" ]; then
            decode_preview_segment "${segment}" "rx"
            last_segment="${segment}"
        else
            sleep 0.2
        fi
    done
    log "RX stop"
    exit 0
fi

need_bin "${FFMPEG_BIN}"
if [ "${ROLE}" = "tx" ]; then
    if [ "${TILE_STREAM}" = "1" ]; then
        need_bin "${FLRC_TILE_BIN}"
    else
        need_bin "${FLRC_BIN}"
    fi
fi
if [ "${STREAM_FORMAT}" = "image" ]; then
    need_bin "${CAPTURE_BIN}"
fi
if [ ! -e "${CAMERA_DEV}" ]; then
    log "missing camera device: ${CAMERA_DEV}"
    exit 1
fi

case "${CODEC}" in
    h264) ENCODER="h264_v4l2m2m" ;;
    h265) ENCODER="hevc_v4l2m2m" ;;
esac

mkdir -p "${TX_DIR}"
preview_player_start
log "camera ${CAMERA_DEV} format=${STREAM_FORMAT} ${CODEC} capture=${WIDTH}x${HEIGHT}@${FPS} encode=${ENCODE_WIDTH}x${ENCODE_HEIGHT} ${BITRATE_KBPS}kbps jpeg_q=${JPEG_QUALITY} preview=${PREVIEW_WIDTH}x${PREVIEW_HEIGHT}@${PREVIEW_FPS} fast=${FAST_FRAME} tile=${TILE_STREAM} compressed=${COMPRESSED_IMAGE_STREAM} rotate=${CAMERA_ROTATE} flip_x=${CAMERA_FLIP_X} flip_y=${CAMERA_FLIP_Y}"
if [ "${ROLE}" = "tx" ] &&
   { [ "${FAST_FRAME}" = "1" ] || [ "${TILE_STREAM}" = "1" ]; }; then
    tx_sender_start
fi

if [ "${ROLE}" = "tx" ] && [ "${STREAM_FORMAT}" = "image" ] &&
   [ "${TILE_STREAM}" = "1" ]; then
    camera_index="$(camera_device_index)"
    capture_rc=0
    transform_args="--rotate ${CAMERA_ROTATE}"
    if [ "${CAMERA_FLIP_X}" = "1" ]; then
        transform_args="${transform_args} --flip-x"
    fi
    if [ "${CAMERA_FLIP_Y}" = "1" ]; then
        transform_args="${transform_args} --flip-y"
    fi

    log "continuous tile capture start dir=${TX_QUEUE_DIR} preview=${PREVIEW_FILE} fps=${PREVIEW_FPS}"
    "${CAPTURE_BIN}" -d "${camera_index}" \
        -w "${CAPTURE_WIDTH}" -h "${CAPTURE_HEIGHT}" -f "${CAPTURE_FORMAT}" \
        --thumb-width "${PREVIEW_WIDTH}" --thumb-height "${PREVIEW_HEIGHT}" \
        --skip 1 --stream-dir "${TX_QUEUE_DIR}" --stream-prefix flrc_cam \
        --stream-duration "${DURATION}" --stream-fps "${PREVIEW_FPS}" \
        --stream-preview "${PREVIEW_FILE}" --stream-meta "${PREVIEW_META}" \
        --stream-max-files 4 ${transform_args} > "${FFMPEG_LOG}" 2>&1 &
    CAPTURE_PID="$!"
    wait "${CAPTURE_PID}" || capture_rc="$?"
    CAPTURE_PID=""
    log "continuous tile capture exited rc=${capture_rc}"
    if [ "${capture_rc}" != "0" ]; then
        tail -20 "${FFMPEG_LOG}" 2>/dev/null || true
    fi
    exit "${capture_rc}"
fi

if [ "${ROLE}" = "tx" ] && [ "${STREAM_FORMAT}" = "image" ] &&
   [ "${COMPRESSED_IMAGE_STREAM}" = "1" ]; then
    camera_index="$(camera_device_index)"
    capture_rc=0
    transform_args="--rotate ${CAMERA_ROTATE}"
    if [ "${CAMERA_FLIP_X}" = "1" ]; then
        transform_args="${transform_args} --flip-x"
    fi
    if [ "${CAMERA_FLIP_Y}" = "1" ]; then
        transform_args="${transform_args} --flip-y"
    fi

    log "continuous jpeg capture start dir=${TX_QUEUE_DIR} preview=${PREVIEW_FILE} fps=${PREVIEW_FPS} q=${JPEG_QUALITY}"
    "${CAPTURE_BIN}" -d "${camera_index}" \
        -w "${CAPTURE_WIDTH}" -h "${CAPTURE_HEIGHT}" -f "${CAPTURE_FORMAT}" \
        --thumb-width "${PREVIEW_WIDTH}" --thumb-height "${PREVIEW_HEIGHT}" \
        --skip 1 --stream-dir "${TX_QUEUE_DIR}" --stream-prefix flrc_cam \
        --stream-output jpeg --jpeg-quality "${JPEG_QUALITY}" \
        --stream-duration "${DURATION}" --stream-fps "${PREVIEW_FPS}" \
        --stream-preview "${PREVIEW_FILE}" --stream-meta "${PREVIEW_META}" \
        --stream-max-files 3 ${transform_args} > "${FFMPEG_LOG}" 2>&1 &
    CAPTURE_PID="$!"
    wait "${CAPTURE_PID}" || capture_rc="$?"
    CAPTURE_PID=""
    log "continuous jpeg capture exited rc=${capture_rc}"
    if [ "${capture_rc}" != "0" ]; then
        tail -20 "${FFMPEG_LOG}" 2>/dev/null || true
    fi
    exit "${capture_rc}"
fi

index=0
while [ ! -e "${STOP_FILE}" ]; do
    if [ "${COUNT}" != "0" ] && [ "${index}" -ge "${COUNT}" ]; then
        break
    fi

    if [ "${STREAM_FORMAT}" = "image" ]; then
        if [ "${TILE_STREAM}" = "1" ] && [ "${ROLE}" = "tx" ]; then
            segment="$(printf '%s/flrc_cam_%06d.raw' "${TX_DIR}" "${index}")"
        else
            segment="$(printf '%s/flrc_cam_%06d.jpg' "${TX_DIR}" "${index}")"
        fi
    else
        segment="$(printf '%s/flrc_cam_%06d.mp4' "${TX_DIR}" "${index}")"
    fi
    rm -f "${segment}"
    log "capture segment ${index}: ${segment}"
    if [ "${STREAM_FORMAT}" = "image" ]; then
        capture_image_segment "${segment}" || capture_rc="$?"
    else
        capture_segment "${segment}" || capture_rc="$?"
    fi
    if [ "${capture_rc:-0}" != "0" ]; then
        log "capture failed rc=${capture_rc}"
        tail -20 "${FFMPEG_LOG}" 2>/dev/null || true
        exit 1
    fi
    capture_rc=0

    bytes="$(wc -c < "${segment}" 2>/dev/null || echo 0)"
    log "segment ${index} size=${bytes} bytes"
    if [ "${STREAM_FORMAT}" != "image" ]; then
        decode_preview_segment "${segment}" "${ROLE}"
    fi
    if [ "${ROLE}" = "tx" ]; then
        if [ "${FAST_FRAME}" = "1" ] || [ "${TILE_STREAM}" = "1" ]; then
            queued="${TX_QUEUE_DIR}/$(basename "${segment}")"
            queued_tmp="${queued}.tmp"
            cp "${segment}" "${queued_tmp}" 2>/dev/null &&
                mv "${queued_tmp}" "${queued}" &&
                rm -f "${segment}"
            tx_queue_trim
            log "queue segment ${index} ready=$(tx_queue_count)"
            if ! kill -0 "${TX_SENDER_PID}" >/dev/null 2>&1; then
                log "TX sender exited"
                break
            fi
        else
            log "send segment ${index}"
            "${FLRC_BIN}" --role stream-tx --file "${segment}" \
                --freq "${FREQ_MHZ}" --br "${BR_KBPS}" --duration 1 \
                --len "${PAYLOAD_LEN}" --spi-hz "${SPI_HZ}" --power "${POWER_DBM}" \
                --rx-poll-us "${RX_POLL_US}" --retries "${RETRIES}" \
                --ack-wait-ms "${ACK_WAIT_MS}" || {
                rc="$?"
                log "send segment ${index} failed rc=${rc}; continue"
            }
        fi
    fi

    index=$((index + 1))
done

if [ "${ROLE}" = "tx" ] && [ "${FAST_FRAME}" = "1" ] &&
   [ "${COUNT}" != "0" ] && [ -n "${TX_SENDER_PID}" ]; then
    drain=0
    while [ "$(tx_queue_count)" -gt 0 ] && [ "${drain}" -lt 50 ]; do
        if ! kill -0 "${TX_SENDER_PID}" >/dev/null 2>&1; then
            break
        fi
        if command -v usleep >/dev/null 2>&1; then
            usleep 100000
        else
            sleep 1
        fi
        drain=$((drain + 1))
    done
    log "queue drain ready=$(tx_queue_count)"
fi

log "done role=${ROLE} segments=${index}"
