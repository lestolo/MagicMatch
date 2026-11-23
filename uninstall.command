#!/bin/bash
# MagicMatch uninstaller for macOS
# GPL-3.0 — StrandLabs

BUNDLE="MagicMatch.ofx.bundle"
SYS_PATH="/Library/OFX/Plugins/$BUNDLE"
USER_PATH="$HOME/Library/OFX/Plugins/$BUNDLE"

cat <<'HEADER'

──────────────────────────────────────────────────────────────
                   MagicMatch Uninstaller
                  GPL-3.0 — StrandLabs
──────────────────────────────────────────────────────────────

HEADER

FOUND=0
[ -e "$SYS_PATH" ] && { echo "Found: $SYS_PATH"; FOUND=1; }
[ -e "$USER_PATH" ] && { echo "Found: $USER_PATH"; FOUND=1; }

if [ "$FOUND" -eq 0 ]; then
    echo "MagicMatch is not installed. Nothing to do."
    echo ""
    read -n 1 -s -r -p "Press any key to close..."
    echo ""
    exit 0
fi

echo ""
echo "Exported CDL files in ~/Documents/MagicMatch/CDL/ will NOT be removed."
echo "Administrator password may be requested."
echo ""
read -n 1 -s -r -p "Press ENTER to uninstall, or Ctrl+C to cancel..."
echo ""
echo ""

[ -e "$SYS_PATH" ] && sudo rm -rf "$SYS_PATH"
[ -e "$USER_PATH" ] && rm -rf "$USER_PATH"

if [ -e "$SYS_PATH" ] || [ -e "$USER_PATH" ]; then
    echo "ERROR: could not remove all files. Delete the folders listed above manually."
else
    echo "✓ MagicMatch uninstalled."
    echo ""
    echo "Quit DaVinci Resolve (Cmd+Q) and reopen it to refresh the plugin list."
fi

echo ""
read -n 1 -s -r -p "Press any key to close this window..."
echo ""
