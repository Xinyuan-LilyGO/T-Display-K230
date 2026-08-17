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
DURATION_SEC="${DURATION_SEC:-1}"
WAIT_SEC="${WAIT_SEC:-20}"
SSH_RETRIES="${SSH_RETRIES:-3}"
HOST_A=""
HOST_B=""

usage() {
    cat <<'USAGE'
Usage:
  meshtastic_voice_pair_test.sh --a <k230-ip> --b <k230-ip> [options]

Runs a two-board K230 Meshtastic voice-note smoke test over SSH:
  - creates a raw 8 kHz S16_LE mono PCM test file on each transmitter
  - asks the local Meshtastic daemon to Opus-encode and transmit it
  - verifies the peer creates /tmp/k230_mesh_voice_rx_*.raw
  - prints sender/receiver chat and event log tails for diagnosis

Options:
  --a IP              First K230 board.
  --b IP              Second K230 board.
  --duration SEC      PCM duration. Default: 1.
  --wait SEC          Wait after each transmission. Default: 20.
  -h, --help          Show this help.

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
    local attempt=1
    local rc=0
    shift
    while [[ "${attempt}" -le "${SSH_RETRIES}" ]]; do
        if ssh "${SSH_OPTS[@]}" "$(ssh_host "${host}")" "$@"; then
            return 0
        fi
        rc=$?
        if [[ "${attempt}" -lt "${SSH_RETRIES}" ]]; then
            sleep "${attempt}"
        fi
        attempt=$((attempt + 1))
    done
    return "${rc}"
}

send_voice() {
    local tx="$1"
    local rx="$2"
    local label="$3"
    local raw_path="/tmp/k230_voice_${label}.raw"
    local before_count
    local after_count

    echo
    echo "== ${label}: ${tx} -> ${rx} =="
    before_count="$(ssh_run "${rx}" "ls /tmp/k230_mesh_voice_rx_* 2>/dev/null | wc -l")" ||
        die "receiver ${rx} is not reachable before ${label}"
    ssh_run "${tx}" "dd if=/dev/zero of='${raw_path}' bs=$((16000 * DURATION_SEC)) count=1 2>/dev/null; cd '${APP_DIR}' && '${PROBE}' --cmd-send-voice '${raw_path}'" ||
        die "transmitter ${tx} failed to queue voice"
    sleep "${WAIT_SEC}"
    after_count="$(ssh_run "${rx}" "ls /tmp/k230_mesh_voice_rx_* 2>/dev/null | wc -l")" ||
        die "receiver ${rx} is not reachable after ${label}"

    echo "-- tx chat --"
    ssh_run "${tx}" "cd '${APP_DIR}' && '${PROBE}' --cmd-chat | tail -20" || true
    echo "-- rx chat --"
    ssh_run "${rx}" "cd '${APP_DIR}' && '${PROBE}' --cmd-chat | tail -30" || true
    echo "-- rx event tail --"
    ssh_run "${rx}" "cd '${APP_DIR}' && '${PROBE}' --cmd-log | tail -50" || true
    echo "-- rx files --"
    ssh_run "${rx}" "ls -l /tmp/k230_mesh_voice_rx_* 2>/dev/null || true" || true

    if [[ "${after_count}" -le "${before_count}" ]]; then
        die "receiver ${rx} did not create a new decoded voice file"
    fi
    echo "RESULT ${label}: PASS before=${before_count} after=${after_count}"
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
        --duration)
            [[ $# -ge 2 ]] || die "--duration requires seconds"
            DURATION_SEC="$2"
            shift 2
            ;;
        --wait)
            [[ $# -ge 2 ]] || die "--wait requires seconds"
            WAIT_SEC="$2"
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

[[ -n "${HOST_A}" ]] || die "--a is required"
[[ -n "${HOST_B}" ]] || die "--b is required"
[[ "${HOST_A}" != "${HOST_B}" ]] || die "--a and --b must be different"
[[ "${DURATION_SEC}" =~ ^[1-9][0-9]*$ ]] || die "--duration must be a positive integer"
[[ "${WAIT_SEC}" =~ ^[1-9][0-9]*$ ]] || die "--wait must be a positive integer"

echo "Meshtastic voice pair test"
echo "A=${HOST_A}"
echo "B=${HOST_B}"
echo "duration=${DURATION_SEC}s wait=${WAIT_SEC}s"

send_voice "${HOST_A}" "${HOST_B}" "a_to_b"
send_voice "${HOST_B}" "${HOST_A}" "b_to_a"

echo
echo "Meshtastic voice pair test passed"
