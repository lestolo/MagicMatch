#!/bin/bash
# MagicMatch installer for macOS
# GPL-3.0 — StrandLabs

set -e

# Bellissimo header
cat <<'HEADER'

──────────────────────────────────────────────────────────────
                    MagicMatch Installer
                  GPL-3.0 — StrandLabs
──────────────────────────────────────────────────────────────

HEADER

# Trova la cartella in cui si trova questo script
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUNDLE="MagicMatch.ofx.bundle"
SRC_BUNDLE="$SCRIPT_DIR/$BUNDLE"
DEST_DIR="/Library/OFX/Plugins"

# Verifica che il bundle sia presente accanto allo script
if [ ! -d "$SRC_BUNDLE" ]; then
    echo "ERROR: $BUNDLE not found next to this installer."
    echo "       Please keep install.command in the same folder as the plugin."
    echo ""
    read -n 1 -s -r -p "Press any key to close..."
    exit 1
fi

# Controlla architettura Mac
ARCH="$(uname -m)"
if [ "$ARCH" != "arm64" ]; then
    echo "WARNING: this Mac reports architecture '$ARCH'."
    echo "         MagicMatch is currently built for Apple Silicon only."
    echo "         Installation will continue but the plugin may not load."
    echo ""
fi

echo "This will install MagicMatch into:"
echo "  $DEST_DIR/$BUNDLE"
echo ""
echo "Administrator password may be requested."
echo ""
read -n 1 -s -r -p "Press ENTER to continue, or Ctrl+C to cancel..."
echo ""
echo ""

# Installazione
echo "Installing..."
sudo mkdir -p "$DEST_DIR"
sudo rm -rf "$DEST_DIR/$BUNDLE"
sudo cp -R "$SRC_BUNDLE" "$DEST_DIR/"

# Rimuove il quarantine attribute (evita prompt di macOS Gatekeeper)
sudo xattr -cr "$DEST_DIR/$BUNDLE" 2>/dev/null || true

# Fix permessi
sudo chmod -R 755 "$DEST_DIR/$BUNDLE"

echo ""
echo "✓ MagicMatch installed successfully."
echo ""
echo "Next steps:"
echo "  1. Quit DaVinci Resolve if it's running (Cmd+Q)."
echo "  2. Re-open DaVinci Resolve."
echo "  3. In the Color page, find MagicMatch under OpenFX → Color."
echo ""
echo "Questions? Write to support@strandlabs.it"
echo ""
read -n 1 -s -r -p "Press any key to close this window..."
echo ""
