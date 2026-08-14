#!/usr/bin/env bash
set -euo pipefail

SSH_USER="${SSH_USER:-root}"
SSH_OPTS=(
    -o StrictHostKeyChecking=no
    -o UserKnownHostsFile=/dev/null
    -o ConnectTimeout="${CONNECT_TIMEOUT:-8}"
)
APP_DIR="${APP_DIR:-/root/app/k230_phone_ui}"
PROBE="${APP_DIR}/k230_meshtastic_probe"
REGION="${REGION:-EU_868}"
PRESET="${PRESET:-LONG_FAST}"
HOP_LIMIT="${HOP_LIMIT:-3}"
TEST_TEXT="${TEST_TEXT:-k230 meshtastic pair test}"
KEEP_DAEMON=0
NODE_A=""
NODE_B=""
HOST_A=""
HOST_B=""

usage() {
    cat <<'USAGE'
Usage:
  meshtastic_pair_test.sh --a <ip> --b <ip> [options]

Runs a two-board Meshtastic probe smoke test over SSH:
  - starts k230_meshtastic_probe daemon on both boards
  - uses EU868/LongFast by default
  - disables rebroadcast by default
  - sends one text packet in each direction
  - checks both remote event logs for the expected payload

Options:
  --a IP                  First K230 board.
  --b IP                  Second K230 board.
  --region NAME          Region. Default: EU_868.
  --preset NAME          Preset. Default: LONG_FAST.
  --text TEXT            Base text payload.
  --keep-daemon          Leave daemons running after the test.
  -h, --help             Show this help.

Environment:
  SSH_USER=root
  APP_DIR=/root/app/k230_phone_ui
  CONNECT_TIMEOUT=8
USAGE
}

die() {
    echo "error: $*" >&2
    exit 1
}

ssh_host() {
    local host="$1"
    if [[ "${host}" == *@* ]]; then
        printf '%s' "${host}"
    else
        printf '%s@%s' "${SSH_USER}" "${host}"
    fi
}

ssh_run() {
    local host="$1"
    shift
    ssh "${SSH_OPTS[@]}" "$(ssh_host "${host}")" "$@"
}

shell_quote() {
    printf '%q' "$1"
}

node_id_for_host() {
    local host="${1##*@}"
    local last="${host##*.}"
    [[ "${last}" =~ ^[0-9]+$ ]] || last=1
    printf '0x4b23%04d' "${last}"
}

start_daemon() {
    local host="$1"
    local node="$2"
    local name="$3"
    local q_probe q_region q_preset q_name q_node q_hop command
    q_probe="$(shell_quote "${PROBE}")"
    q_region="$(shell_quote "${REGION}")"
    q_preset="$(shell_quote "${PRESET}")"
    q_name="$(shell_quote "${name}")"
    q_node="$(shell_quote "${node}")"
    q_hop="$(shell_quote "${HOP_LIMIT}")"
    command="killall k230_meshtastic_probe 2>/dev/null || true; "
    command+="rm -f /tmp/k230_meshtastic.sock /tmp/k230_meshtastic_pair.log; "
    command+="nohup ${q_probe} --daemon --region ${q_region} --preset ${q_preset} "
    command+="--node ${q_name} --from ${q_node} --to 0xffffffff --hop-limit ${q_hop} "
    command+="--no-ack --no-nodeinfo --no-rebroadcast "
    command+=">/tmp/k230_meshtastic_pair.log 2>&1 & "
    command+="sleep 4; ${q_probe} --cmd-status"
    ssh_run "${host}" "${command}"
}

stop_daemon() {
    local host="$1"
    ssh_run "${host}" "killall k230_meshtastic_probe 2>/dev/null || true" >/dev/null 2>&1 || true
}

send_and_check() {
    local tx_host="$1"
    local rx_host="$2"
    local payload="$3"
    local q_probe q_payload rx_log
    q_probe="$(shell_quote "${PROBE}")"
    q_payload="$(shell_quote "${payload}")"
    rx_log="/tmp/k230_meshtastic_pair_${rx_host//[^A-Za-z0-9]/_}.log"
    echo
    echo "TX ${tx_host} -> RX ${rx_host}: ${payload}"
    ssh_run "${tx_host}" "${q_probe} --cmd-send ${q_payload}"
    sleep 9
    ssh_run "${rx_host}" "${q_probe} --cmd-log" | tee "${rx_log}"
    if ! grep -Fq "${payload}" "${rx_log}"; then
        die "payload was not found in ${rx_host} log: ${payload}"
    fi
    ssh_run "${rx_host}" "${q_probe} --cmd-status"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --a)
            [[ $# -ge 2 ]] || die "--a requires an IP"
            HOST_A="$2"
            shift 2
            ;;
        --b)
            [[ $# -ge 2 ]] || die "--b requires an IP"
            HOST_B="$2"
            shift 2
            ;;
        --region)
            [[ $# -ge 2 ]] || die "--region requires a value"
            REGION="$2"
            shift 2
            ;;
        --preset)
            [[ $# -ge 2 ]] || die "--preset requires a value"
            PRESET="$2"
            shift 2
            ;;
        --text)
            [[ $# -ge 2 ]] || die "--text requires a value"
            TEST_TEXT="$2"
            shift 2
            ;;
        --keep-daemon)
            KEEP_DAEMON=1
            shift
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

[[ -n "${HOST_A}" ]] || die "--a is required"
[[ -n "${HOST_B}" ]] || die "--b is required"
[[ "${HOST_A}" != "${HOST_B}" ]] || die "--a and --b must be different"

NODE_A="${NODE_A:-$(node_id_for_host "${HOST_A}")}"
NODE_B="${NODE_B:-$(node_id_for_host "${HOST_B}")}"

if [[ "${KEEP_DAEMON}" -eq 0 ]]; then
    trap 'stop_daemon "${HOST_A}"; stop_daemon "${HOST_B}"' EXIT
fi

echo "Meshtastic pair test"
echo "A=${HOST_A} node=${NODE_A}"
echo "B=${HOST_B} node=${NODE_B}"
echo "region=${REGION} preset=${PRESET} relay=off"

echo
echo "[1/4] Start daemon on A"
start_daemon "${HOST_A}" "${NODE_A}" "k230-a"

echo
echo "[2/4] Start daemon on B"
start_daemon "${HOST_B}" "${NODE_B}" "k230-b"

echo
echo "[3/4] A to B"
send_and_check "${HOST_A}" "${HOST_B}" "${TEST_TEXT} A-to-B"

echo
echo "[4/4] B to A"
send_and_check "${HOST_B}" "${HOST_A}" "${TEST_TEXT} B-to-A"

echo
echo "PASS: bidirectional Meshtastic text path is working."
