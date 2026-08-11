#!/bin/sh
# Restore the original WebClient.exe and remove the helper's files.
#
# usage: ./uninstall.sh [game-directory]

set -e

GAME=${1:-${XAL_GAME_DIR:-"$HOME/.local/share/Steam/steamapps/common/AoE2DE"}}

LIVE="$GAME/WebClient.exe"
ORIG="$GAME/WebClient.exe.orig"

if [ ! -e "$ORIG" ]; then
    echo "no backup at $ORIG" >&2
    echo "Recover the original with Steam > Properties > Installed Files >" >&2
    echo "Verify integrity of game files." >&2
    exit 1
fi

cp "$ORIG" "$LIVE"
rm -f "$ORIG"
rm -f "$GAME/xal-helper.py" "$GAME/xal-launch.sh" \
      "$GAME/xal-request.txt" "$GAME/xal-result.txt" "$GAME/xal-helper.lock"

echo "restored original -> $LIVE"
