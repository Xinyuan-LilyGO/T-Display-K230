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
TRACEROUTE_RETRIES="${TRACEROUTE_RETRIES:-2}"
REMOTE_REQUEST_RETRIES="${REMOTE_REQUEST_RETRIES:-2}"
K230_A=""
K230_B=""
NODE_A=""
NODE_B=""
OFFICIAL_NODE="${OFFICIAL_NODE:-}"
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
  - optionally sends reliable text from each K230 to the official device
  - optionally asks the official device for NodeInfo and traceroute from K230
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
  --traceroute-retries N  Retry traceroute steps after a timeout. Default: 2.
  --remote-request-retries N
                          Retry K230-originated remote request steps after a timeout.
                          Default: 2.
  --official-node NODE    Official device node ID for K230-to-official ACK tests.
  --skip-position         Skip position requests when GPS is unavailable.
  -h, --help              Show this help.

Environment:
  MESHTASTIC_CLI=/path/to/meshtastic
  SSH_USER=root
  APP_DIR=/root/app/k230_phone_ui
  CONNECT_TIMEOUT=8
  LOG_DIR=/tmp
  STEP_DELAY=8
  TRACEROUTE_RETRIES=2
  REMOTE_REQUEST_RETRIES=2
  OFFICIAL_NODE=!050da224
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

node_probe_id() {
    local node="$1"
    node="${node#!}"
    node="${node#0x}"
    printf '0x%08x' "0x${node}"
}

shell_quote() {
    local value="$1"
    printf "'%s'" "${value//\'/\'\\\'\'}"
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

cli_output_has_failure() {
    grep -Eq 'Received a NAK|Aborting due to:|Timed out waiting|Timed out|Resource temporarily unavailable'
}

run_matrix_step() {
    local name="$1"
    local output
    local rc=0
    shift

    echo
    echo "== ${name} =="
    if [[ "${STEP_INDEX}" -gt 0 && "${STEP_DELAY}" -gt 0 ]]; then
        echo "settle=${STEP_DELAY}s"
        sleep "${STEP_DELAY}"
    fi
    STEP_INDEX=$((STEP_INDEX + 1))
    if output="$(run_cli "$@" 2>&1)"; then
        rc=0
    else
        rc=$?
    fi
    printf '%s\n' "${output}"
    if [[ "${rc}" -eq 0 ]] && ! printf '%s\n' "${output}" | cli_output_has_failure; then
        echo "RESULT ${name}: PASS"
    else
        echo "RESULT ${name}: FAIL"
        FAILURES=$((FAILURES + 1))
    fi
}

run_matrix_step_retry() {
    local name="$1"
    local retries="$2"
    local attempt=1
    local max_attempts=$((retries + 1))
    local output
    local rc=0
    shift 2

    echo
    echo "== ${name} =="
    if [[ "${STEP_INDEX}" -gt 0 && "${STEP_DELAY}" -gt 0 ]]; then
        echo "settle=${STEP_DELAY}s"
        sleep "${STEP_DELAY}"
    fi
    STEP_INDEX=$((STEP_INDEX + 1))
    while [[ "${attempt}" -le "${max_attempts}" ]]; do
        echo "attempt=${attempt}/${max_attempts}"
        if output="$(run_cli "$@" 2>&1)"; then
            rc=0
        else
            rc=$?
        fi
        printf '%s\n' "${output}"
        if [[ "${rc}" -eq 0 ]] &&
           ! printf '%s\n' "${output}" | cli_output_has_failure; then
            echo "RESULT ${name}: PASS attempt=${attempt}"
            return 0
        fi
        if [[ "${attempt}" -lt "${max_attempts}" ]]; then
            echo "RESULT ${name}: RETRY attempt=${attempt}"
            if [[ "${STEP_DELAY}" -gt 0 ]]; then
                sleep "${STEP_DELAY}"
            fi
        fi
        attempt=$((attempt + 1))
    done
    echo "RESULT ${name}: FAIL attempts=${max_attempts}"
    FAILURES=$((FAILURES + 1))
}

run_k230_send_ack_step() {
    local name="$1"
    local host="$2"
    local target="$3"
    local text="$4"
    local target_probe
    local quoted_target
    local quoted_text
    local response
    local chat
    local line
    local before_status
    local before_ack_rx
    local before_nak_rx
    local before_timeout
    local status
    local ack_rx
    local nak_rx
    local timeout_count
    local attempt

    echo
    echo "== ${name} =="
    if [[ "${STEP_INDEX}" -gt 0 && "${STEP_DELAY}" -gt 0 ]]; then
        echo "settle=${STEP_DELAY}s"
        sleep "${STEP_DELAY}"
    fi
    STEP_INDEX=$((STEP_INDEX + 1))
    target_probe="$(node_probe_id "${target}")"
    quoted_target="$(shell_quote "${target_probe}")"
    quoted_text="$(shell_quote "${text}")"
    echo "target=${target_probe}"
    echo "text=${text}"
    before_status="$(ssh_run "${host}" "${PROBE} --cmd-status 2>/dev/null" || true)"
    before_ack_rx="$(printf '%s\n' "${before_status}" | tr ' ' '\n' |
        awk -F= '$1 == "ack_rx" { print $2; exit }')"
    before_nak_rx="$(printf '%s\n' "${before_status}" | tr ' ' '\n' |
        awk -F= '$1 == "nak_rx" { print $2; exit }')"
    before_timeout="$(printf '%s\n' "${before_status}" | tr ' ' '\n' |
        awk -F= '$1 == "ack_timeout" { print $2; exit }')"
    [[ "${before_ack_rx}" =~ ^[0-9]+$ ]] || before_ack_rx=0
    [[ "${before_nak_rx}" =~ ^[0-9]+$ ]] || before_nak_rx=0
    [[ "${before_timeout}" =~ ^[0-9]+$ ]] || before_timeout=0
    echo "before_ack_rx=${before_ack_rx} before_nak_rx=${before_nak_rx} before_timeout=${before_timeout}"
    if ! response="$(ssh_run "${host}" "${PROBE} --cmd-send-to-ack ${quoted_target} ${quoted_text} 2>/dev/null")"; then
        echo "RESULT ${name}: FAIL queue-command"
        FAILURES=$((FAILURES + 1))
        return 0
    fi
    echo "${response}"
    if ! printf '%s\n' "${response}" | grep -q '^OK queued'; then
        echo "RESULT ${name}: FAIL queue-response"
        FAILURES=$((FAILURES + 1))
        return 0
    fi
    for attempt in $(seq 1 8); do
        sleep 5
        status="$(ssh_run "${host}" "${PROBE} --cmd-status 2>/dev/null" || true)"
        ack_rx="$(printf '%s\n' "${status}" | tr ' ' '\n' |
            awk -F= '$1 == "ack_rx" { print $2; exit }')"
        nak_rx="$(printf '%s\n' "${status}" | tr ' ' '\n' |
            awk -F= '$1 == "nak_rx" { print $2; exit }')"
        timeout_count="$(printf '%s\n' "${status}" | tr ' ' '\n' |
            awk -F= '$1 == "ack_timeout" { print $2; exit }')"
        [[ "${ack_rx}" =~ ^[0-9]+$ ]] || ack_rx=0
        [[ "${nak_rx}" =~ ^[0-9]+$ ]] || nak_rx=0
        [[ "${timeout_count}" =~ ^[0-9]+$ ]] || timeout_count=0
        if [[ "${ack_rx}" -gt "${before_ack_rx}" ]]; then
            echo "status_ack_rx=${ack_rx}"
            chat="$(ssh_run "${host}" "${PROBE} --cmd-chat 2>/dev/null" || true)"
            line="$(printf '%s\n' "${chat}" | grep -F "${text}" | tail -1 || true)"
            [[ -z "${line}" ]] || echo "chat=${line}"
            echo "RESULT ${name}: PASS attempt=${attempt}"
            return 0
        fi
        if [[ "${nak_rx}" -gt "${before_nak_rx}" ]]; then
            echo "status_nak_rx=${nak_rx}"
            echo "RESULT ${name}: FAIL nak"
            FAILURES=$((FAILURES + 1))
            return 0
        fi
        if [[ "${timeout_count}" -gt "${before_timeout}" ]]; then
            echo "status_ack_timeout=${timeout_count}"
            echo "RESULT ${name}: FAIL timeout"
            FAILURES=$((FAILURES + 1))
            return 0
        fi
        chat="$(ssh_run "${host}" "${PROBE} --cmd-chat 2>/dev/null" || true)"
        line="$(printf '%s\n' "${chat}" | grep -F "${text}" | tail -1 || true)"
        if [[ -n "${line}" ]]; then
            echo "chat=${line}"
            if printf '%s\n' "${line}" | grep -q 'ack=ack'; then
                echo "RESULT ${name}: PASS attempt=${attempt}"
                return 0
            fi
            if printf '%s\n' "${line}" | grep -Eq 'ack=(timeout|dropped)'; then
                echo "RESULT ${name}: FAIL ${line}"
                FAILURES=$((FAILURES + 1))
                return 0
            fi
        fi
    done
    echo "RESULT ${name}: FAIL ack-timeout"
    FAILURES=$((FAILURES + 1))
}

node_line_for() {
    local nodes="$1"
    local target="$2"
    local target_probe

    target_probe="$(node_probe_id "${target}")"
    printf '%s\n' "${nodes}" | awk -v node="${target_probe}" '
        tolower($1) == tolower(node) {
            print
            exit
        }
    '
}

node_rx_from_line() {
    local line="$1"

    printf '%s\n' "${line}" | tr ' ' '\n' |
        awk -F= '$1 == "rx" { print $2; exit }'
}

node_line_has_request_result() {
    local request_type="$1"
    local line="$2"

    case "${request_type}" in
        nodeinfo)
            [[ "${line}" == *"name="* && "${line}" != *"name=-"* ]]
            ;;
        traceroute)
            [[ "${line}" == *"trace="* && "${line}" != *"trace=-"* ]]
            ;;
        *)
            return 1
            ;;
    esac
}

run_k230_remote_request_step() {
    local name="$1"
    local host="$2"
    local target="$3"
    local request_type="$4"
    local target_probe
    local quoted_target
    local before_nodes
    local before_line
    local before_rx
    local response
    local nodes
    local line
    local rx
    local attempt
    local request_attempt
    local max_request_attempts

    echo
    echo "== ${name} =="
    if [[ "${STEP_INDEX}" -gt 0 && "${STEP_DELAY}" -gt 0 ]]; then
        echo "settle=${STEP_DELAY}s"
        sleep "${STEP_DELAY}"
    fi
    STEP_INDEX=$((STEP_INDEX + 1))

    target_probe="$(node_probe_id "${target}")"
    quoted_target="$(shell_quote "${target_probe}")"
    before_nodes="$(ssh_run "${host}" "${PROBE} --cmd-nodes 2>/dev/null" || true)"
    before_line="$(node_line_for "${before_nodes}" "${target_probe}")"
    before_rx="$(node_rx_from_line "${before_line}")"
    if [[ ! "${before_rx}" =~ ^[0-9]+$ ]]; then
        before_rx=0
    fi
    echo "target=${target_probe}"
    echo "request=${request_type}"
    echo "before_rx=${before_rx}"
    max_request_attempts=$((REMOTE_REQUEST_RETRIES + 1))
    for request_attempt in $(seq 1 "${max_request_attempts}"); do
        echo "request_attempt=${request_attempt}/${max_request_attempts}"
        if ! response="$(ssh_run "${host}" "${PROBE} --cmd-request-${request_type} ${quoted_target} 2>/dev/null")"; then
            echo "RESULT ${name}: FAIL queue-command"
            FAILURES=$((FAILURES + 1))
            return 0
        fi
        echo "${response}"
        if ! printf '%s\n' "${response}" | grep -q '^OK request queued'; then
            echo "RESULT ${name}: FAIL queue-response"
            FAILURES=$((FAILURES + 1))
            return 0
        fi
        for attempt in $(seq 1 8); do
            sleep 5
            nodes="$(ssh_run "${host}" "${PROBE} --cmd-nodes 2>/dev/null" || true)"
            line="$(node_line_for "${nodes}" "${target_probe}")"
            rx="$(node_rx_from_line "${line}")"
            if [[ "${rx}" =~ ^[0-9]+$ ]] &&
               [[ "${rx}" -gt "${before_rx}" ]] &&
               node_line_has_request_result "${request_type}" "${line}"; then
                echo "node=${line}"
                echo "RESULT ${name}: PASS request_attempt=${request_attempt} poll_attempt=${attempt}"
                return 0
            fi
        done
        if [[ "${request_attempt}" -lt "${max_request_attempts}" ]]; then
            echo "RESULT ${name}: RETRY request_attempt=${request_attempt}"
            if [[ "${STEP_DELAY}" -gt 0 ]]; then
                sleep "${STEP_DELAY}"
            fi
        fi
    done
    echo "RESULT ${name}: FAIL request-timeout"
    if [[ -n "${line:-}" ]]; then
        echo "last_node=${line}"
    fi
    FAILURES=$((FAILURES + 1))
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
        --traceroute-retries)
            [[ $# -ge 2 ]] || die "--traceroute-retries requires a value"
            TRACEROUTE_RETRIES="$2"
            shift 2
            ;;
        --remote-request-retries)
            [[ $# -ge 2 ]] || die "--remote-request-retries requires a value"
            REMOTE_REQUEST_RETRIES="$2"
            shift 2
            ;;
        --official-node)
            [[ $# -ge 2 ]] || die "--official-node requires a node ID"
            OFFICIAL_NODE="$2"
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
    if [[ -n "${OFFICIAL_NODE}" ]]; then
        echo "official_node=$(node_cli_id "${OFFICIAL_NODE}")"
    fi

    run_matrix_step "nodes" --nodes
    run_matrix_step "official-to-a-text-ack" \
        --dest "${NODE_A}" --sendtext "${TEST_TEXT} official-to-a ${TS}" --ack
    run_matrix_step "official-to-b-text-ack" \
        --dest "${NODE_B}" --sendtext "${TEST_TEXT} official-to-b ${TS}" --ack
    if [[ -n "${OFFICIAL_NODE}" ]]; then
        run_k230_send_ack_step "a-to-official-text-ack" "${K230_A}" \
            "${OFFICIAL_NODE}" "${TEST_TEXT} a-to-official ${TS}"
        run_k230_send_ack_step "b-to-official-text-ack" "${K230_B}" \
            "${OFFICIAL_NODE}" "${TEST_TEXT} b-to-official ${TS}"
        run_k230_remote_request_step "a-to-official-nodeinfo-request" \
            "${K230_A}" "${OFFICIAL_NODE}" "nodeinfo"
        run_k230_remote_request_step "b-to-official-nodeinfo-request" \
            "${K230_B}" "${OFFICIAL_NODE}" "nodeinfo"
        run_k230_remote_request_step "a-to-official-traceroute-request" \
            "${K230_A}" "${OFFICIAL_NODE}" "traceroute"
        run_k230_remote_request_step "b-to-official-traceroute-request" \
            "${K230_B}" "${OFFICIAL_NODE}" "traceroute"
    fi
    run_matrix_step "a-device-telemetry" --dest "${NODE_A}" --request-telemetry
    run_matrix_step "b-device-telemetry" --dest "${NODE_B}" --request-telemetry
    if [[ "${RUN_POSITION}" -ne 0 ]]; then
        run_matrix_step "a-position" --dest "${NODE_A}" --request-position
        run_matrix_step "b-position" --dest "${NODE_B}" --request-position
    fi
    run_matrix_step_retry "a-traceroute" "${TRACEROUTE_RETRIES}" \
        --traceroute "${NODE_A}"
    run_matrix_step_retry "b-traceroute" "${TRACEROUTE_RETRIES}" \
        --traceroute "${NODE_B}"
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
