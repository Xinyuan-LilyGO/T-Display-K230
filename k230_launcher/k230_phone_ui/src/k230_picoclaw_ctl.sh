#!/bin/sh

set -u

APP_DIR="/root/app/k230_phone_ui"
PICO_HOME="/root/picoclaw"
PICO_BIN_DIR="$PICO_HOME/bin"
PICO_BIN="$PICO_BIN_DIR/picoclaw"
PICO_CONFIG="$PICO_HOME/config.json"
PICO_SECURITY="$PICO_HOME/.security.yml"
PICO_WORKSPACE="$PICO_HOME/workspace"
PICO_LOG="/tmp/k230_picoclaw_ui.log"
PICO_GATEWAY_LOG="/tmp/k230_picoclaw_gateway.log"
PICO_GATEWAY_PID="/tmp/k230_picoclaw_gateway.pid"
PICO_WEIXIN_AUTH_LOG="/tmp/k230_picoclaw_weixin_auth.log"
PICO_WEIXIN_AUTH_PID="/tmp/k230_picoclaw_weixin_auth.pid"
PICO_WEIXIN_GATEWAY_DIGEST="/tmp/k230_picoclaw_weixin_gateway.digest"
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
        rm -f "$PICO_GATEWAY_PID"
    fi
    ps 2>/dev/null | awk '$0 ~ /\/picoclaw([[:space:]]|$)/ && $0 ~ /[[:space:]]gateway([[:space:]]|$)/ {print $1; exit}'
}

weixin_auth_pid()
{
    if [ -s "$PICO_WEIXIN_AUTH_PID" ]; then
        pid="$(cat "$PICO_WEIXIN_AUTH_PID" 2>/dev/null || true)"
        if [ -n "$pid" ] && kill -0 "$pid" >/dev/null 2>&1; then
            printf '%s\n' "$pid"
            return 0
        fi
    fi
    ps 2>/dev/null | awk '$0 ~ /\/picoclaw([[:space:]]|$)/ && $0 ~ /[[:space:]]auth[[:space:]]/ && $0 ~ /[[:space:]]weixin([[:space:]]|$)/ {print $1; exit}'
}

json_escape()
{
    printf '%s' "${1:-}" | sed \
        -e 's/\\/\\\\/g' \
        -e 's/"/\\"/g' \
        -e 's/	/\\t/g'
}

json_string_value()
{
    key="$1"
    file="$2"
    [ -s "$file" ] || return 1
    sed -n "s/.*\"$key\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" "$file" | head -n 1
}

weixin_security_has_token()
{
    [ -s "$PICO_SECURITY" ] || return 1
    awk '
        /^[^[:space:]].*:/{section=$1}
        /weixin:/{in_weixin=1}
        in_weixin && /token:/{found=1; exit}
        END{exit found ? 0 : 1}
    ' "$PICO_SECURITY"
}

weixin_config_state()
{
    if grep -q '"weixin"' "$PICO_CONFIG" 2>/dev/null &&
       weixin_security_has_token; then
        printf 'ready\n'
    else
        printf 'missing\n'
    fi
}

model_api_key_from_security()
{
    model_name="${1:-}"
    [ -n "$model_name" ] || model_name="$(json_string_value model_name "$PICO_CONFIG" || true)"
    [ -n "$model_name" ] || return 1
    [ -s "$PICO_SECURITY" ] || return 1
    awk -v target="${model_name}:0:" '
        /^model_list:/ {
            in_models = 1
            next
        }
        in_models && /^[^[:space:]]/ {
            exit
        }
        in_models && $1 == target {
            in_target = 1
            next
        }
        in_target && /^[[:space:]]{2}[^[:space:]].*:/ {
            exit
        }
        in_target && /^[[:space:]]+-[[:space:]]+/ {
            sub(/^[[:space:]]+-[[:space:]]+/, "")
            print
            exit
        }
    ' "$PICO_SECURITY"
}

model_api_key_state()
{
    model_name="$(json_string_value model_name "$PICO_CONFIG" || true)"
    if sed -n 's/.*"api_keys"[[:space:]]*:[[:space:]]*\[[[:space:]]*"\([^"]\+\)".*/\1/p' "$PICO_CONFIG" 2>/dev/null | grep -q .; then
        printf 'ready\n'
        return
    fi
    if [ -n "$(model_api_key_from_security "$model_name" || true)" ]; then
        printf 'ready\n'
        return
    fi
    printf 'missing\n'
}

weixin_auth_state()
{
    if [ "$(weixin_config_state)" = "ready" ]; then
        printf 'stopped\n'
        return
    fi
    pid="$(weixin_auth_pid || true)"
    if [ -n "${pid:-}" ]; then
        printf 'running\n'
        return
    fi
    if grep -qi 'login failed\|qrcode expired\|expired' "$PICO_WEIXIN_AUTH_LOG" 2>/dev/null; then
        printf 'failed\n'
        return
    fi
    printf 'stopped\n'
}

weixin_gateway_digest()
{
    {
        cksum "$PICO_CONFIG" 2>/dev/null || true
        cksum "$PICO_SECURITY" 2>/dev/null || true
    } | awk '{printf "%s:%s;", $1, $2}'
}

gateway_start_only()
{
    log "gateway start"
    TZ="$PICO_TZ" SSL_CERT_FILE="$PICO_CA_FILE" PICOCLAW_HOME="$PICO_HOME" PICOCLAW_CONFIG="$PICO_CONFIG" \
        nohup "$PICO_BIN" --no-color gateway --host 0.0.0.0 \
        >"$PICO_GATEWAY_LOG" 2>&1 &
    echo "$!" >"$PICO_GATEWAY_PID"
    sleep 1
}

gateway_stop_only()
{
    pid="$(gateway_pid || true)"
    if [ -n "${pid:-}" ]; then
        kill "$pid" >/dev/null 2>&1 || true
        sleep 1
        kill -9 "$pid" >/dev/null 2>&1 || true
        rm -f "$PICO_GATEWAY_PID"
        log "gateway stopped pid=$pid"
    fi
}

weixin_cleanup_auth_if_ready()
{
    [ "$(weixin_config_state)" = "ready" ] || return 0
    pid="$(weixin_auth_pid || true)"
    if [ -n "${pid:-}" ]; then
        kill "$pid" >/dev/null 2>&1 || true
        sleep 1
        kill -9 "$pid" >/dev/null 2>&1 || true
        rm -f "$PICO_WEIXIN_AUTH_PID"
        log "weixin auth completed; cleaned auth pid=$pid"
    fi
    : >"$PICO_WEIXIN_AUTH_LOG"
}

weixin_sync_gateway_if_ready()
{
    [ "$(weixin_config_state)" = "ready" ] || return 0
    [ -x "$PICO_BIN" ] || return 0
    digest="$(weixin_gateway_digest)"
    loaded="$(cat "$PICO_WEIXIN_GATEWAY_DIGEST" 2>/dev/null || true)"
    if [ "$digest" = "$loaded" ] && [ -n "$(gateway_pid || true)" ]; then
        return 0
    fi
    log "weixin config ready; restart gateway to load channel"
    gateway_stop_only
    gateway_start_only
    printf '%s\n' "$digest" >"$PICO_WEIXIN_GATEWAY_DIGEST"
}

weixin_qr_link()
{
    if [ "$(weixin_config_state)" = "ready" ]; then
        return 0
    fi
    if [ -z "$(weixin_auth_pid || true)" ]; then
        return 0
    fi
    sed -n 's/^QR Code Link:[[:space:]]*//p' "$PICO_WEIXIN_AUTH_LOG" 2>/dev/null | tail -n 1
}

weixin_channel_block()
{
    if [ "$(weixin_config_state)" != "ready" ]; then
        return 0
    fi
    base_url="$(json_string_value base_url "$PICO_CONFIG" || true)"
    proxy="$(json_string_value proxy "$PICO_CONFIG" || true)"
    [ -n "$base_url" ] || base_url="https://ilinkai.weixin.qq.com/"
    base_url="$(json_escape "$base_url")"
    proxy="$(json_escape "$proxy")"
    cat <<EOF

    "weixin": {
      "enabled": true,
      "type": "weixin",
      "allow_from": [],
      "settings": {
        "base_url": "$base_url",
        "proxy": "$proxy"
      }
    }
EOF
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

    health="missing"
    if [ "$gateway" = "running" ]; then
        if wget -q -T 2 -O - "http://127.0.0.1:${PICO_GATEWAY_PORT}/health" >/dev/null 2>&1; then
            health="ready"
        fi
    fi

    printf 'installed=%s\nversion=%s\ngateway=%s\npid=%s\nhealth=%s\nnetwork=%s\nip=%s\nconfig=%s\nmodel_key=%s\nweixin=%s\nweixin_auth=%s\nweixin_qr=%s\nurl=http://%s:%s\n' \
        "$installed" "$version" "$gateway" "$pid" "$health" "$network" "$ipaddr" "$config" \
        "$(model_api_key_state)" "$(weixin_config_state)" "$(weixin_auth_state)" "$(weixin_qr_link)" "$ipaddr" "$PICO_GATEWAY_PORT"
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
    api_key_raw="${4:-}"
    if [ -z "$api_key_raw" ] && [ -s "$PICO_CONFIG" ]; then
        api_key_raw="$(sed -n 's/.*"api_keys"[[:space:]]*:[[:space:]]*\[[[:space:]]*"\([^"]*\)".*/\1/p' "$PICO_CONFIG" | head -n 1)"
    fi
    if [ -z "$api_key_raw" ]; then
        api_key_raw="$(model_api_key_from_security "$model_name" || true)"
    fi
    api_key="$(json_escape "$api_key_raw")"
    if [ -n "$api_key" ]; then
        api_key_line="\"api_keys\": [\"$api_key\"],"
    else
        api_key_line=""
    fi
    weixin_block="$(weixin_channel_block)"

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
  "channel_list": {$weixin_block
  }
}
EOF
    log "config saved model_name=$model_name model=$model_id api_base=$api_base"
    if [ -n "$(gateway_pid || true)" ]; then
        log "config saved; restart running gateway"
        gateway_stop_only
        gateway_start_only
        if [ "$(weixin_config_state)" = "ready" ]; then
            weixin_gateway_digest >"$PICO_WEIXIN_GATEWAY_DIGEST"
        fi
        printf 'OK: config saved and gateway restarted\n'
    else
        printf 'OK: config saved\n'
    fi
}

migrate_legacy_config_if_needed()
{
    [ -s "$PICO_CONFIG" ] || return 0
    if ! awk '
        /"maixcam"[[:space:]]*:/ {
            in_maixcam = 1
            lines = 0
        }
        in_maixcam && lines < 12 {
            if ($0 ~ /"enabled"[[:space:]]*:[[:space:]]*true/) {
                found = 1
                exit
            }
            lines++
        }
        END { exit found ? 0 : 1 }
    ' "$PICO_CONFIG" 2>/dev/null; then
        return 0
    fi
    model_name_raw="$(json_string_value model_name "$PICO_CONFIG" || true)"
    model_id_raw="$(json_string_value model "$PICO_CONFIG" || true)"
    api_base_raw="$(json_string_value api_base "$PICO_CONFIG" || true)"
    [ -n "$model_name_raw" ] || model_name_raw="k230-agent"
    [ -n "$model_id_raw" ] || model_id_raw="openai/gpt-4o-mini"
    [ -n "$api_base_raw" ] || api_base_raw="https://api.openai.com/v1"
    log "migrate enabled maixcam channel out of default gateway config"
    save_config_cmd "$model_name_raw" "$model_id_raw" "$api_base_raw" "" >/dev/null
}

ask_cmd()
{
    ensure_dirs
    if [ ! -x "$PICO_BIN" ]; then
        printf 'ERROR: picoclaw is not installed\n'
        return 2
    fi
    if [ ! -s "$PICO_CONFIG" ]; then
        save_config_cmd >/dev/null
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
        save_config_cmd >/dev/null
    fi
    migrate_legacy_config_if_needed
    pid="$(gateway_pid || true)"
    if [ -n "${pid:-}" ]; then
        printf 'OK: gateway already running pid=%s\n' "$pid"
        return 0
    fi
    gateway_start_only
    if [ "$(weixin_config_state)" = "ready" ]; then
        weixin_gateway_digest >"$PICO_WEIXIN_GATEWAY_DIGEST"
    fi
    "$0" status
}

gateway_stop_cmd()
{
    gateway_stop_only
    "$0" status
}

weixin_status_cmd()
{
    weixin_cleanup_auth_if_ready
    weixin_sync_gateway_if_ready
    "$0" status
    if [ -s "$PICO_WEIXIN_AUTH_LOG" ]; then
        printf '\n--- weixin-auth log ---\n'
        tail -40 "$PICO_WEIXIN_AUTH_LOG" 2>/dev/null || true
    fi
}

weixin_auth_cmd()
{
    ensure_dirs
    if [ ! -x "$PICO_BIN" ]; then
        printf 'ERROR: picoclaw is not installed\n'
        return 2
    fi
    if [ ! -s "$PICO_CONFIG" ]; then
        save_config_cmd >/dev/null
    fi
    if ! have_network; then
        printf 'ERROR: no network connection\n'
        return 4
    fi

    pid="$(weixin_auth_pid || true)"
    if [ -n "${pid:-}" ]; then
        weixin_status_cmd
        return 0
    fi

    : >"$PICO_WEIXIN_AUTH_LOG"
    log "weixin auth start"
    TZ="$PICO_TZ" SSL_CERT_FILE="$PICO_CA_FILE" PICOCLAW_HOME="$PICO_HOME" PICOCLAW_CONFIG="$PICO_CONFIG" \
        nohup "$PICO_BIN" --no-color auth weixin --timeout 300 \
        >"$PICO_WEIXIN_AUTH_LOG" 2>&1 &
    echo "$!" >"$PICO_WEIXIN_AUTH_PID"

    i=0
    while [ "$i" -lt 15 ]; do
        if [ -n "$(weixin_qr_link)" ] || [ "$(weixin_config_state)" = "ready" ]; then
            break
        fi
        pid="$(weixin_auth_pid || true)"
        [ -n "${pid:-}" ] || break
        sleep 1
        i=$((i + 1))
    done
    weixin_cleanup_auth_if_ready
    weixin_sync_gateway_if_ready
    weixin_status_cmd
}

weixin_cancel_cmd()
{
    pid="$(weixin_auth_pid || true)"
    if [ -n "${pid:-}" ]; then
        kill "$pid" >/dev/null 2>&1 || true
        sleep 1
        kill -9 "$pid" >/dev/null 2>&1 || true
        rm -f "$PICO_WEIXIN_AUTH_PID"
        : >"$PICO_WEIXIN_AUTH_LOG"
        log "weixin auth cancelled pid=$pid"
    fi
    if [ "$(weixin_config_state)" != "ready" ]; then
        : >"$PICO_WEIXIN_AUTH_LOG"
    fi
    weixin_status_cmd
}

log_cmd()
{
    tail -80 "$PICO_LOG" "$PICO_GATEWAY_LOG" "$PICO_WEIXIN_AUTH_LOG" 2>/dev/null || true
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
    weixin-auth)
        weixin_auth_cmd
        ;;
    weixin-status)
        weixin_status_cmd
        ;;
    weixin-cancel)
        weixin_cancel_cmd
        ;;
    log)
        log_cmd
        ;;
    *)
        printf 'Usage: %s {status|install|save-config MODEL_NAME MODEL_ID API_BASE API_KEY|ask PROMPT|gateway-start|gateway-stop|weixin-auth|weixin-status|weixin-cancel|log}\n' "$0"
        exit 64
        ;;
esac
