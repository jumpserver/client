#!/bin/sh
# Run after installing the native package in a disposable target distribution.
set -eu
test -x /usr/bin/client
test -f /usr/lib/client/bin/config.json
test -f /usr/share/icons/hicolor/512x512/apps/client.png
test -f /usr/share/licenses/client/LICENSE
count=0
for launcher in /usr/lib/client/bin/linux-*/JumpServerClient; do
    test -x "$launcher"
    test -x "$(dirname "$launcher")/client"
    count=$((count + 1))
done
test "$count" -eq 1
desktop-file-validate /usr/share/applications/client.desktop
grep -q 'x-scheme-handler/jms=.*client.desktop' /usr/share/applications/mimeinfo.cache
dependencies=$(ldd /usr/bin/client)
printf '%s\n' "$dependencies"
if printf '%s\n' "$dependencies" | grep -q 'not found'; then
    exit 1
fi
# Offscreen startup alone would not detect a missing normal desktop plugin.
xcb_count=0
for plugin in /usr/lib*/qt*/plugins/platforms/libqxcb.so /usr/lib/*/qt*/plugins/platforms/libqxcb.so; do
    if [ -f "$plugin" ]; then
        plugin_dependencies=$(ldd "$plugin")
        if printf '%s\n' "$plugin_dependencies" | grep -q 'not found'; then
            printf '%s\n' "$plugin_dependencies"
            exit 1
        fi
        xcb_count=$((xcb_count + 1))
    fi
done
test "$xcb_count" -gt 0
printf '%s\n' 'XCB desktop platform plugin dependencies resolved.'
# A temporary home prevents the smoke test from opening an existing profile.
smoke_home=$(mktemp -d)
trap 'rm -rf "$smoke_home"' EXIT HUP INT TERM
mkdir -p "$smoke_home/config" "$smoke_home/cache" "$smoke_home/runtime"
chmod 700 "$smoke_home/runtime"
HOME="$smoke_home" XDG_CONFIG_HOME="$smoke_home/config" \
    XDG_CACHE_HOME="$smoke_home/cache" XDG_RUNTIME_DIR="$smoke_home/runtime" \
    QT_QPA_PLATFORM=offscreen /usr/bin/client --smoke-test
printf '%s\n' 'Installed package smoke test passed.'
