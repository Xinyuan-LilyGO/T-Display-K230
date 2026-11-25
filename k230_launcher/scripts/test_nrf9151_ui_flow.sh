#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 <user@host> <k230_phone_ui_fullswitch_test>" >&2
    exit 2
}

if [ "$#" -ne 2 ]; then
    usage
fi

TARGET="$1"
TEST_BINARY="$2"
REMOTE_TEST_BINARY="/tmp/k230_phone_ui_fullswitch_test"
REMOTE_APP_DIR="/root/app/k230_phone_ui"
REMOTE_SERVICE="/etc/init.d/S99zz_k230_phone_ui"
GENERAL_LOG="/tmp/k230_page_smoke_test.log"
CELLULAR_LOG="/tmp/k230_cellular_flow_test.log"
service_stopped=0

if [ ! -x "$TEST_BINARY" ]; then
    echo "ERROR: test binary is not executable: $TEST_BINARY" >&2
    exit 2
fi

restore_ui() {
    if [ "$service_stopped" -eq 1 ]; then
        ssh "$TARGET" "$REMOTE_SERVICE restart >/tmp/k230_ui_test_restart.log 2>&1 || $REMOTE_SERVICE start >/tmp/k230_ui_test_restart.log 2>&1 || true"
    fi
}
trap restore_ui EXIT INT TERM

echo "[1/6] Checking device and deploying temporary test binary"
ssh "$TARGET" "test -x $REMOTE_SERVICE && test -d $REMOTE_APP_DIR"
scp -q "$TEST_BINARY" "$TARGET:$REMOTE_TEST_BINARY"
ssh "$TARGET" "chmod 0755 $REMOTE_TEST_BINARY; $REMOTE_SERVICE stop >/tmp/k230_ui_test_stop.log 2>&1 || true; pkill -x k230_phone_ui_fullswitch_test 2>/dev/null || true"
service_stopped=1

before_crashes="$(ssh "$TARGET" "dmesg | grep -c 'k230_phone_ui.*unhandled signal' || true")"

echo "[2/6] Switching through every visible top-level app twice"
timeout 240 ssh "$TARGET" "cd $REMOTE_APP_DIR && K230_PAGE_SMOKE_TEST=1 K230_PAGE_SMOKE_CYCLES=2 K230_PAGE_SMOKE_INTERVAL_MS=900 $REMOTE_TEST_BINARY >$GENERAL_LOG 2>&1"
ssh "$TARGET" "grep -q 'PAGE_SMOKE_COMPLETE cycles=2' /tmp/k230_touch_trace.log"
ssh "$TARGET" "cp /tmp/k230_touch_trace.log /tmp/k230_page_smoke_trace.log"

echo "[3/6] Testing Modem -> GNSS -> HTTP -> MQTT -> Modem"
timeout 720 ssh "$TARGET" "cd $REMOTE_APP_DIR && K230_CELLULAR_FLOW_TEST=1 K230_CELLULAR_GNSS_TIMEOUT_S=300 $REMOTE_TEST_BINARY >$CELLULAR_LOG 2>&1"
ssh "$TARGET" "grep -q 'PASS all Cellular pages and services' $CELLULAR_LOG"

echo "[4/6] Checking for crashes and stale UART ownership"
after_crashes="$(ssh "$TARGET" "dmesg | grep -c 'k230_phone_ui.*unhandled signal' || true")"
if [ "$after_crashes" -ne "$before_crashes" ]; then
    echo "ERROR: new UI crash detected in kernel log" >&2
    exit 1
fi
ssh "$TARGET" "if command -v fuser >/dev/null 2>&1; then ! fuser /tmp/k230_nrf9151_uart.lock >/dev/null 2>&1; fi"

echo "[5/6] Restoring the production UI service"
restore_ui
service_stopped=0
sleep 5
ssh "$TARGET" "pidof k230_phone_ui >/dev/null"

echo "[6/6] Result summary"
ssh "$TARGET" "grep -E 'PAGE_SMOKE_(START|CYCLE_COMPLETE|COMPLETE)' /tmp/k230_page_smoke_trace.log | tail -8; grep -E '^\[cellular-flow-test\]' $CELLULAR_LOG"
echo "PASS: UI page switching, GNSS, LTE restoration, HTTP GET, and modem fields"
