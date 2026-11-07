#!/bin/sh
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || true
fi
# This does not run xdg-mime default or remove the user's configuration.
exit 0
