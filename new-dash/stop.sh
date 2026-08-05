#!/bin/sh
set -eu

NEW_DASH_DIR=$(CDPATH= cd "$(dirname "$0")" && pwd)
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

service_is_absent() {
    NEW_DASH_LAUNCHCTL_OUTPUT=
    if NEW_DASH_LAUNCHCTL_OUTPUT=$(launchctl print "$NEW_DASH_TARGET" 2>&1); then
        return 1
    fi
    case "$NEW_DASH_LAUNCHCTL_OUTPUT" in
        "Could not find service \"$NEW_DASH_LABEL\" in domain for "*)
            return 0
            ;;
        *)
            printf 'Unable to inspect New Dash service %s: %s\n' \
                "$NEW_DASH_TARGET" "$NEW_DASH_LAUNCHCTL_OUTPUT" >&2
            return 2
            ;;
    esac
}

if service_is_absent; then
    :
else
    NEW_DASH_SERVICE_STATE=$?
    if [ "$NEW_DASH_SERVICE_STATE" -eq 2 ]; then
        exit 1
    fi
    if ! launchctl bootout "$NEW_DASH_TARGET"; then
        echo "Unable to stop New Dash service: $NEW_DASH_TARGET" >&2
        exit 1
    fi
    if service_is_absent; then
        :
    else
        NEW_DASH_SERVICE_STATE=$?
        if [ "$NEW_DASH_SERVICE_STATE" -eq 1 ]; then
            echo "New Dash service is still registered: $NEW_DASH_TARGET" >&2
        fi
        exit 1
    fi
fi

rm -f "$NEW_DASH_PLIST" "$NEW_DASH_CONFIG"

printf 'New Dash service stopped. History was preserved: %s\n' \
    "$NEW_DASH_SUPPORT_DIR/new-dash.sqlite3"
printf 'Logs were preserved: %s and %s\n' "$NEW_DASH_STDOUT" "$NEW_DASH_STDERR"
