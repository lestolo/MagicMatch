# MagicMatch: Installation Guide

## Requirements

- Mac with Apple Silicon (M1 or later), macOS 13 or newer
- DaVinci Resolve **Studio** 18 or later (the free version does not load OFX plugins)

## Install (recommended)

1. Download `MagicMatch-1.3b-macOS-arm64.zip` from the [Releases page](https://github.com/lestolo/MagicMatch/releases) (newest version on top) and unzip it.
2. Double-click `install.command`.
   - If macOS says it cannot be opened: right-click the file → **Open** → **Open**.
3. Press Enter and type your Mac password when asked. The plugin is copied to `/Library/OFX/Plugins/`.
4. **Quit DaVinci Resolve completely (⌘Q) and reopen it.** Resolve loads plugins only at startup.
5. Open the **Color** page → **OpenFX** panel → **Color** → drag **MagicMatch** onto a node.

## Manual install

If you prefer the Terminal, from the unzipped folder:

```bash
sudo mkdir -p /Library/OFX/Plugins
sudo cp -R MagicMatch.ofx.bundle /Library/OFX/Plugins/
sudo xattr -cr /Library/OFX/Plugins/MagicMatch.ofx.bundle
```

Without admin rights, install for the current user only:

```bash
mkdir -p ~/Library/OFX/Plugins
cp -R MagicMatch.ofx.bundle ~/Library/OFX/Plugins/
xattr -cr ~/Library/OFX/Plugins/MagicMatch.ofx.bundle
```

## First test

1. On a clip, add MagicMatch and click **Grab Reference**.
2. Go to a second clip of the same scene, add MagicMatch and click **Auto Match**.
3. The status line reads `Matched using grabbed ref.` and the CDL values update.

For best results work in a log space with a Color Space Transform before and after the node. See the [README](README.md#usage).

## Update

Run the new `install.command`: it replaces the previous version. Restart Resolve afterwards.

## Uninstall

Double-click `uninstall.command` from the release folder (right-click → **Open** if macOS blocks it). Or manually:

```bash
sudo rm -rf /Library/OFX/Plugins/MagicMatch.ofx.bundle
rm -rf ~/Library/OFX/Plugins/MagicMatch.ofx.bundle
```

Exported CDL files are in `~/Documents/MagicMatch/CDL/` and are not removed.

## Troubleshooting

| Problem | Fix |
|---|---|
| MagicMatch is not in the OpenFX list | Fully quit and reopen Resolve; check the bundle is in `/Library/OFX/Plugins/`; run `sudo xattr -cr /Library/OFX/Plugins/MagicMatch.ofx.bundle` |
| OpenFX panel is empty or missing | You need Resolve **Studio**; the free version has no OFX support |
| macOS blocks `install.command` | Right-click → **Open**. The build is not code-signed yet |
| Plugin listed but does not load | Check the Mac is Apple Silicon (`uname -m` must print `arm64`) |
| Status says `No reference. Grab one first.` | Click **Grab Reference** on the reference clip first, or connect a Reference clip |

## Build from source

See [Build from source](README.md#build-from-source) in the README.

## Reporting problems

Open a [GitHub issue](https://github.com/lestolo/MagicMatch/issues) with your macOS and Resolve versions, what you did, and a screenshot.
