#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TX_HOST=""
RX_HOST=""
REMOTE_FILE=""
DURATION=120
PAYLOAD_LEN=252
POWER_DBM=8
RX_POLL_US=50
FREQ_MHZ=2400
BR_KBPS=2600
SPI_HZ=16000000
MAX_RETRIES=6
ACK_WAIT_MS=2500
DO_DEPLOY=0
RESTART_UI=1
CONNECT_TIMEOUT="${CONNECT_TIMEOUT:-8}"
SSH_KEY="${SSH_KEY:-}"
SSH_PORT="${SSH_PORT:-}"
RESULT_DIR="${RESULT_DIR:-/root/work/k230-script/results}"
REMOTE_APP_DIR="${REMOTE_APP_DIR:-/root/app/k230_phone_ui}"
REMOTE_OUT_DIR="${REMOTE_OUT_DIR:-/root/videos/flrc_rx}"
VIDEO_BIN="${VIDEO_BIN:-k230_lora_flrc_video}"

usage() {
    cat <<'USAGE'
Usage:
  lora_flrc_video_transfer.sh --tx <ip> --rx <ip> [options]

Runs a two-board LR2021 FLRC video-file transfer test over SSH.
The transport sends START/DATA/END packets with per-packet CRC32, then repairs
missing/corrupt chunks through RX NACK and TX retransmission until RX DONE.

Options:
  --tx HOST                 TX board, IP or root@IP.
  --rx HOST                 RX board, IP or root@IP.
  --file PATH               Remote video file on TX. If omitted, the script
                            uses the smallest /root/videos/*.mp4 on TX.
  --duration SEC            RX wait timeout. Default: 120.
  --out-dir DIR             Remote RX output directory.
                            Default: /root/videos/flrc_rx.
  --freq MHz                FLRC frequency. Default: 2400.
  --br KBPS                 FLRC bitrate. Default: 2600.
  --spi-hz HZ               SPI speed. Default: 16000000.
  --len BYTES               FLRC packet length. Default: 252.
  --power DBM               LR2021 16E8 HF safe power. Default: 8.
  --rx-poll-us USEC         RX DIO poll interval. Default: 50.
  --retries N               ACK/NACK repair rounds. Default: 6.
  --ack-wait-ms MS          TX wait time for RX DONE/NACK. Default: 2500.
  --deploy                  Deploy current built launcher to both boards first.
  --no-restart-ui           Leave launcher stopped after test.
  --result-dir DIR          Local result directory.
                            Default: /root/work/k230-script/results.
  --ssh-key FILE            SSH private key.
  --ssh-port PORT           SSH port.
  --connect-timeout SEC     SSH connect timeout. Default: 8.
  -h, --help                Show help.

Example:
  ./lora_flrc_video_transfer.sh --tx 192.168.100.210 --rx 192.168.100.237 \
      --file /root/videos/video02.mp4 --duration 120
USAGE
}

die() {
    echo "error: $*" >&2
    exit 1
}

normalize_host() {
    local host="$1"
    if [[ "${host}" != *@* ]]; then
        host="root@${host}"
    fi
    printf '%s\n' "${host}"
}

host_addr_only() {
    local host="$1"
    printf '%s\n' "${host#*@}"
}

ssh_run() {
    local host="$1"
    shift
    ssh "${SSH_OPTS[@]}" "${host}" "$@"
}

remote_stop_ui() {
    local host="$1"
    ssh_run "${host}" "if [ -x /etc/init.d/S99zz_k230_phone_ui ]; then /etc/init.d/S99zz_k230_phone_ui stop >/tmp/k230_flrc_video_stop_ui.log 2>&1 || true; fi; killall -q k230_phone_ui k230_phone_ui_fullswitch_test ${VIDEO_BIN} k230_lora_flrc_bench || true; rm -f /tmp/k230_lora_flrc_video_*.log /tmp/k230_lora_flrc_video_*.pid; sync"
}

remote_start_ui() {
    local host="$1"
    ssh_run "${host}" "if [ -x /etc/init.d/S99zz_k230_phone_ui ]; then /etc/init.d/S99zz_k230_phone_ui start >/tmp/k230_flrc_video_start_ui.log 2>&1 || true; elif [ -x '${REMOTE_APP_DIR}/k230_phone_ui' ]; then mkdir -p /var/log; (cd '${REMOTE_APP_DIR}' && HOME=/root nohup ./k230_phone_ui >>/var/log/k230_phone_ui.log 2>&1 &); fi"
}

extract_result() {
    local file="$1"
    awk '/^RESULT / { line=$0 } END { if(line) print line; else print "RESULT missing" }' "${file}"
}

field_value() {
    local line="$1"
    local key="$2"
    awk -v k="${key}" '{
        for(i = 1; i <= NF; i++) {
            split($i, a, "=");
            if(a[1] == k) {
                print a[2];
                exit;
            }
        }
    }' <<<"${line}"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tx)
            [[ $# -ge 2 ]] || die "--tx requires a value"
            TX_HOST="$2"
            shift 2
            ;;
        --rx)
            [[ $# -ge 2 ]] || die "--rx requires a value"
            RX_HOST="$2"
            shift 2
            ;;
        --file)
            [[ $# -ge 2 ]] || die "--file requires a value"
            REMOTE_FILE="$2"
            shift 2
            ;;
        --duration)
            [[ $# -ge 2 ]] || die "--duration requires a value"
            DURATION="$2"
            shift 2
            ;;
        --out-dir)
            [[ $# -ge 2 ]] || die "--out-dir requires a value"
            REMOTE_OUT_DIR="$2"
            shift 2
            ;;
        --freq)
            [[ $# -ge 2 ]] || die "--freq requires a value"
            FREQ_MHZ="$2"
            shift 2
            ;;
        --br)
            [[ $# -ge 2 ]] || die "--br requires a value"
            BR_KBPS="$2"
            shift 2
            ;;
        --spi-hz)
            [[ $# -ge 2 ]] || die "--spi-hz requires a value"
            SPI_HZ="$2"
            shift 2
            ;;
        --len)
            [[ $# -ge 2 ]] || die "--len requires a value"
            PAYLOAD_LEN="$2"
            shift 2
            ;;
        --power)
            [[ $# -ge 2 ]] || die "--power requires a value"
            POWER_DBM="$2"
            shift 2
            ;;
        --rx-poll-us)
            [[ $# -ge 2 ]] || die "--rx-poll-us requires a value"
            RX_POLL_US="$2"
            shift 2
            ;;
        --retries)
            [[ $# -ge 2 ]] || die "--retries requires a value"
            MAX_RETRIES="$2"
            shift 2
            ;;
        --ack-wait-ms)
            [[ $# -ge 2 ]] || die "--ack-wait-ms requires a value"
            ACK_WAIT_MS="$2"
            shift 2
            ;;
        --deploy)
            DO_DEPLOY=1
            shift
            ;;
        --no-restart-ui)
            RESTART_UI=0
            shift
            ;;
        --result-dir)
            [[ $# -ge 2 ]] || die "--result-dir requires a value"
            RESULT_DIR="$2"
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
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown argument: $1"
            ;;
    esac
done

[[ -n "${TX_HOST}" ]] || die "missing --tx"
[[ -n "${RX_HOST}" ]] || die "missing --rx"
[[ "${DURATION}" =~ ^[0-9]+$ ]] || die "--duration must be seconds"
[[ "${PAYLOAD_LEN}" =~ ^[0-9]+$ ]] || die "--len must be numeric"
[[ "${POWER_DBM}" =~ ^-?[0-9]+$ ]] || die "--power must be numeric"
[[ "${RX_POLL_US}" =~ ^[0-9]+$ ]] || die "--rx-poll-us must be numeric"
[[ "${MAX_RETRIES}" =~ ^[0-9]+$ ]] || die "--retries must be numeric"
[[ "${ACK_WAIT_MS}" =~ ^[0-9]+$ ]] || die "--ack-wait-ms must be numeric"
[[ "${DURATION}" -gt 0 ]] || die "--duration must be positive"
[[ "${PAYLOAD_LEN}" -gt 24 && "${PAYLOAD_LEN}" -le 255 ]] || die "--len must be 25..255"
[[ "${MAX_RETRIES}" -gt 0 ]] || die "--retries must be positive"
[[ "${ACK_WAIT_MS}" -ge 100 ]] || die "--ack-wait-ms must be at least 100"

TX_HOST="$(normalize_host "${TX_HOST}")"
RX_HOST="$(normalize_host "${RX_HOST}")"
TX_ADDR="$(host_addr_only "${TX_HOST}")"
RX_ADDR="$(host_addr_only "${RX_HOST}")"
[[ "${TX_ADDR}" != "${RX_ADDR}" ]] || die "TX and RX must be different devices; both are ${TX_ADDR}"

SSH_OPTS=(-o BatchMode=yes -o StrictHostKeyChecking=no
          -o UserKnownHostsFile=/dev/null -o ConnectTimeout="${CONNECT_TIMEOUT}")
if [[ -n "${SSH_KEY}" ]]; then
    SSH_OPTS+=(-i "${SSH_KEY}")
fi
if [[ -n "${SSH_PORT}" ]]; then
    SSH_OPTS+=(-p "${SSH_PORT}")
fi

if [[ -z "${REMOTE_FILE}" ]]; then
    REMOTE_FILE="$(ssh_run "${TX_HOST}" "for f in /root/videos/*.mp4; do [ -f \"\$f\" ] || continue; wc -c \"\$f\"; done | sort -n | awk 'NR==1 {print \$2}'")"
fi
[[ -n "${REMOTE_FILE}" ]] || die "no remote input file; pass --file"

TS="$(date -u +%Y%m%d_%H%M%S)"
RUN_DIR="${RESULT_DIR}/lora_flrc_video_${TS}"
mkdir -p "${RUN_DIR}"

cat <<EOF
TX        : ${TX_HOST}
RX        : ${RX_HOST}
File      : ${REMOTE_FILE}
RX out    : ${REMOTE_OUT_DIR}
Duration  : ${DURATION}s
FLRC      : ${FREQ_MHZ} MHz, ${BR_KBPS} kbps, SPI ${SPI_HZ}, len ${PAYLOAD_LEN}, power ${POWER_DBM} dBm
Reliable  : retries ${MAX_RETRIES}, ACK wait ${ACK_WAIT_MS} ms
Results   : ${RUN_DIR}
EOF

echo
echo "[1/6] Check SSH and video binary"
ssh_run "${TX_HOST}" "echo TX online; test -x '${REMOTE_APP_DIR}/${VIDEO_BIN}'; ls -lh '${REMOTE_FILE}'; '${REMOTE_APP_DIR}/${VIDEO_BIN}' --help | sed -n '1,14p'" | tee "${RUN_DIR}/tx_system.txt"
ssh_run "${RX_HOST}" "echo RX online; test -x '${REMOTE_APP_DIR}/${VIDEO_BIN}'; mkdir -p '${REMOTE_OUT_DIR}'; '${REMOTE_APP_DIR}/${VIDEO_BIN}' --help | sed -n '1,14p'" | tee "${RUN_DIR}/rx_system.txt"

if [[ "${DO_DEPLOY}" -eq 1 ]]; then
    echo
    echo "[2/6] Deploy current built launcher to both boards"
    "${SCRIPT_DIR}/deploy_launcher.sh" "${RX_HOST}" --no-build
    "${SCRIPT_DIR}/deploy_launcher.sh" "${TX_HOST}" --no-build
else
    echo
    echo "[2/6] Skip deploy"
fi

echo
echo "[3/6] Stop launcher on both boards"
remote_stop_ui "${RX_HOST}"
remote_stop_ui "${TX_HOST}"

trap 'echo; echo "cleanup: stop transfer and restart UI"; remote_stop_ui "${RX_HOST}" || true; remote_stop_ui "${TX_HOST}" || true; if [[ "${RESTART_UI}" -eq 1 ]]; then remote_start_ui "${RX_HOST}" || true; remote_start_ui "${TX_HOST}" || true; fi' EXIT

RX_LOG="${RUN_DIR}/rx_file.log"
TX_LOG="${RUN_DIR}/tx_file.log"
REMOTE_RX_LOG="/tmp/k230_lora_flrc_video_rx_${TS}.log"
REMOTE_TX_LOG="/tmp/k230_lora_flrc_video_tx_${TS}.log"

echo
echo "[4/6] Start RX"
ssh_run "${RX_HOST}" "killall -q ${VIDEO_BIN} || true; rm -f '${REMOTE_RX_LOG}'; mkdir -p '${REMOTE_OUT_DIR}'; cd '${REMOTE_APP_DIR}' && (./${VIDEO_BIN} --role stream-rx --freq '${FREQ_MHZ}' --br '${BR_KBPS}' --duration '${DURATION}' --len '${PAYLOAD_LEN}' --spi-hz '${SPI_HZ}' --power '${POWER_DBM}' --rx-poll-us '${RX_POLL_US}' --retries '${MAX_RETRIES}' --ack-wait-ms '${ACK_WAIT_MS}' --out-dir '${REMOTE_OUT_DIR}' > '${REMOTE_RX_LOG}' 2>&1 & echo \$! >/tmp/k230_lora_flrc_video_rx.pid)"
sleep 1

echo
echo "[5/6] Run TX"
ssh_run "${TX_HOST}" "killall -q ${VIDEO_BIN} || true; rm -f '${REMOTE_TX_LOG}'; cd '${REMOTE_APP_DIR}' && ./${VIDEO_BIN} --role stream-tx --freq '${FREQ_MHZ}' --br '${BR_KBPS}' --duration 1 --len '${PAYLOAD_LEN}' --spi-hz '${SPI_HZ}' --power '${POWER_DBM}' --rx-poll-us '${RX_POLL_US}' --retries '${MAX_RETRIES}' --ack-wait-ms '${ACK_WAIT_MS}' --file '${REMOTE_FILE}' > '${REMOTE_TX_LOG}' 2>&1" || true
sleep 2
ssh_run "${RX_HOST}" "killall -q ${VIDEO_BIN} || true; cat '${REMOTE_RX_LOG}' 2>/dev/null || true" > "${RX_LOG}"
ssh_run "${TX_HOST}" "cat '${REMOTE_TX_LOG}' 2>/dev/null || true" > "${TX_LOG}"

TX_RESULT="$(extract_result "${TX_LOG}")"
RX_RESULT="$(extract_result "${RX_LOG}")"
RX_PATH="$(field_value "${RX_RESULT}" path)"

echo "TX: ${TX_RESULT}" | tee "${RUN_DIR}/summary.txt"
echo "RX: ${RX_RESULT}" | tee -a "${RUN_DIR}/summary.txt"
if [[ -n "${RX_PATH}" && "${RX_PATH}" != "none" ]]; then
    ssh_run "${RX_HOST}" "ls -lh '${RX_PATH}' 2>/dev/null || true; sha256sum '${RX_PATH}' 2>/dev/null || true" | tee -a "${RUN_DIR}/summary.txt"
fi
ssh_run "${TX_HOST}" "sha256sum '${REMOTE_FILE}' 2>/dev/null || true" | tee -a "${RUN_DIR}/summary.txt"

echo
echo "[6/6] Restart launcher"
if [[ "${RESTART_UI}" -eq 1 ]]; then
    remote_start_ui "${RX_HOST}"
    remote_start_ui "${TX_HOST}"
else
    echo "Skipped by --no-restart-ui"
fi
trap - EXIT

echo
cat "${RUN_DIR}/summary.txt"
echo
echo "Saved logs under ${RUN_DIR}"
