#!/bin/sh
set -eu

NEW_DASH_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
NEW_DASH_VENV="$NEW_DASH_DIR/.venv"
NEW_DASH_START="$NEW_DASH_DIR/start.sh"

NEW_DASH_LABEL=io.friendorfoe.new-dash
NEW_DASH_DOMAIN="gui/$(id -u)"
NEW_DASH_TARGET="$NEW_DASH_DOMAIN/$NEW_DASH_LABEL"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "New Dash overnight service is supported only on macOS." >&2
    exit 1
fi

if [ -z "${HOME:-}" ] || [ ! -d "$HOME" ]; then
    echo "HOME must name an existing directory." >&2
    exit 1
fi

NEW_DASH_SUPPORT_DIR="$HOME/Library/Application Support/New Dash"
NEW_DASH_CONFIG="$NEW_DASH_SUPPORT_DIR/service.conf"
NEW_DASH_LOG_DIR="$HOME/Library/Logs/New Dash"
NEW_DASH_STDOUT="$NEW_DASH_LOG_DIR/service.log"
NEW_DASH_STDERR="$NEW_DASH_LOG_DIR/service-error.log"
NEW_DASH_PLIST="$HOME/Library/LaunchAgents/$NEW_DASH_LABEL.plist"

usage() {
    cat <<'EOF'
Usage: ./start.sh [--http-port PORT] [--port /dev/cu.*]

Register and start the New Dash overnight macOS service.
EOF
}

valid_port() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
    esac

    NEW_DASH_PORT_NORMALIZED=$(printf '%s' "$1" | sed 's/^0*//')
    if [ -z "$NEW_DASH_PORT_NORMALIZED" ]; then
        return 1
    fi
    if [ "${#NEW_DASH_PORT_NORMALIZED}" -gt 5 ]; then
        return 1
    fi
    [ "$NEW_DASH_PORT_NORMALIZED" -le 65535 ]
}

valid_badge_port() {
    case "$1" in
        /dev/cu.*) return 0 ;;
        *) return 1 ;;
    esac
}

xml_escape() {
    printf '%s' "$1" | sed \
        -e 's/&/\&amp;/g' \
        -e 's/</\&lt;/g' \
        -e 's/>/\&gt;/g' \
        -e 's/"/\&quot;/g' \
        -e "s/'/\&apos;/g"
}

run_service() {
    if [ ! -r "$NEW_DASH_CONFIG" ]; then
        echo "New Dash service configuration is missing: $NEW_DASH_CONFIG" >&2
        exit 1
    fi
    if [ ! -x "$NEW_DASH_VENV/bin/python" ]; then
        echo "New Dash virtual environment is missing: $NEW_DASH_VENV" >&2
        exit 1
    fi

    NEW_DASH_HTTP_PORT=
    NEW_DASH_BADGE_PORT=
    {
        IFS= read -r NEW_DASH_HTTP_PORT
        IFS= read -r NEW_DASH_BADGE_PORT || :
    } < "$NEW_DASH_CONFIG"

    if ! valid_port "$NEW_DASH_HTTP_PORT"; then
        echo "Invalid service HTTP port." >&2
        exit 1
    fi
    if [ -n "$NEW_DASH_BADGE_PORT" ] && ! valid_badge_port "$NEW_DASH_BADGE_PORT"; then
        echo "Invalid service badge port." >&2
        exit 1
    fi

    PYTHONUNBUFFERED=1
    export PYTHONUNBUFFERED
    set -- --no-browser --http-port "$NEW_DASH_HTTP_PORT"
    if [ -n "$NEW_DASH_BADGE_PORT" ]; then
        set -- "$@" --port "$NEW_DASH_BADGE_PORT"
    fi
    exec /usr/bin/caffeinate -i "$NEW_DASH_VENV/bin/python" -m new_dash "$@"
}

if [ "$#" -eq 1 ] && [ "$1" = "--service" ]; then
    run_service
fi

NEW_DASH_HTTP_PORT=18888
NEW_DASH_BADGE_PORT=
NEW_DASH_BADGE_PORT_SET=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --help)
            usage
            exit 0
            ;;
        --http-port)
            if [ "$#" -lt 2 ]; then
                echo "--http-port requires a value." >&2
                exit 1
            fi
            NEW_DASH_HTTP_PORT=$2
            shift 2
            ;;
        --port)
            if [ "$#" -lt 2 ]; then
                echo "--port requires a value." >&2
                exit 1
            fi
            NEW_DASH_BADGE_PORT=$2
            NEW_DASH_BADGE_PORT_SET=1
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if ! valid_port "$NEW_DASH_HTTP_PORT"; then
    echo "HTTP port must be a decimal value from 1 through 65535." >&2
    exit 1
fi
if [ "$NEW_DASH_BADGE_PORT_SET" -eq 1 ] && ! valid_badge_port "$NEW_DASH_BADGE_PORT"; then
    echo "Badge port must be under /dev/cu.*." >&2
    exit 1
fi

if [ ! -x "$NEW_DASH_VENV/bin/python" ]; then
    python3 -m venv "$NEW_DASH_VENV"
fi
"$NEW_DASH_VENV/bin/python" -m pip install -e "$NEW_DASH_DIR"

mkdir -p "$NEW_DASH_SUPPORT_DIR" "$NEW_DASH_LOG_DIR" "$HOME/Library/LaunchAgents"
umask 077
NEW_DASH_CONFIG_TMP=
NEW_DASH_PLIST_TMP=
cleanup() {
    if [ -n "$NEW_DASH_CONFIG_TMP" ]; then
        rm -f "$NEW_DASH_CONFIG_TMP"
    fi
    if [ -n "$NEW_DASH_PLIST_TMP" ]; then
        rm -f "$NEW_DASH_PLIST_TMP"
    fi
}
trap cleanup EXIT HUP INT TERM
NEW_DASH_CONFIG_TMP=$(mktemp "$NEW_DASH_SUPPORT_DIR/service.conf.XXXXXX")
NEW_DASH_PLIST_TMP=$(mktemp "$HOME/Library/LaunchAgents/$NEW_DASH_LABEL.plist.XXXXXX")

printf '%s\n%s\n' "$NEW_DASH_HTTP_PORT" "$NEW_DASH_BADGE_PORT" > "$NEW_DASH_CONFIG_TMP"
mv -f "$NEW_DASH_CONFIG_TMP" "$NEW_DASH_CONFIG"
chmod 600 "$NEW_DASH_CONFIG"

NEW_DASH_START_XML=$(xml_escape "$NEW_DASH_START")
NEW_DASH_STDOUT_XML=$(xml_escape "$NEW_DASH_STDOUT")
NEW_DASH_STDERR_XML=$(xml_escape "$NEW_DASH_STDERR")
cat > "$NEW_DASH_PLIST_TMP" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>Label</key><string>io.friendorfoe.new-dash</string>
  <key>ProgramArguments</key>
  <array>
    <string>/bin/sh</string>
    <string>$NEW_DASH_START_XML</string>
    <string>--service</string>
  </array>
  <key>RunAtLoad</key><true/>
  <key>KeepAlive</key><true/>
  <key>ThrottleInterval</key><integer>5</integer>
  <key>StandardOutPath</key><string>$NEW_DASH_STDOUT_XML</string>
  <key>StandardErrorPath</key><string>$NEW_DASH_STDERR_XML</string>
</dict>
</plist>
EOF
mv -f "$NEW_DASH_PLIST_TMP" "$NEW_DASH_PLIST"
chmod 600 "$NEW_DASH_PLIST"

if launchctl print "$NEW_DASH_TARGET" >/dev/null 2>&1; then
    launchctl bootout "$NEW_DASH_TARGET"
fi
launchctl bootstrap "$NEW_DASH_DOMAIN" "$NEW_DASH_PLIST"

printf 'New Dash dashboard: http://127.0.0.1:%s/\n' "$NEW_DASH_HTTP_PORT"
printf 'Service label: %s\n' "$NEW_DASH_LABEL"
printf 'Stop with: %s\n' "$NEW_DASH_DIR/stop.sh"
printf 'Service log: %s\n' "$NEW_DASH_STDOUT"
printf 'Service error log: %s\n' "$NEW_DASH_STDERR"
