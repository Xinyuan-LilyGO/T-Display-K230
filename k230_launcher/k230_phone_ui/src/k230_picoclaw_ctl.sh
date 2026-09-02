#!/bin/sh

set -u

APP_DIR="/root/app/k230_phone_ui"
PICO_HOME="/root/picoclaw"
PICO_BIN_DIR="$PICO_HOME/bin"
PICO_BIN="$PICO_BIN_DIR/picoclaw"
PICO_CONFIG="$PICO_HOME/config.json"
PICO_WORKSPACE="$PICO_HOME/workspace"
PICO_LOG="/tmp/k230_picoclaw_ui.log"
PICO_GATEWAY_LOG="/tmp/k230_picoclaw_gateway.log"
PICO_GATEWAY_PID="/tmp/k230_picoclaw_gateway.pid"
PICO_RELEASE_TAG="${PICO_RELEASE_TAG:-v0.3.1}"
PICO_RELEASE_URL="${PICO_RELEASE_URL:-https://github.com/sipeed/picoclaw/releases/download/${PICO_RELEASE_TAG}/picoclaw_Linux_riscv64.tar.gz}"
PICO_GATEWAY_PORT="${PICO_GATEWAY_PORT:-18790}"
PICO_CA_FILE="${PICO_CA_FILE:-/etc/ssl/certs/ca-certificates.crt}"
PICO_TZ="${PICO_TZ:-UTC}"

log()
{
    mkdir -p "$(dirname "$PICO_LOG")"
    printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >>"$PICO_LOG"
}

ensure_dirs()
{
    mkdir -p "$PICO_BIN_DIR" "$PICO_WORKSPACE"
}

have_network()
{
    ip route get 1.1.1.1 >/dev/null 2>&1
}

board_ip()
{
    ip -4 route get 1.1.1.1 2>/dev/null | awk '
        {
            for (i = 1; i <= NF; i++) {
                if ($i == "src" && (i + 1) <= NF) {
                    print $(i + 1);
                    exit;
                }
            }
        }'
}

gateway_pid()
{
    if [ -s "$PICO_GATEWAY_PID" ]; then
        pid="$(cat "$PICO_GATEWAY_PID" 2>/dev/null || true)"
        if [ -n "$pid" ] && kill -0 "$pid" >/dev/null 2>&1; then
            printf '%s\n' "$pid"
            return 0
        fi
    fi
    pidof picoclaw 2>/dev/null | awk '{print $1}'
}

json_escape()
{
    printf '%s' "${1:-}" | sed \
        -e 's/\\/\\\\/g' \
        -e 's/"/\\"/g' \
        -e 's/	/\\t/g'
}

status_cmd()
{
    ensure_dirs
    if [ -x "$PICO_BIN" ]; then
        installed="yes"
        version="$("$PICO_BIN" --no-color version 2>/dev/null | awk '
            /picoclaw / {
                for (i = 1; i <= NF; i++) {
                    if ($i == "picoclaw" && (i + 1) <= NF) {
                        print $i " " $(i + 1);
                        exit;
                    }
                }
            }')"
        [ -n "$version" ] || version="$("$PICO_BIN" version --no-color 2>/dev/null | awk '
            /picoclaw / {
                for (i = 1; i <= NF; i++) {
                    if ($i == "picoclaw" && (i + 1) <= NF) {
                        print $i " " $(i + 1);
                        exit;
                    }
                }
            }')"
        [ -n "$version" ] || version="installed"
    else
        installed="no"
        version="not installed"
    fi

    pid="$(gateway_pid || true)"
    if [ -n "${pid:-}" ]; then
        gateway="running"
    else
        gateway="stopped"
        pid="-"
    fi

    if have_network; then
        network="ready"
    else
        network="missing"
    fi
    ipaddr="$(board_ip)"
    [ -n "$ipaddr" ] || ipaddr="-"

    if [ -s "$PICO_CONFIG" ]; then
        config="ready"
    else
        config="missing"
    fi

    printf 'installed=%s\nversion=%s\ngateway=%s\npid=%s\nnetwork=%s\nip=%s\nconfig=%s\nurl=http://%s:%s\n' \
        "$installed" "$version" "$gateway" "$pid" "$network" "$ipaddr" "$config" "$ipaddr" "$PICO_GATEWAY_PORT"
}

download_file()
{
    url="$1"
    out="$2"
    if command -v curl >/dev/null 2>&1; then
        CURL_CA_BUNDLE="$PICO_CA_FILE" SSL_CERT_FILE="$PICO_CA_FILE" \
            curl -L --fail --connect-timeout 15 --max-time 240 -o "$out" "$url"
        return $?
    fi
    if command -v wget >/dev/null 2>&1; then
        SSL_CERT_FILE="$PICO_CA_FILE" wget -O "$out" "$url"
        return $?
    fi
    return 127
}

install_cmd()
{
    ensure_dirs
    if ! have_network; then
        log "install skipped: no network route"
        printf 'ERROR: no network connection\n'
        return 2
    fi

    tmp="/tmp/picoclaw_${PICO_RELEASE_TAG}.tar.gz"
    extract="/tmp/picoclaw_extract_$$"
    rm -rf "$extract"
    mkdir -p "$extract"

    log "install downloading $PICO_RELEASE_URL"
    if ! download_file "$PICO_RELEASE_URL" "$tmp" >>"$PICO_LOG" 2>&1; then
        log "install failed: download error"
        rm -rf "$extract"
        printf 'ERROR: download failed\n'
        return 3
    fi

    if ! tar -xzf "$tmp" -C "$extract" >>"$PICO_LOG" 2>&1; then
        log "install failed: extract error"
        rm -rf "$extract"
        printf 'ERROR: extract failed\n'
        return 4
    fi

    found="$(find "$extract" -type f -name picoclaw -perm /111 2>/dev/null | head -n 1)"
    if [ -z "$found" ]; then
        found="$(find "$extract" -type f -name picoclaw 2>/dev/null | head -n 1)"
    fi
    if [ -z "$found" ]; then
        log "install failed: binary not found"
        rm -rf "$extract"
        printf 'ERROR: binary not found in release archive\n'
        return 5
    fi

    cp "$found" "$PICO_BIN"
    chmod 0755 "$PICO_BIN"
    rm -rf "$extract"
    log "install complete: $PICO_BIN"
    "$0" status
}

save_config_cmd()
{
    ensure_dirs
    model_name="$(json_escape "${1:-k230-agent}")"
    model_id="$(json_escape "${2:-openai/gpt-4o-mini}")"
    api_base="$(json_escape "${3:-https://api.openai.com/v1}")"
    api_key="$(json_escape "${4:-}")"
    if [ -n "$api_key" ]; then
        api_key_line="\"api_keys\": [\"$api_key\"],"
    else
        api_key_line=""
    fi

    cat >"$PICO_CONFIG" <<EOF
{
  "version": 3,
  "agents": {
    "defaults": {
      "workspace": "$PICO_WORKSPACE",
      "restrict_to_workspace": true,
      "model_name": "$model_name",
      "max_tokens": 2048,
      "context_window": 32768,
      "temperature": 0.7,
      "max_tool_iterations": 6
    }
  },
  "gateway": {
    "host": "0.0.0.0",
    "port": $PICO_GATEWAY_PORT,
    "log_level": "warn"
  },
  "model_list": [
    {
      "model_name": "$model_name",
      "model": "$model_id",
      $api_key_line
      "api_base": "$api_base"
    }
  ],
  "channel_list": {
    "maixcam": {
      "enabled": true,
      "type": "maixcam",
      "allow_from": [],
      "settings": {
        "host": "0.0.0.0",
        "port": $PICO_GATEWAY_PORT
      }
    }
  }
}
EOF
    log "config saved model_name=$model_name model=$model_id api_base=$api_base"
    printf 'OK: config saved\n'
}

ask_cmd()
{
    ensure_dirs
    if [ ! -x "$PICO_BIN" ]; then
        printf 'ERROR: picoclaw is not installed\n'
        return 2
    fi
    if [ ! -s "$PICO_CONFIG" ]; then
        printf 'ERROR: config missing\n'
        return 3
    fi
    if ! have_network; then
        printf 'ERROR: no network connection\n'
        return 4
    fi

    prompt="${1:-}"
    if [ -z "$prompt" ]; then
        printf 'ERROR: empty prompt\n'
        return 5
    fi

    log "ask start len=${#prompt}"
    tmp="/tmp/k230_picoclaw_ask_$$.log"
    TZ="$PICO_TZ" SSL_CERT_FILE="$PICO_CA_FILE" PICOCLAW_HOME="$PICO_HOME" PICOCLAW_CONFIG="$PICO_CONFIG" \
        "$PICO_BIN" --no-color agent -m "$prompt" >"$tmp" 2>&1
    rc=$?
    cat "$tmp" | tee -a "$PICO_LOG"
    rm -f "$tmp"
    log "ask done rc=$rc"
    return "$rc"
}

gateway_start_cmd()
{
    ensure_dirs
    if [ ! -x "$PICO_BIN" ]; then
        printf 'ERROR: picoclaw is not installed\n'
        return 2
    fi
    if [ ! -s "$PICO_CONFIG" ]; then
        printf 'ERROR: config missing\n'
        return 3
    fi
    pid="$(gateway_pid || true)"
    if [ -n "${pid:-}" ]; then
        printf 'OK: gateway already running pid=%s\n' "$pid"
        return 0
    fi
    log "gateway start"
    TZ="$PICO_TZ" SSL_CERT_FILE="$PICO_CA_FILE" PICOCLAW_HOME="$PICO_HOME" PICOCLAW_CONFIG="$PICO_CONFIG" \
        nohup "$PICO_BIN" --no-color gateway --host 0.0.0.0 \
        >"$PICO_GATEWAY_LOG" 2>&1 &
    echo "$!" >"$PICO_GATEWAY_PID"
    sleep 1
    "$0" status
}

gateway_stop_cmd()
{
    pid="$(gateway_pid || true)"
    if [ -n "${pid:-}" ]; then
        kill "$pid" >/dev/null 2>&1 || true
        sleep 1
        kill -9 "$pid" >/dev/null 2>&1 || true
        rm -f "$PICO_GATEWAY_PID"
        log "gateway stopped pid=$pid"
    fi
    "$0" status
}

log_cmd()
{
    tail -80 "$PICO_LOG" "$PICO_GATEWAY_LOG" 2>/dev/null || true
}

case "${1:-status}" in
    status)
        status_cmd
        ;;
    install)
        install_cmd
        ;;
    save-config)
        shift
        save_config_cmd "$@"
        ;;
    ask)
        shift
        ask_cmd "$*"
        ;;
    gateway-start)
        gateway_start_cmd
        ;;
    gateway-stop)
        gateway_stop_cmd
        ;;
    log)
        log_cmd
        ;;
    *)
        printf 'Usage: %s {status|install|save-config MODEL_NAME MODEL_ID API_BASE API_KEY|ask PROMPT|gateway-start|gateway-stop|log}\n' "$0"
        exit 64
        ;;
esac
