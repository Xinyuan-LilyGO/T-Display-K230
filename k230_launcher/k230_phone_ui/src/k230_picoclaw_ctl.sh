#!/bin/sh

set -u

APP_DIR="${APP_DIR:-/root/app/k230_phone_ui}"
PICO_HOME="${PICO_HOME:-/root/picoclaw}"
PICO_BIN_DIR="${PICO_BIN_DIR:-$PICO_HOME/bin}"
PICO_BIN="${PICO_BIN:-$PICO_BIN_DIR/picoclaw}"
PICO_CONFIG="${PICO_CONFIG:-$PICO_HOME/config.json}"
PICO_SECURITY="${PICO_SECURITY:-$PICO_HOME/.security.yml}"
PICO_WORKSPACE="${PICO_WORKSPACE:-$PICO_HOME/workspace}"
PICO_LOG="${PICO_LOG:-/tmp/k230_picoclaw_ui.log}"
PICO_GATEWAY_LOG="${PICO_GATEWAY_LOG:-/tmp/k230_picoclaw_gateway.log}"
PICO_GATEWAY_PID="${PICO_GATEWAY_PID:-/tmp/k230_picoclaw_gateway.pid}"
PICO_WEIXIN_AUTH_LOG="${PICO_WEIXIN_AUTH_LOG:-/tmp/k230_picoclaw_weixin_auth.log}"
PICO_WEIXIN_AUTH_PID="${PICO_WEIXIN_AUTH_PID:-/tmp/k230_picoclaw_weixin_auth.pid}"
PICO_WEIXIN_GATEWAY_DIGEST="${PICO_WEIXIN_GATEWAY_DIGEST:-/tmp/k230_picoclaw_weixin_gateway.digest}"
UI_PREFS_DIR="${UI_PREFS_DIR:-/root/.config/k230_phone_ui}"
UI_PREFS_FILE="${UI_PREFS_FILE:-$UI_PREFS_DIR/settings.conf}"
PICO_PROFILE_COUNT_KEY="picoclaw.profile.count"
PICO_PROFILE_ACTIVE_KEY="picoclaw.profile.active"
PICO_PROFILE_MAX=8
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

prefs_ensure_dir()
{
    mkdir -p "$UI_PREFS_DIR"
}

pref_get()
{
    key="${1:-}"
    [ -n "$key" ] || return 1
    [ -s "$UI_PREFS_FILE" ] || return 1
    awk -F= -v target="$key" '
        function trim(v) {
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
            return v
        }
        {
            line = $0
            if(line ~ /^[[:space:]]*#/ || line !~ /=/) {
                next
            }
            split(line, parts, "=")
            key = trim(parts[1])
            if(key == target) {
                sub(/^[^=]*=/, "", line)
                print trim(line)
                exit
            }
        }
    ' "$UI_PREFS_FILE"
}

pref_set()
{
    key="${1:-}"
    value="${2:-}"
    [ -n "$key" ] || return 1
    prefs_ensure_dir

    tmp="${UI_PREFS_FILE}.tmp.$$"
    if [ -s "$UI_PREFS_FILE" ]; then
        awk -v target="$key" -v new_value="$value" '
            function trim(v) {
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
                return v
            }
            BEGIN {
                found = 0
            }
            {
                line = $0
                if(line ~ /^[[:space:]]*#/ || line !~ /=/) {
                    print line
                    next
                }
                split(line, parts, "=")
                key = trim(parts[1])
                if(key == target) {
                    print target "=" new_value
                    found = 1
                    next
                }
                print line
            }
            END {
                if(!found) {
                    print target "=" new_value
                }
            }
        ' "$UI_PREFS_FILE" >"$tmp"
    else
        {
            printf '# k230_phone_ui persistent settings\n'
            printf '%s=%s\n' "$key" "$value"
        } >"$tmp"
    fi
    mv "$tmp" "$UI_PREFS_FILE"
}

profile_key()
{
    printf 'picoclaw.profile.%s.%s\n' "${1:-0}" "${2:-title}"
}

profile_count_raw()
{
    count="$(pref_get "$PICO_PROFILE_COUNT_KEY" 2>/dev/null || true)"
    case "$count" in
        ''|*[!0-9]*)
            count=0
            ;;
    esac
    if [ "$count" -lt 0 ]; then
        count=0
    fi
    if [ "$count" -gt "$PICO_PROFILE_MAX" ]; then
        count="$PICO_PROFILE_MAX"
    fi
    printf '%s\n' "$count"
}

profile_write()
{
    index="${1:-0}"
    title="${2:-}"
    model_name="${3:-}"
    model_id="${4:-}"
    api_base="${5:-}"
    api_key="${6:-}"

    pref_set "$(profile_key "$index" title)" "$title"
    pref_set "$(profile_key "$index" model_name)" "$model_name"
    pref_set "$(profile_key "$index" model_id)" "$model_id"
    pref_set "$(profile_key "$index" api_base)" "$api_base"
    pref_set "$(profile_key "$index" api_key)" "$api_key"
}

profile_seed_defaults()
{
    if [ "$(profile_count_raw)" -gt 0 ]; then
        return 0
    fi

    profile_write 0 "DeepSeek Chat" "deepseek-chat" "deepseek/deepseek-chat" "https://api.deepseek.com/v1" ""
    profile_write 1 "DeepSeek Reasoner" "deepseek-reasoner" "deepseek/deepseek-reasoner" "https://api.deepseek.com/v1" ""
    profile_write 2 "GPT-4o mini" "gpt-4o-mini" "openai/gpt-4o-mini" "https://api.openai.com/v1" ""
    profile_write 3 "OpenAI compatible" "custom-agent" "openai/gpt-4o-mini" "https://api.openai.com/v1" ""
    pref_set "$PICO_PROFILE_ACTIVE_KEY" "0"
    pref_set "$PICO_PROFILE_COUNT_KEY" "4"
}

profile_field()
{
    pref_get "$(profile_key "${1:-0}" "${2:-title}")" 2>/dev/null || true
}

profile_find()
{
    model_name="${1:-}"
    model_id="${2:-}"
    api_base="${3:-}"
    count="$(profile_count_raw)"
    i=0
    while [ "$i" -lt "$count" ]; do
        if [ "$(profile_field "$i" model_name)" = "$model_name" ] &&
           [ "$(profile_field "$i" model_id)" = "$model_id" ] &&
           [ "$(profile_field "$i" api_base)" = "$api_base" ]; then
            printf '%s\n' "$i"
            return 0
        fi
        i=$((i + 1))
    done
    return 1
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
    awk -v target0="${model_name}:0:" -v target1="${model_name}:" '
        function clean_value(v) {
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", v)
            gsub(/^"|"$/, "", v)
            gsub(/^'\''|'\''$/, "", v)
            return v
        }
        /^model_list:/ {
            in_models = 1
            next
        }
        in_models && /^[^[:space:]]/ {
            exit
        }
        in_models && ($1 == target0 || $1 == target1) {
            in_target = 1
            next
        }
        in_target && /^[[:space:]]{2}[^[:space:]].*:/ {
            exit
        }
        in_target && /api_keys:[[:space:]]*\[/ {
            line = $0
            sub(/^.*api_keys:[[:space:]]*\[[[:space:]]*/, "", line)
            sub(/[[:space:]]*\].*$/, "", line)
            split(line, parts, ",")
            value = clean_value(parts[1])
            if(value != "") {
                print value
                exit
            }
        }
        in_target && /^[[:space:]]+-[[:space:]]+/ {
            sub(/^[[:space:]]+-[[:space:]]+/, "")
            print clean_value($0)
            exit
        }
    ' "$PICO_SECURITY"
}

model_api_key_state()
{
    model_name="$(json_string_value model_name "$PICO_CONFIG" || true)"
    if [ -n "$(model_api_key_from_security "$model_name" || true)" ]; then
        printf 'ready\n'
        return
    fi
    printf 'missing\n'
}

yaml_single_quote()
{
    printf "'%s'" "$(printf '%s' "${1:-}" | sed "s/'/''/g")"
}

model_security_write_key()
{
    model_name="${1:-}"
    api_key="${2:-}"
    [ -n "$model_name" ] || return 0
    [ -n "$api_key" ] || return 0

    tmp="${PICO_SECURITY}.tmp.$$"
    if [ -s "$PICO_SECURITY" ]; then
        awk '
            /^model_list:/ {
                skip = 1
                next
            }
            skip && /^[^[:space:]]/ {
                skip = 0
            }
            !skip {
                print
            }
        ' "$PICO_SECURITY" >"$tmp"
    else
        : >"$tmp"
    fi

    {
        printf '\nmodel_list:\n'
        printf '  %s:0:\n' "$model_name"
        printf '    api_keys:\n'
        printf '      - %s\n' "$(yaml_single_quote "$api_key")"
    } >>"$tmp"
    chmod 0600 "$tmp"
    mv "$tmp" "$PICO_SECURITY"
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
    model_name_raw="${1:-k230-agent}"
    model_id_raw="${2:-openai/gpt-4o-mini}"
    api_base_raw="${3:-https://api.openai.com/v1}"
    model_name="$(json_escape "$model_name_raw")"
    model_id="$(json_escape "$model_id_raw")"
    api_base="$(json_escape "$api_base_raw")"
    api_key_raw="${4:-}"
    if [ -z "$api_key_raw" ]; then
        api_key_raw="$(model_api_key_from_security "$model_name_raw" || true)"
    fi
    if [ -n "$api_key_raw" ]; then
        model_security_write_key "$model_name_raw" "$api_key_raw"
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

profile_add_cmd()
{
    ensure_dirs
    profile_seed_defaults

    model_name="${1:-}"
    model_id="${2:-}"
    api_base="${3:-}"
    api_key="${4:-}"
    title="${5:-$model_name}"

    if [ -z "$model_name" ] || [ -z "$model_id" ] ||
       [ -z "$api_base" ] || [ -z "$api_key" ]; then
        printf 'ERROR: usage profile-add MODEL_NAME MODEL_ID API_BASE API_KEY [TITLE]\n'
        return 64
    fi

    index="$(profile_find "$model_name" "$model_id" "$api_base" || true)"
    if [ -z "$index" ]; then
        index="$(profile_count_raw)"
        if [ "$index" -ge "$PICO_PROFILE_MAX" ]; then
            printf 'ERROR: profile limit reached (%s)\n' "$PICO_PROFILE_MAX"
            return 2
        fi
        pref_set "$PICO_PROFILE_COUNT_KEY" "$((index + 1))"
        action="added"
    else
        action="updated"
    fi

    profile_write "$index" "$title" "$model_name" "$model_id" "$api_base" "$api_key"
    pref_set "$PICO_PROFILE_ACTIVE_KEY" "$index"
    save_config_cmd "$model_name" "$model_id" "$api_base" "$api_key" >/dev/null
    log "profile $action index=$index model_name=$model_name model=$model_id api_base=$api_base"
    printf 'OK: profile %s index=%s active=yes key=ready\n' "$action" "$index"
}

profile_use_cmd()
{
    ensure_dirs
    profile_seed_defaults
    index="${1:-}"
    count="$(profile_count_raw)"

    case "$index" in
        ''|*[!0-9]*)
            printf 'ERROR: usage profile-use INDEX\n'
            return 64
            ;;
    esac
    if [ "$index" -lt 0 ] || [ "$index" -ge "$count" ]; then
        printf 'ERROR: profile index out of range\n'
        return 2
    fi

    model_name="$(profile_field "$index" model_name)"
    model_id="$(profile_field "$index" model_id)"
    api_base="$(profile_field "$index" api_base)"
    api_key="$(profile_field "$index" api_key)"
    pref_set "$PICO_PROFILE_ACTIVE_KEY" "$index"
    save_config_cmd "$model_name" "$model_id" "$api_base" "$api_key" >/dev/null
    log "profile selected index=$index model_name=$model_name model=$model_id api_base=$api_base"
    printf 'OK: active profile index=%s\n' "$index"
}

profile_list_cmd()
{
    ensure_dirs
    profile_seed_defaults
    count="$(profile_count_raw)"
    active="$(pref_get "$PICO_PROFILE_ACTIVE_KEY" 2>/dev/null || true)"
    case "$active" in
        ''|*[!0-9]*)
            active=0
            ;;
    esac

    i=0
    while [ "$i" -lt "$count" ]; do
        marker=" "
        [ "$i" = "$active" ] && marker="*"
        title="$(profile_field "$i" title)"
        model_name="$(profile_field "$i" model_name)"
        model_id="$(profile_field "$i" model_id)"
        api_base="$(profile_field "$i" api_base)"
        if [ -n "$(profile_field "$i" api_key)" ]; then
            key_state="ready"
        else
            key_state="missing"
        fi
        printf '%s %s | %s | %s | %s | %s | key=%s\n' \
            "$marker" "$i" "$title" "$model_name" "$model_id" "$api_base" "$key_state"
        i=$((i + 1))
    done
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
    if [ "$(weixin_config_state)" = "ready" ]; then
        weixin_status_cmd
        return 0
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

weixin_remove_security_token()
{
    [ -s "$PICO_SECURITY" ] || return 0
    tmp="${PICO_SECURITY}.tmp.$$"
    awk '
        /^channel_list:/ {
            in_channels = 1
            in_weixin = 0
            print
            next
        }
        in_channels && /^[^[:space:]]/ {
            in_channels = 0
            in_weixin = 0
        }
        in_channels && /^  weixin:/ {
            in_weixin = 1
            print
            next
        }
        in_channels && /^  [^ ].*:/ {
            in_weixin = 0
        }
        in_weixin && /^[[:space:]]+token:/ {
            next
        }
        { print }
    ' "$PICO_SECURITY" >"$tmp" && mv "$tmp" "$PICO_SECURITY"
}

weixin_disable_config()
{
    [ -s "$PICO_CONFIG" ] || return 0
    tmp="${PICO_CONFIG}.tmp.$$"
    awk '
        /"weixin"[[:space:]]*:[[:space:]]*\{/ {
            in_weixin = 1
            changed = 0
        }
        in_weixin && !changed && /"enabled"[[:space:]]*:/ {
            sub(/true|false/, "false")
            changed = 1
        }
        in_weixin && /^[[:space:]]*\}/ {
            in_weixin = 0
        }
        { print }
    ' "$PICO_CONFIG" >"$tmp" && mv "$tmp" "$PICO_CONFIG"
}

weixin_unbind_cmd()
{
    ensure_dirs
    pid="$(weixin_auth_pid || true)"
    if [ -n "${pid:-}" ]; then
        kill "$pid" >/dev/null 2>&1 || true
        sleep 1
        kill -9 "$pid" >/dev/null 2>&1 || true
    fi
    rm -f "$PICO_WEIXIN_AUTH_PID" "$PICO_WEIXIN_GATEWAY_DIGEST"
    : >"$PICO_WEIXIN_AUTH_LOG"
    weixin_remove_security_token
    weixin_disable_config
    rm -rf "$PICO_HOME/channels/weixin"
    if [ -n "$(gateway_pid || true)" ]; then
        log "weixin unbound; restart running gateway"
        gateway_stop_only
        gateway_start_only
    else
        log "weixin unbound"
    fi
    "$0" status
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
    profile-add|profile-set)
        shift
        profile_add_cmd "$@"
        ;;
    profile-use)
        shift
        profile_use_cmd "$@"
        ;;
    profile-list)
        profile_list_cmd
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
    weixin-unbind)
        weixin_unbind_cmd
        ;;
    log)
        log_cmd
        ;;
    *)
        printf 'Usage: %s {status|install|save-config MODEL_NAME MODEL_ID API_BASE API_KEY|profile-add MODEL_NAME MODEL_ID API_BASE API_KEY [TITLE]|profile-use INDEX|profile-list|ask PROMPT|gateway-start|gateway-stop|weixin-auth|weixin-status|weixin-cancel|weixin-unbind|log}\n' "$0"
        exit 64
        ;;
esac
