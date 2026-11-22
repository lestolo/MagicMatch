# MagicMatch OFX Plugin — Build System
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Alessio Pallante / StrandLabs

# Path agli header OpenFX (https://github.com/AcademySoftwareFoundation/openfx)
# Override: make OFX_SDK=/percorso/openfx/include
OFX_SDK      ?= $(HOME)/Desktop/openfx/include

PLUGIN_NAME  = MagicMatch
BUNDLE_NAME  = $(PLUGIN_NAME).ofx.bundle
BINARY       = $(PLUGIN_NAME).ofx

BUILD_DIR    = build
BUNDLE_DIR   = $(BUILD_DIR)/$(BUNDLE_NAME)
MACOS_DIR    = $(BUNDLE_DIR)/Contents/MacOS

CXX          = clang++
# -O3: ottimizzazione massima
# -fvisibility=hidden: nasconde tutti i simboli interni
# -ffunction-sections / -fdata-sections: permette allo stripper di rimuovere simboli non usati
CXXFLAGS     = -std=c++17 -O3 -arch arm64 \
               -I$(OFX_SDK) \
               -fvisibility=hidden \
               -fvisibility-inlines-hidden \
               -ffunction-sections -fdata-sections \
               -Wall -Wno-unused-parameter \
               -DMAGICMATCH_VERSION_STR='"1.0 beta"'

# -Wl,-dead_strip: rimuove codice morto
# -Wl,-x: strip local symbols
# -Wl,-S: strip debug symbols
LDFLAGS      = -arch arm64 -bundle \
               -Wl,-undefined,dynamic_lookup \
               -Wl,-exported_symbol,_OfxGetPlugin \
               -Wl,-exported_symbol,_OfxGetNumberOfPlugins \
               -Wl,-dead_strip \
               -Wl,-x -Wl,-S

SRC          = src/MagicMatch.cpp
OBJ          = $(BUILD_DIR)/MagicMatch.o

.PHONY: all clean install install-user check release

all: $(MACOS_DIR)/$(BINARY) $(BUNDLE_DIR)/Contents/Info.plist

$(OBJ): $(SRC)
	@mkdir -p $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(MACOS_DIR)/$(BINARY): $(OBJ)
	@mkdir -p $(MACOS_DIR)
	$(CXX) $(LDFLAGS) $(OBJ) -o $@
	@strip -x $(MACOS_DIR)/$(BINARY) 2>/dev/null || true
	@echo ""
	@echo ">>> Build OK: $@"

$(BUNDLE_DIR)/Contents/Info.plist:
	@mkdir -p $(BUNDLE_DIR)/Contents
	@printf '%s\n' \
	'<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0">' \
	'<dict>' \
	'  <key>CFBundleExecutable</key><string>$(BINARY)</string>' \
	'  <key>CFBundleIdentifier</key><string>com.strandlabs.magicmatch</string>' \
	'  <key>CFBundleName</key><string>$(PLUGIN_NAME)</string>' \
	'  <key>CFBundleDisplayName</key><string>MagicMatch</string>' \
	'  <key>CFBundleShortVersionString</key><string>1.0 beta</string>' \
	'  <key>CFBundleVersion</key><string>1.0.0</string>' \
	'  <key>CFBundlePackageType</key><string>BNDL</string>' \
	'  <key>CFBundleSignature</key><string>????</string>' \
	'  <key>NSHumanReadableCopyright</key><string>Copyright (C) 2026 Alessio Pallante / StrandLabs. GPL-3.0-or-later.</string>' \
	'  <key>LSMinimumSystemVersion</key><string>13.0</string>' \
	'  <key>MMProductURL</key><string>https://www.strandlabs.eu/magicmatch</string>' \
	'  <key>MMSupportEmail</key><string>support@strandlabs.eu</string>' \
	'</dict></plist>' \
	> $@
	@echo ">>> Info.plist creato"

check: all
	@echo ""
	@echo ">>> Simboli esportati (devono essere SOLO i due OfxGet*):"
	@nm -gU $(MACOS_DIR)/$(BINARY) | grep -i Ofx || echo "    (nessuno trovato!)"
	@echo ""
	@echo ">>> Conteggio simboli visibili (piu' basso = meglio):"
	@nm -gU $(MACOS_DIR)/$(BINARY) | wc -l

# Target per produrre lo zip finale da distribuire agli utenti
# Il risultato e' in build/MagicMatch-1.0b-macOS-arm64.zip
# Quando unzippato, crea una cartella "MagicMatch-1.0b/" contenente:
#   - MagicMatch.ofx.bundle    (plugin compilato)
#   - install.command          (installer double-click)
#   - INSTALL.md               (quick install guide)
#   - README.md                (full documentation)
release: all
	@rm -rf $(BUILD_DIR)/release
	@mkdir -p $(BUILD_DIR)/release/MagicMatch-1.0b
	@cp -r $(BUNDLE_DIR) $(BUILD_DIR)/release/MagicMatch-1.0b/
	@cp README.md $(BUILD_DIR)/release/MagicMatch-1.0b/ 2>/dev/null || true
	@cp INSTALL.md $(BUILD_DIR)/release/MagicMatch-1.0b/ 2>/dev/null || true
	@cp install.command $(BUILD_DIR)/release/MagicMatch-1.0b/ 2>/dev/null || true
	@chmod +x $(BUILD_DIR)/release/MagicMatch-1.0b/install.command 2>/dev/null || true
	@# Rimuove eventuali attributi di quarantena dal bundle prima del packaging
	@xattr -cr $(BUILD_DIR)/release/MagicMatch-1.0b/$(BUNDLE_NAME) 2>/dev/null || true
	@cd $(BUILD_DIR)/release && zip -r ../MagicMatch-1.0b-macOS-arm64.zip MagicMatch-1.0b -x "*.DS_Store"
	@echo ""
	@echo "════════════════════════════════════════════════════════════"
	@echo " RELEASE READY"
	@echo "════════════════════════════════════════════════════════════"
	@echo " File: $(BUILD_DIR)/MagicMatch-1.0b-macOS-arm64.zip"
	@echo " Size: $$(du -h $(BUILD_DIR)/MagicMatch-1.0b-macOS-arm64.zip | cut -f1)"
	@echo ""
	@echo " Attach this zip to a GitHub Release."
	@echo " Users will: unzip -> double-click install.command"
	@echo "════════════════════════════════════════════════════════════"

install: all
	sudo rm -rf /Library/OFX/Plugins/$(BUNDLE_NAME)
	sudo mkdir -p /Library/OFX/Plugins
	sudo cp -r $(BUNDLE_DIR) /Library/OFX/Plugins/
	sudo xattr -cr /Library/OFX/Plugins/$(BUNDLE_NAME)
	@echo ""
	@echo ">>> Installato in /Library/OFX/Plugins/$(BUNDLE_NAME)"
	@echo ">>> Riavvia DaVinci Resolve"

install-user: all
	rm -rf $(HOME)/Library/OFX/Plugins/$(BUNDLE_NAME)
	mkdir -p $(HOME)/Library/OFX/Plugins
	cp -r $(BUNDLE_DIR) $(HOME)/Library/OFX/Plugins/
	@echo ">>> Installato in ~/Library/OFX/Plugins/$(BUNDLE_NAME)"

clean:
	rm -rf $(BUILD_DIR)
	@echo ">>> Pulito"
