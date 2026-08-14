#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TX_HOST=""
RX_HOST=""
DURATION=15
PAYLOAD_LEN=252
POWER_DBM=8
RX_POLL_US=50
FREQ_LIST="2400"
BR_LIST="2600,1300,650"
SPI_LIST="4000000,8000000,12000000"
DO_DEPLOY=0
RESTART_UI=1
CONNECT_TIMEOUT="${CONNECT_TIMEOUT:-8}"
SSH_KEY="${SSH_KEY:-}"
SSH_PORT="${SSH_PORT:-}"
RESULT_DIR="${RESULT_DIR:-/root/work/k230-script/results}"
REMOTE_APP_DIR="${REMOTE_APP_DIR:-/root/app/k230_phone_ui}"
BENCH_BIN="${BENCH_BIN:-k230_lora_flrc_bench}"

usage() {
    cat <<'USAGE'
Usage:
  lora_flrc_pair_test.sh --tx <ip> --rx <ip> [options]

Runs an unattended two-board LR2021 FLRC throughput test over SSH.
The script stops the LVGL launcher on both boards during the test so the
standalone benchmark binary can own the LoRa SPI/GPIO resources.

Options:
  --tx HOST                 TX board, IP or root@IP.
  --rx HOST                 RX board, IP or root@IP.
  --duration SEC            Per-combination duration. Default: 15.
  --len BYTES               FLRC payload length, max 255. Default: 252.
  --power DBM               LR2021 16E8 HF safe power. Default: 8.
  --rx-poll-us USEC         RX DIO poll interval. Default: 50.
  --freq-list LIST          Comma-separated MHz list. Default: 2400.
  --br-list LIST            Comma-separated kbps list. Default: 2600,1300,650.
  --spi-list LIST           Comma-separated SPI Hz list.
                            Default: 4000000,8000000,12000000.
  --deploy                  Deploy current built launcher to both boards first.
  --no-restart-ui           Leave launcher stopped after test.
  --result-dir DIR          Local result directory.
                            Default: /root/work/k230-script/results.
  --ssh-key FILE            SSH private key.
  --ssh-port PORT           SSH port.
  --connect-timeout SEC     SSH connect timeout. Default: 8.
  -h, --help                Show help.

Example:
  ./lora_flrc_pair_test.sh --rx 192.168.100.237 --tx 192.168.100.210 \
      --duration 20 --freq-list 2400,2450 --br-list 2600 --deploy
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

csv_to_array() {
    local csv="$1"
    local -n out_ref="$2"
    local old_ifs="${IFS}"
    IFS=','
    read -r -a out_ref <<<"${csv}"
    IFS="${old_ifs}"
}

remote_stop_ui() {
    local host="$1"
    ssh_run "${host}" "if [ -x /etc/init.d/S99zz_k230_phone_ui ]; then /etc/init.d/S99zz_k230_phone_ui stop >/tmp/k230_flrc_stop_ui.log 2>&1 || true; fi; killall -q k230_phone_ui k230_phone_ui_fullswitch_test ${BENCH_BIN} || true; rm -f /tmp/k230_lora_flrc_bench_*.log /tmp/k230_lora_flrc_*.pid; sync"
}

remote_start_ui() {
    local host="$1"
    ssh_run "${host}" "if [ -x /etc/init.d/S99zz_k230_phone_ui ]; then /etc/init.d/S99zz_k230_phone_ui start >/tmp/k230_flrc_start_ui.log 2>&1 || true; elif [ -x '${REMOTE_APP_DIR}/k230_phone_ui' ]; then mkdir -p /var/log; (cd '${REMOTE_APP_DIR}' && HOME=/root nohup ./k230_phone_ui >>/var/log/k230_phone_ui.log 2>&1 &); fi"
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
[[ "${DURATION}" -gt 0 ]] || die "--duration must be positive"
[[ "${PAYLOAD_LEN}" -gt 0 && "${PAYLOAD_LEN}" -le 255 ]] || die "--len must be 1..255"

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

declare -a FREQS BRS SPIS
csv_to_array "${FREQ_LIST}" FREQS
csv_to_array "${BR_LIST}" BRS
csv_to_array "${SPI_LIST}" SPIS

TS="$(date -u +%Y%m%d_%H%M%S)"
RUN_DIR="${RESULT_DIR}/lora_flrc_${TS}"
mkdir -p "${RUN_DIR}"
SUMMARY="${RUN_DIR}/summary.tsv"

cat <<EOF
TX          : ${TX_HOST}
RX          : ${RX_HOST}
Duration    : ${DURATION}s per combination
Payload     : ${PAYLOAD_LEN} bytes
Power       : ${POWER_DBM} dBm
RX poll     : ${RX_POLL_US} us
Frequencies : ${FREQ_LIST}
Bitrates    : ${BR_LIST}
SPI speeds  : ${SPI_LIST}
Results     : ${RUN_DIR}
EOF

echo
echo "[1/6] Check SSH and benchmark binary"
ssh_run "${TX_HOST}" "echo TX online; uname -a; test -x '${REMOTE_APP_DIR}/${BENCH_BIN}' && '${REMOTE_APP_DIR}/${BENCH_BIN}' --help | sed -n '1,12p'" | tee "${RUN_DIR}/tx_system.txt"
ssh_run "${RX_HOST}" "echo RX online; uname -a; test -x '${REMOTE_APP_DIR}/${BENCH_BIN}' && '${REMOTE_APP_DIR}/${BENCH_BIN}' --help | sed -n '1,12p'" | tee "${RUN_DIR}/rx_system.txt"

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

printf "freq_mhz\tbr_kbps\tspi_hz\ttx_mbps\trx_mbps\ttx_packets\trx_packets\ttx_errors\trx_errors\ttx_log\trx_log\n" > "${SUMMARY}"

echo
echo "[4/6] Run FLRC matrix"
trap 'echo; echo "cleanup: stop benchmark and restart UI"; remote_stop_ui "${RX_HOST}" || true; remote_stop_ui "${TX_HOST}" || true; if [[ "${RESTART_UI}" -eq 1 ]]; then remote_start_ui "${RX_HOST}" || true; remote_start_ui "${TX_HOST}" || true; fi' EXIT

for freq in "${FREQS[@]}"; do
    for br in "${BRS[@]}"; do
        for spi in "${SPIS[@]}"; do
            label="freq${freq}_br${br}_spi${spi}"
            rx_log="${RUN_DIR}/rx_${label}.log"
            tx_log="${RUN_DIR}/tx_${label}.log"
            remote_rx_log="/tmp/k230_lora_flrc_bench_rx_${label}.log"
            remote_tx_log="/tmp/k230_lora_flrc_bench_tx_${label}.log"

            echo
            echo "== ${label} =="
            ssh_run "${RX_HOST}" "killall -q ${BENCH_BIN} || true; rm -f '${remote_rx_log}'; cd '${REMOTE_APP_DIR}' && (./${BENCH_BIN} --role rx --freq '${freq}' --br '${br}' --duration '$((DURATION + 2))' --len '${PAYLOAD_LEN}' --spi-hz '${spi}' --power '${POWER_DBM}' --rx-poll-us '${RX_POLL_US}' > '${remote_rx_log}' 2>&1 & echo \$! >/tmp/k230_lora_flrc_rx.pid)"
            sleep 1
            ssh_run "${TX_HOST}" "killall -q ${BENCH_BIN} || true; rm -f '${remote_tx_log}'; cd '${REMOTE_APP_DIR}' && ./${BENCH_BIN} --role tx --freq '${freq}' --br '${br}' --duration '${DURATION}' --len '${PAYLOAD_LEN}' --spi-hz '${spi}' --power '${POWER_DBM}' --rx-poll-us '${RX_POLL_US}' > '${remote_tx_log}' 2>&1" || true
            sleep 1
            ssh_run "${RX_HOST}" "killall -q ${BENCH_BIN} || true; cat '${remote_rx_log}' 2>/dev/null || true" > "${rx_log}"
            ssh_run "${TX_HOST}" "cat '${remote_tx_log}' 2>/dev/null || true" > "${tx_log}"

            tx_result="$(extract_result "${tx_log}")"
            rx_result="$(extract_result "${rx_log}")"
            tx_mbps="$(field_value "${tx_result}" mbps)"
            rx_mbps="$(field_value "${rx_result}" mbps)"
            tx_packets="$(field_value "${tx_result}" packets)"
            rx_packets="$(field_value "${rx_result}" packets)"
            tx_errors="$(field_value "${tx_result}" errors)"
            rx_errors="$(field_value "${rx_result}" errors)"
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
                "${freq}" "${br}" "${spi}" "${tx_mbps:-NA}" "${rx_mbps:-NA}" \
                "${tx_packets:-NA}" "${rx_packets:-NA}" \
                "${tx_errors:-NA}" "${rx_errors:-NA}" \
                "${tx_log}" "${rx_log}" >> "${SUMMARY}"

            echo "TX: ${tx_result}"
            echo "RX: ${rx_result}"
        done
    done
done

echo
echo "[5/6] Restart launcher"
if [[ "${RESTART_UI}" -eq 1 ]]; then
    remote_start_ui "${RX_HOST}"
    remote_start_ui "${TX_HOST}"
else
    echo "Skipped by --no-restart-ui"
fi
trap - EXIT

echo
echo "[6/6] Summary"
column -t -s $'\t' "${SUMMARY}" 2>/dev/null || cat "${SUMMARY}"
echo
echo "Saved logs under ${RUN_DIR}"
