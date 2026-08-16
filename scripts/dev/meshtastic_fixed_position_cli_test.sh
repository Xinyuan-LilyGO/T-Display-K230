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
K230=""
LAT_I="225400000"
LON_I="1139500000"
ALT_M="42"
SETTLE_SEC="${SETTLE_SEC:-8}"

usage() {
    cat <<'USAGE'
Usage:
  meshtastic_fixed_position_cli_test.sh --host <k230-ip> [options]

Temporarily restarts one K230 Meshtastic daemon with an explicit fixed
position, asks an official Meshtastic device for that position through the
Python CLI, and restores the daemon with its previous runtime parameters.

This is a development interoperability test only. It does not write fixed
position preferences and should not be used as a release default.

Options:
  --host IP               K230 board to test.
  --port PATH             Official Meshtastic serial port. Default: /dev/ttyUSB0.
  --cli PATH              Meshtastic Python CLI path.
  --lat-i VALUE           Latitude in degrees * 1e7. Default: 225400000.
  --lon-i VALUE           Longitude in degrees * 1e7. Default: 1139500000.
  --alt-m VALUE           Altitude in meters. Default: 42.
  --settle SEC            Wait after daemon restart. Default: 8.
  -h, --help              Show this help.

Environment:
  MESHTASTIC_CLI=/path/to/meshtastic
  SSH_USER=root
  APP_DIR=/root/app/k230_phone_ui
  CONNECT_TIMEOUT=8
  LOG_DIR=/tmp
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

status_field() {
    local status="$1"
    local key="$2"
    printf '%s\n' "${status}" | tr ' ' '\n' | awk -F= -v k="${key}" '$1 == k { print $2; exit }'
}

node_cli_id() {
    local node="$1"
    node="${node#!}"
    node="${node#0x}"
    printf '!%08x' "0x${node}"
}

decimal_to_i() {
    local value="$1"
    awk -v v="${value}" 'BEGIN { printf "%d", v * 10000000 }'
}

daemon_command_from_status() {
    local status="$1"
    local fixed_arg="${2:-}"
    local region preset channel psk node from to hop want_ack relay
    local position position_interval telemetry telemetry_interval
    local telemetry_env telemetry_env_interval slot manual_power power
    local fixed fixed_lat fixed_lon fixed_alt
    local args=()

    region="$(status_field "${status}" region)"
    preset="$(status_field "${status}" preset)"
    channel="$(status_field "${status}" channel)"
    psk="$(status_field "${status}" psk)"
    node="$(status_field "${status}" node)"
    from="$(status_field "${status}" from)"
    to="$(status_field "${status}" to)"
    hop="$(status_field "${status}" hop)"
    want_ack="$(status_field "${status}" want_ack)"
    relay="$(status_field "${status}" relay)"
    position="$(status_field "${status}" position)"
    position_interval="$(status_field "${status}" position_interval)"
    telemetry="$(status_field "${status}" telemetry)"
    telemetry_interval="$(status_field "${status}" telemetry_interval)"
    telemetry_env="$(status_field "${status}" telemetry_env)"
    telemetry_env_interval="$(status_field "${status}" telemetry_env_interval)"
    slot="$(status_field "${status}" slot)"
    manual_power="$(status_field "${status}" manual_power)"
    power="$(status_field "${status}" power)"
    fixed="$(status_field "${status}" fixed)"
    fixed_lat="$(status_field "${status}" fixed_lat)"
    fixed_lon="$(status_field "${status}" fixed_lon)"
    fixed_alt="0"

    [[ -n "${region}" ]] || region="EU_868"
    [[ -n "${preset}" ]] || preset="LONG_FAST"
    [[ -n "${psk}" ]] || psk="default"
    [[ -n "${node}" ]] || node="k230-t-display"
    [[ -n "${from}" ]] || from="0"
    [[ -n "${to}" ]] || to="0xffffffff"
    [[ -n "${hop}" ]] || hop="3"
    [[ -n "${position_interval}" ]] || position_interval="900"
    [[ -n "${telemetry_interval}" ]] || telemetry_interval="300"
    [[ -n "${telemetry_env_interval}" ]] || telemetry_env_interval="300"

    args+=(--daemon --region "${region}" --preset "${preset}")
    if [[ -n "${channel}" && "${channel}" != "-" ]]; then
        args+=(--channel-name "${channel}")
    fi
    if [[ -n "${slot}" && "${slot}" != "auto" ]]; then
        args+=(--slot "${slot}")
    fi
    args+=(--psk "${psk}")
    if [[ "${manual_power}" == "true" && -n "${power}" ]]; then
        args+=(--power "${power}")
    fi
    args+=(--node "${node}" --from "${from}" --to "${to}" --hop-limit "${hop}")
    if [[ "${want_ack}" == "on" ]]; then
        args+=(--ack)
    else
        args+=(--no-ack)
    fi
    if [[ "${relay}" == "off" ]]; then
        args+=(--no-rebroadcast)
    fi
    if [[ "${position}" == "off" ]]; then
        args+=(--no-position)
    else
        args+=(--position --position-interval "${position_interval}")
    fi
    if [[ -n "${fixed_arg}" ]]; then
        args+=(--fixed-position-i "${fixed_arg}")
    elif [[ "${fixed}" == "on" && -n "${fixed_lat}" && -n "${fixed_lon}" ]]; then
        args+=(--fixed-position-i "$(decimal_to_i "${fixed_lat}"),$(decimal_to_i "${fixed_lon}"),${fixed_alt}")
    fi
    if [[ "${telemetry}" == "off" ]]; then
        args+=(--no-telemetry)
    else
        args+=(--telemetry --telemetry-interval "${telemetry_interval}")
    fi
    if [[ "${telemetry_env}" == "off" ]]; then
        args+=(--no-env-telemetry)
    else
        args+=(--env-telemetry --env-telemetry-interval "${telemetry_env_interval}")
    fi

    printf '%q ' "${args[@]}"
}

restart_daemon() {
    local host="$1"
    local args="$2"

    ssh_run "${host}" "set -eu
${PROBE} --cmd-quit >/tmp/mesh_fixed_position_quit.log 2>&1 || true
sleep 1
ps -a | grep 'k230_meshtastic_probe --daemon' | grep -v grep | while read pid rest; do
    kill \"\$pid\" 2>/dev/null || true
done
sleep 1
rm -f /tmp/k230_meshtastic.sock
nohup ${PROBE} ${args} >/tmp/k230_meshtastic_daemon_ui.log 2>&1 &
for i in 1 2 3 4 5; do
    test -S /tmp/k230_meshtastic.sock && exit 0
    sleep 1
done
exit 1
"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host)
            [[ $# -ge 2 ]] || die "--host requires an IP"
            K230="$2"
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
        --lat-i)
            [[ $# -ge 2 ]] || die "--lat-i requires a value"
            LAT_I="$2"
            shift 2
            ;;
        --lon-i)
            [[ $# -ge 2 ]] || die "--lon-i requires a value"
            LON_I="$2"
            shift 2
            ;;
        --alt-m)
            [[ $# -ge 2 ]] || die "--alt-m requires a value"
            ALT_M="$2"
            shift 2
            ;;
        --settle)
            [[ $# -ge 2 ]] || die "--settle requires a value"
            SETTLE_SEC="$2"
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

[[ -n "${K230}" ]] || die "--host is required"
[[ -x "${CLI}" ]] || die "Meshtastic CLI is not executable: ${CLI}"
[[ -e "${PORT}" ]] || die "serial port does not exist: ${PORT}"

TS="$(date -u +%Y%m%d_%H%M%S)"
LOG="${LOG_DIR%/}/meshtastic_fixed_position_cli_test_${TS}.log"
DAEMON_LOG="${LOG_DIR%/}/meshtastic_fixed_position_cli_test_${TS}_${K230//[^A-Za-z0-9]/_}.daemon.log"

ORIGINAL_STATUS="$(ssh_run "${K230}" "${PROBE} --cmd-status 2>/dev/null")"
NODE="$(node_cli_id "$(status_field "${ORIGINAL_STATUS}" from)")"
[[ "${NODE}" != "!00000000" ]] || die "failed to read node ID from ${K230}"

TEST_ARGS="$(daemon_command_from_status "${ORIGINAL_STATUS}" "${LAT_I},${LON_I},${ALT_M}")"
RESTORE_ARGS="$(daemon_command_from_status "${ORIGINAL_STATUS}")"

restore() {
    echo "Restoring K230 daemon..." | tee -a "${LOG}" || true
    restart_daemon "${K230}" "${RESTORE_ARGS}" || true
}
trap restore EXIT

{
    echo "Meshtastic fixed position CLI test"
    echo "time=${TS}"
    echo "host=${K230}"
    echo "node=${NODE}"
    echo "port=${PORT}"
    echo "fixed=${LAT_I},${LON_I},${ALT_M}"
    echo
    echo "Restarting daemon with fixed position..."
} | tee "${LOG}"

restart_daemon "${K230}" "${TEST_ARGS}"
sleep "${SETTLE_SEC}"
ssh_run "${K230}" "${PROBE} --cmd-status" | tee -a "${LOG}"

{
    echo
    echo "Requesting position through official Meshtastic device..."
} | tee -a "${LOG}"

timeout "$((TIMEOUT + 30))" "${CLI}" --port "${PORT}" --dest "${NODE}" \
    --request-position --no-nodes --timeout "${TIMEOUT}" \
    --wait-to-disconnect "${WAIT_TO_DISCONNECT}" | tee -a "${LOG}"

ssh_run "${K230}" "tail -260 /tmp/k230_meshtastic_daemon_ui.log 2>/dev/null || true" \
    >"${DAEMON_LOG}" || true

echo "Log: ${LOG}"
echo "Daemon log: ${DAEMON_LOG}"
