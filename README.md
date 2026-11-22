<p align="center"><img src="assets/icon.png" alt="MagicMatch logo" width="128"></p>

# MagicMatch

[![License: GPL v3+](https://img.shields.io/badge/license-GPL--3.0--or--later-blue.svg)](LICENSE)
![Platform: macOS arm64](https://img.shields.io/badge/platform-macOS%20arm64-lightgrey.svg)
![Status: beta](https://img.shields.io/badge/status-1.2%20beta-orange.svg)

**Open-source camera-matching plugin for DaVinci Resolve (OpenFX).**

MagicMatch analyzes a *source* and a *reference* image and computes an ASC CDL
(Slope, Offset, Power, Saturation) that brings the source closer to the reference.
It is built for **camera matching** within the same scene: different angles, different
cameras, same look. It runs fully locally: no server, no AI inference, no accounts.

> **Beta.** Tested on macOS Apple Silicon with DaVinci Resolve Studio. Feedback and bug reports are welcome.

## Features

- **Auto Match**: computes Slope / Offset / Saturation from source vs reference.
- **Grab Reference**: stores the reference statistics inside the Resolve project, no export/import of stills.
- **Reference clip input**: alternative to Grab Reference, using an OFX clip as reference.
- **Mask inputs**: analyze only selected areas (e.g. Magic Mask on faces), for source and reference separately.
- **Export CDL**: saves an ASC CDL v1.01 `.cdl` file.
- **Intensity** blend and optional **skin protection**.

## Install

1. Download `MagicMatch-1.2b-macOS-arm64.zip` from the [Releases page](https://github.com/lestolo/MagicMatch/releases) (newest version on top).
2. Unzip, double-click `install.command` (right-click → **Open** if macOS blocks it).
3. Quit and restart DaVinci Resolve.

Full instructions and troubleshooting: [INSTALL.md](INSTALL.md).

## Requirements

- macOS 13+ on Apple Silicon (M1 or later)
- DaVinci Resolve **Studio** 18 or later (the free version does not load OFX plugins)
- Windows: the source is cross-platform-ready, but a Windows build is not provided or tested yet. Contributions welcome.

## Usage

Find the plugin on the Color page under **OpenFX → Color → MagicMatch**.

### Grab Reference workflow (recommended)

1. On the **reference** clip (e.g. CAM A), add a MagicMatch node and click **Grab Reference**.
2. Copy the node to the clip to match (e.g. CAM B).
3. Click **Auto Match**. Slope / Offset / Saturation update automatically.
4. Optionally click **Export CDL** → `~/Documents/MagicMatch/CDL/`.

### Reference clip workflow

Connect a reference image to the node's Reference input and click **Auto Match**.
If a Reference clip is connected it takes priority over grabbed statistics.

### Masks

Connect Magic Mask key outputs to the two blue Key inputs: first = Source Mask, second = Reference Mask.
Only masked areas are analyzed. Masks are optional.

### Work in log

CDL is color-space agnostic, but results are far better in a log space (LogC3, S-Log3, V-Log, ACEScct).
Put a Color Space Transform before MagicMatch and one after to return to your working space.

## Parameters

| Parameter | Description |
|---|---|
| Auto Match | Analyzes source vs reference and writes the CDL values |
| Grab Reference | Stores the current frame's statistics as the reference |
| Export CDL | Saves the current values as an ASC CDL `.cdl` file |
| Match White Point | On by default. Auto Match also aligns exposure/contrast from the white and black points |
| Match Midtones (Power) | Off by default. Auto Match also estimates a Power (0.9–1.1) from the mean luma |
| Intensity | Blend between original (0) and full correction (1) |
| Slope / Offset / Power R,G,B | CDL values (Power is left at 1.0 by Auto Match) |
| Saturation | Global saturation |
| Skin Protection / Strength | Preserve skin tones under strong corrections |
| Clamp Output | Off by default: values above 1.0 (linear, ACEScg, HDR) are preserved. Turn on to limit the output to 0–1 |

Per pixel: `out = (in * Slope + Offset)^Power`, then saturation.

## How it works

Neutral-point analysis. For each image MagicMatch computes a saturation-weighted neutral gray point,
the 1st/99th luma percentiles as black/white points, and a "colorful" saturation estimate.

- **Slope**: ratio of reference/source neutral points per channel (clamped 0.7–1.4). With *Match White Point* it is also scaled by `(ref.white − ref.black) / (src.white − src.black)` (clamped 0.7–1.4)
- **Offset**: one uniform value aligning the black points (clamped ±0.15)
- **Power**: 1.0, or with *Match Midtones* estimated from the mean luma (clamped 0.9–1.1)
- **Saturation**: ratio of colorful saturation (clamped 0.85–1.15)

## Known limitations

- Single-frame analysis (the current timeline frame).
- Exposure differences over ~1–2 stops hit the Slope clamps: correct exposure first.
- Tuned for camera matching, not stylized look transfer.
- Unsigned build: see [INSTALL.md](INSTALL.md) for the Gatekeeper steps.

## Build from source

macOS, Xcode command line tools, and the [OpenFX headers](https://github.com/AcademySoftwareFoundation/openfx):

```bash
git clone https://github.com/AcademySoftwareFoundation/openfx.git ~/Desktop/openfx
make                       # build/MagicMatch.ofx.bundle
make check                 # verify exported symbols
make install               # /Library/OFX/Plugins (sudo)
# or: make install-user    # ~/Library/OFX/Plugins
make release               # distributable zip in build/
```

Use a different SDK location with `make OFX_SDK=/path/to/openfx/include`.

## Contributing

Issues and pull requests are welcome. Keep the code cross-platform: guard OS-specific code with `#ifdef _WIN32`.

## Author

Alessio Pallante, [StrandLabs](https://www.strandlabs.eu/magicmatch), Rome.

## License

[GNU GPL v3.0 or later](LICENSE). Forks and redistributed binaries must remain open source under the same license.
OpenFX headers are BSD-licensed.
