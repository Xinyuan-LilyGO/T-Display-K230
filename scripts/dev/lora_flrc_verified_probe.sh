#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TX_HOST=""
RX_HOST=""
REMOTE_SOURCE_FILE=""
PROBE_SIZE=65536
FREQ_LIST="2400"
BR_LIST="2600,1300,650"
SPI_LIST="8000000,12000000,16000000,20000000,24000000,32000000"
DURATION=120
PAYLOAD_LEN=252
POWER_DBM=8
RX_POLL_US=50
MAX_RETRIES=6
ACK_WAIT_MS=2500
CONNECT_TIMEOUT="${CONNECT_TIMEOUT:-8}"
SSH_KEY="${SSH_KEY:-}"
SSH_PORT="${SSH_PORT:-}"
RESULT_DIR="${RESULT_DIR:-/root/work/k230-script/results}"
REMOTE_PROBE_FILE="/root/videos/flrc_probe.bin"
RESTART_UI=1

usage() {
    cat <<'USAGE'
Usage:
  lora_flrc_verified_probe.sh --tx <ip> --rx <ip> [options]

Runs a payload-verified LR2021 FLRC probe. Each parameter set transfers a real
video-file slice through the reliable START/DATA/END + CRC + ACK/NACK path.
Only crc_ok=1 on RX counts as usable.

Options:
  --tx HOST                 TX board, IP or root@IP.
  --rx HOST                 RX board, IP or root@IP.
  --file PATH               Remote source video file on TX. Default: smallest
                            /root/videos/*.mp4 on TX.
  --probe-size BYTES        Bytes copied into /root/videos/flrc_probe.bin.
                            Default: 65536.
  --freq-list LIST          Comma list in MHz. Default: 2400.
  --br-list LIST            Comma list in kbps. Default: 2600,1300,650.
  --spi-list LIST           Comma list in Hz.
                            Default: 8000000,12000000,16000000,20000000,24000000,32000000.
  --duration SEC            RX wait timeout per run. Default: 120.
  --len BYTES               FLRC packet length. Default: 252.
  --power DBM               LR2021 16E8 HF safe power. Default: 8.
  --rx-poll-us USEC         RX DIO poll interval. Default: 50.
  --retries N               ACK/NACK repair rounds. Default: 6.
  --ack-wait-ms MS          TX wait time for RX DONE/NACK. Default: 2500.
  --result-dir DIR          Local result directory.
  --no-restart-ui           Leave launcher stopped after probing.
  --ssh-key FILE            SSH private key.
  --ssh-port PORT           SSH port.
  --connect-timeout SEC     SSH connect timeout. Default: 8.
  -h, --help                Show help.

Example:
  ./lora_flrc_verified_probe.sh --tx 192.168.100.210 --rx 192.168.100.237 \
      --br-list 2600 --spi-list 12000000,16000000,20000000 --probe-size 65536
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

split_csv() {
    local text="$1"
    local -n out_ref="$2"
    IFS=',' read -r -a out_ref <<<"${text}"
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
            REMOTE_SOURCE_FILE="$2"
            shift 2
            ;;
        --probe-size)
            [[ $# -ge 2 ]] || die "--probe-size requires a value"
            PROBE_SIZE="$2"
            shift 2
            ;;
        --freq-list)
            [[ $# -ge 2 ]] || die "--freq-list requires a value"
            FREQ_LIST="$2"
            shift 2
            ;;
        --br-list)
            [[ $# -ge 2 ]] || die "--br-list requires a value"
            BR_LIST="$2"
            shift 2
            ;;
        --spi-list)
            [[ $# -ge 2 ]] || die "--spi-list requires a value"
            SPI_LIST="$2"
            shift 2
            ;;
        --duration)
            [[ $# -ge 2 ]] || die "--duration requires a value"
            DURATION="$2"
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
        --result-dir)
            [[ $# -ge 2 ]] || die "--result-dir requires a value"
            RESULT_DIR="$2"
            shift 2
            ;;
        --no-restart-ui)
            RESTART_UI=0
            shift
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
[[ "${PROBE_SIZE}" =~ ^[0-9]+$ && "${PROBE_SIZE}" -gt 0 ]] || die "--probe-size must be positive"
[[ "${DURATION}" =~ ^[0-9]+$ && "${DURATION}" -gt 0 ]] || die "--duration must be positive"
[[ "${PAYLOAD_LEN}" =~ ^[0-9]+$ && "${PAYLOAD_LEN}" -gt 24 && "${PAYLOAD_LEN}" -le 255 ]] || die "--len must be 25..255"
[[ "${POWER_DBM}" =~ ^-?[0-9]+$ ]] || die "--power must be numeric"
[[ "${RX_POLL_US}" =~ ^[0-9]+$ ]] || die "--rx-poll-us must be numeric"
[[ "${MAX_RETRIES}" =~ ^[0-9]+$ && "${MAX_RETRIES}" -gt 0 ]] || die "--retries must be positive"
[[ "${ACK_WAIT_MS}" =~ ^[0-9]+$ && "${ACK_WAIT_MS}" -ge 100 ]] || die "--ack-wait-ms must be at least 100"

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

if [[ -z "${REMOTE_SOURCE_FILE}" ]]; then
    REMOTE_SOURCE_FILE="$(ssh_run "${TX_HOST}" "for f in /root/videos/*.mp4; do [ -f \"\$f\" ] || continue; wc -c \"\$f\"; done | sort -n | awk 'NR==1 {print \$2}'")"
fi
[[ -n "${REMOTE_SOURCE_FILE}" ]] || die "no remote source file; pass --file"

TS="$(date -u +%Y%m%d_%H%M%S)"
RUN_DIR="${RESULT_DIR}/lora_flrc_verified_probe_${TS}"
mkdir -p "${RUN_DIR}"

split_csv "${FREQ_LIST}" FREQS
split_csv "${BR_LIST}" BRS
split_csv "${SPI_LIST}" SPIS

echo "TX        : ${TX_HOST}"
echo "RX        : ${RX_HOST}"
echo "Source    : ${REMOTE_SOURCE_FILE}"
echo "Probe     : ${REMOTE_PROBE_FILE} (${PROBE_SIZE} bytes max)"
echo "Results   : ${RUN_DIR}"

ssh_run "${TX_HOST}" "mkdir -p /root/videos; rm -f '${REMOTE_PROBE_FILE}'; dd if='${REMOTE_SOURCE_FILE}' of='${REMOTE_PROBE_FILE}' bs=1 count='${PROBE_SIZE}' >/tmp/k230_flrc_probe_dd.log 2>&1; ls -lh '${REMOTE_PROBE_FILE}'; sha256sum '${REMOTE_PROBE_FILE}'" | tee "${RUN_DIR}/probe_source.txt"

if [[ "${RESTART_UI}" -eq 1 ]]; then
    trap 'ssh "${SSH_OPTS[@]}" "${RX_HOST}" "if [ -x /etc/init.d/S99zz_k230_phone_ui ]; then /etc/init.d/S99zz_k230_phone_ui start >/tmp/k230_flrc_probe_start_ui.log 2>&1 || true; fi" >/dev/null 2>&1 || true; ssh "${SSH_OPTS[@]}" "${TX_HOST}" "if [ -x /etc/init.d/S99zz_k230_phone_ui ]; then /etc/init.d/S99zz_k230_phone_ui start >/tmp/k230_flrc_probe_start_ui.log 2>&1 || true; fi" >/dev/null 2>&1 || true' EXIT
fi

TSV="${RUN_DIR}/results.tsv"
printf 'freq_mhz\tbr_kbps\tspi_hz\tcrc_ok\tfile_mbps\ttx_errors\trx_errors\ttx_retries\trx_retries\trun_dir\n' > "${TSV}"

for freq in "${FREQS[@]}"; do
    for br in "${BRS[@]}"; do
        for spi in "${SPIS[@]}"; do
            freq="${freq//[[:space:]]/}"
            br="${br//[[:space:]]/}"
            spi="${spi//[[:space:]]/}"
            [[ -n "${freq}" && -n "${br}" && -n "${spi}" ]] || continue

            echo
            echo "=== Probe freq=${freq} br=${br} spi=${spi} ==="
            RUN_LOG="${RUN_DIR}/probe_${freq}_${br}_${spi}.log"
            if "${SCRIPT_DIR}/lora_flrc_video_transfer.sh" \
                --tx "${TX_HOST}" --rx "${RX_HOST}" --file "${REMOTE_PROBE_FILE}" \
                --duration "${DURATION}" --freq "${freq}" --br "${br}" \
                --spi-hz "${spi}" --len "${PAYLOAD_LEN}" --power "${POWER_DBM}" \
                --rx-poll-us "${RX_POLL_US}" --retries "${MAX_RETRIES}" \
                --ack-wait-ms "${ACK_WAIT_MS}" --result-dir "${RUN_DIR}" \
                --no-restart-ui | tee "${RUN_LOG}"; then
                status=0
            else
                status=$?
            fi

            tx_result="$(grep '^TX: RESULT' "${RUN_LOG}" | tail -1 | sed 's/^TX: //')"
            rx_result="$(grep '^RX: RESULT' "${RUN_LOG}" | tail -1 | sed 's/^RX: //')"
            crc_ok="$(field_value "${rx_result}" crc_ok)"
            file_mbps="$(field_value "${tx_result}" file_mbps)"
            tx_errors="$(field_value "${tx_result}" errors)"
            rx_errors="$(field_value "${rx_result}" errors)"
            tx_retries="$(field_value "${tx_result}" retries)"
            rx_retries="$(field_value "${rx_result}" retries)"
            [[ -n "${crc_ok}" ]] || crc_ok=0
            [[ -n "${file_mbps}" ]] || file_mbps=0
            [[ -n "${tx_errors}" ]] || tx_errors=unknown
            [[ -n "${rx_errors}" ]] || rx_errors=unknown
            [[ -n "${tx_retries}" ]] || tx_retries=unknown
            [[ -n "${rx_retries}" ]] || rx_retries=unknown

            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                "${freq}" "${br}" "${spi}" "${crc_ok}" "${file_mbps}" \
                "${tx_errors}" "${rx_errors}" "${tx_retries}" "${rx_retries}" \
                "${RUN_LOG}" >> "${TSV}"

            if [[ "${status}" -ne 0 ]]; then
                echo "probe command exited with ${status}; continuing"
            fi
        done
    done
done

echo
echo "Verified probe results:"
column -t -s $'\t' "${TSV}" 2>/dev/null || cat "${TSV}"
