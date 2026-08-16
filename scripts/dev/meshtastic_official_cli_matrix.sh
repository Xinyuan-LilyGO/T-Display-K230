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
CLI="${MESHTASTIC_CLI:-/root/work/.venvs/meshtastic-cli/bin/meshtastic}"
PORT="${PORT:-/dev/ttyUSB0}"
TIMEOUT="${TIMEOUT:-70}"
WAIT_TO_DISCONNECT="${WAIT_TO_DISCONNECT:-3}"
LOG_DIR="${LOG_DIR:-/tmp}"
TEST_TEXT="${TEST_TEXT:-k230 official cli matrix}"
STEP_DELAY="${STEP_DELAY:-8}"
K230_A=""
K230_B=""
NODE_A=""
NODE_B=""
RUN_POSITION=1
FAILURES=0
STEP_INDEX=0

usage() {
    cat <<'USAGE'
Usage:
  meshtastic_official_cli_matrix.sh --a <k230-ip> --b <k230-ip> [options]

Runs an interoperability matrix between one official Meshtastic device and
two already-running K230 Meshtastic daemons:
  - reads node IDs from each K230 daemon status
  - lists nodes through the official serial device
  - sends reliable text from the official device to each K230
  - requests device telemetry from each K230
  - optionally requests position from each K230
  - runs traceroute from the official device to each K230
  - snapshots K230 daemon logs after the test

Options:
  --a IP                  First K230 board.
  --b IP                  Second K230 board.
  --port PATH             Official Meshtastic serial port. Default: /dev/ttyUSB0.
  --cli PATH              Meshtastic Python CLI path.
  --text TEXT             Base text payload.
  --timeout SEC           Remote operation timeout. Default: 70.
  --step-delay SEC        Delay between LoRa operations. Default: 8.
  --skip-position         Skip position requests when GPS is unavailable.
  -h, --help              Show this help.

Environment:
  MESHTASTIC_CLI=/path/to/meshtastic
  SSH_USER=root
  APP_DIR=/root/app/k230_phone_ui
  CONNECT_TIMEOUT=8
  LOG_DIR=/tmp
  STEP_DELAY=8
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

node_from_status() {
    local host="$1"
    local status
    status="$(ssh_run "${host}" "${PROBE} --cmd-status 2>/dev/null")"
    printf '%s\n' "${status}" | tr ' ' '\n' | awk -F= '$1 == "from" { print $2; exit }'
}

node_cli_id() {
    local node="$1"
    node="${node#!}"
    node="${node#0x}"
    printf '!%08x' "0x${node}"
}

run_cli() {
    local extra_args=()

    echo
    echo "### $*"
    if [[ "${1:-}" != "--nodes" ]]; then
        extra_args+=(--no-nodes)
    fi
    timeout "$((TIMEOUT + 30))" "${CLI}" --port "${PORT}" "$@" "${extra_args[@]}" \
        --timeout "${TIMEOUT}" --wait-to-disconnect "${WAIT_TO_DISCONNECT}"
}

run_matrix_step() {
    local name="$1"
    shift

    echo
    echo "== ${name} =="
    if [[ "${STEP_INDEX}" -gt 0 && "${STEP_DELAY}" -gt 0 ]]; then
        echo "settle=${STEP_DELAY}s"
        sleep "${STEP_DELAY}"
    fi
    STEP_INDEX=$((STEP_INDEX + 1))
    if run_cli "$@"; then
        echo "RESULT ${name}: PASS"
    else
        echo "RESULT ${name}: FAIL"
        FAILURES=$((FAILURES + 1))
    fi
}

tail_k230_log() {
    local host="$1"
    local out="$2"
    ssh_run "${host}" \
        "tail -260 /tmp/k230_meshtastic_daemon_ui.log 2>/dev/null || true" \
        >"${out}" || true
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --a)
            [[ $# -ge 2 ]] || die "--a requires an IP"
            K230_A="$2"
            shift 2
            ;;
        --b)
            [[ $# -ge 2 ]] || die "--b requires an IP"
            K230_B="$2"
            shift 2
            ;;
        --port)
            [[ $# -ge 2 ]] || die "--port requires a path"
            PORT="$2"
            shift 2
            ;;
        --cli)
            [[ $# -ge 2 ]] || die "--cli requires a path"
            CLI="$2"
            shift 2
            ;;
        --text)
            [[ $# -ge 2 ]] || die "--text requires a value"
            TEST_TEXT="$2"
            shift 2
            ;;
        --timeout)
            [[ $# -ge 2 ]] || die "--timeout requires a value"
            TIMEOUT="$2"
            shift 2
            ;;
        --step-delay)
            [[ $# -ge 2 ]] || die "--step-delay requires a value"
            STEP_DELAY="$2"
            shift 2
            ;;
        --skip-position)
            RUN_POSITION=0
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

[[ -n "${K230_A}" ]] || die "--a is required"
[[ -n "${K230_B}" ]] || die "--b is required"
[[ "${K230_A}" != "${K230_B}" ]] || die "--a and --b must be different"
[[ -x "${CLI}" ]] || die "Meshtastic CLI is not executable: ${CLI}"
[[ -e "${PORT}" ]] || die "serial port does not exist: ${PORT}"

NODE_A="$(node_from_status "${K230_A}")"
NODE_B="$(node_from_status "${K230_B}")"
[[ -n "${NODE_A}" ]] || die "failed to read node ID from ${K230_A}"
[[ -n "${NODE_B}" ]] || die "failed to read node ID from ${K230_B}"
NODE_A="$(node_cli_id "${NODE_A}")"
NODE_B="$(node_cli_id "${NODE_B}")"

TS="$(date -u +%Y%m%d_%H%M%S)"
LOG="${LOG_DIR%/}/meshtastic_official_cli_matrix_${TS}.log"
LOG_A="${LOG_DIR%/}/meshtastic_official_cli_matrix_${TS}_${K230_A//[^A-Za-z0-9]/_}.daemon.log"
LOG_B="${LOG_DIR%/}/meshtastic_official_cli_matrix_${TS}_${K230_B//[^A-Za-z0-9]/_}.daemon.log"

{
    echo "Meshtastic official CLI matrix"
    echo "time=${TS}"
    echo "port=${PORT}"
    echo "k230_a=${K230_A} node=${NODE_A}"
    echo "k230_b=${K230_B} node=${NODE_B}"

    run_matrix_step "nodes" --nodes
    run_matrix_step "official-to-a-text-ack" \
        --dest "${NODE_A}" --sendtext "${TEST_TEXT} official-to-a ${TS}" --ack
    run_matrix_step "official-to-b-text-ack" \
        --dest "${NODE_B}" --sendtext "${TEST_TEXT} official-to-b ${TS}" --ack
    run_matrix_step "a-device-telemetry" --dest "${NODE_A}" --request-telemetry
    run_matrix_step "b-device-telemetry" --dest "${NODE_B}" --request-telemetry
    if [[ "${RUN_POSITION}" -ne 0 ]]; then
        run_matrix_step "a-position" --dest "${NODE_A}" --request-position
        run_matrix_step "b-position" --dest "${NODE_B}" --request-position
    fi
    run_matrix_step "a-traceroute" --traceroute "${NODE_A}"
    run_matrix_step "b-traceroute" --traceroute "${NODE_B}"
    echo
    echo "failures=${FAILURES}"
} > >(tee "${LOG}") 2>&1

tail_k230_log "${K230_A}" "${LOG_A}"
tail_k230_log "${K230_B}" "${LOG_B}"

echo
echo "Matrix log: ${LOG}"
echo "K230 A daemon log: ${LOG_A}"
echo "K230 B daemon log: ${LOG_B}"

if [[ "${FAILURES}" -ne 0 ]]; then
    exit 2
fi
