#!/bin/sh
# Install the replacement WebClient.exe and the helper into the game directory.
#
# Safe to run repeatedly: the backup is only ever taken from a genuine
# original, identified by its mscoree.dll import (the replacement is native
# code and has none). Re-running with the replacement already in place
# refreshes it and leaves the backup alone.
#
# usage: ./install.sh [game-directory]

set -e

HERE=$(cd "$(dirname "$0")" && pwd)
GAME=${1:-${XAL_GAME_DIR:-"$HOME/.local/share/Steam/steamapps/common/AoE2DE"}}

SHIM="$HERE/WebClient.exe"
LIVE="$GAME/WebClient.exe"
ORIG="$GAME/WebClient.exe.orig"

[ -f "$SHIM" ] || { echo "build it first: make" >&2; exit 1; }
[ -f "$LIVE" ] || { echo "no WebClient.exe in $GAME" >&2; exit 1; }

if grep -qa "mscoree.dll" "$LIVE"; then
    if [ -e "$ORIG" ]; then
        echo "backup already exists, leaving it alone: $ORIG"
    else
        cp "$LIVE" "$ORIG"
        echo "backed up original -> $ORIG"
    fi
else
    echo "WebClient.exe is already the replacement; backup untouched"
    if [ ! -e "$ORIG" ]; then
        echo "REFUSING: no backup exists and the original is gone." >&2
        echo "Recover it with Steam > Properties > Installed Files >" >&2
        echo "Verify integrity of game files, then run this again." >&2
        exit 1
    fi
fi

cp "$SHIM" "$LIVE"
cp "$HERE/xal-helper.py" "$HERE/xal-launch.sh" "$GAME/"
rm -f "$GAME/xal-request.txt" "$GAME/xal-result.txt"

