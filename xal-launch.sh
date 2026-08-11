#!/bin/sh
# Started by WebClient.exe from inside the Wine prefix, via `start.exe /unix`.
# You do not run this yourself.
#
# The game runs inside Steam's pressure-vessel container, where the host's
# Firefox is not usable directly. steam-runtime-launch-client --host escapes
# that and runs the helper on the real desktop. Outside a container (for
# instance when testing with `proton runinprefix`) it is not present, and
# running the helper directly is already correct.
#
# Exits quietly if a helper is already running: XAL performs sign-in as several
# stages, each spawning its own WebClient.exe, and one helper handles them all.

# -P resolves symlinks: Wine invokes this through the prefix's dosdevices
# drive links, so a logical path would carry a "dosdevices/s:" segment around.
GAME=$(cd "$(dirname "$0")" && pwd -P)
HELPER="$GAME/xal-helper.py"
LOG=${XAL_LOG:-/tmp/xal-helper.log}

[ -f "$HELPER" ] || exit 0

# No process check here on purpose: this script runs inside pressure-vessel's
# PID namespace while the helper runs on the host, so pgrep cannot see it and
# would happily start a second one for every stage. The helper takes a lock on
# xal-helper.lock instead and exits by itself if one is already running.

if command -v steam-runtime-launch-client >/dev/null 2>&1; then
    exec steam-runtime-launch-client --host -- \
        python3 -u "$HELPER" --game "$GAME" >>"$LOG" 2>&1
fi

exec python3 -u "$HELPER" --game "$GAME" >>"$LOG" 2>&1
