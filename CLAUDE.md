# CLAUDE.md — MagicMatch

Plugin OFX open source (GPL-3.0-or-later) per DaVinci Resolve Studio che calcola un ASC CDL per fare camera matching tra due immagini (source → reference).
Autore: Alessio Pallante (StrandLabs, Roma). Lingua di lavoro: **italiano** quando scrive in italiano, inglese altrimenti. Stile: diretto e conciso, niente spiegazioni di base (conosce C++, Python, build system, Resolve).

## Struttura

```
src/MagicMatch.cpp   # unico sorgente (~1170 righe), C++17, OpenFX 1.4
Makefile             # build macOS arm64: all / install / install-user / check / release / clean
install.command      # installer double-click per le release
README.md  INSTALL.md  LICENSE (GPL-3.0)
```

OFX SDK (header): `~/Desktop/openfx/include` (override: `make OFX_SDK=...`). Repo: https://github.com/AcademySoftwareFoundation/openfx

## Build e test

```bash
make && make install        # /Library/OFX/Plugins, esegue xattr -cr
make check                  # simboli esportati: solo OfxGetPlugin / OfxGetNumberOfPlugins
make release                # build/MagicMatch-1.1b-macOS-arm64.zip
```

- Resolve carica gli OFX **solo all'avvio**: dopo ogni install ricordare di riavviare Resolve (⌘Q e riapri).
- Finché il bundle non è firmato/notarizzato: `sudo xattr -cr /Library/OFX/Plugins/MagicMatch.ofx.bundle`.
- Sanity check su Linux (senza Resolve): `g++ -std=c++17 -Wall -Wextra -Wno-unused-parameter -I <openfx>/include -fPIC -shared src/MagicMatch.cpp -o /tmp/mm.so`
- Il plugin richiede **Resolve Studio** (la free non carica OFX).

## Regole di sviluppo

- Codice **completo e compilabile**, mai pseudocodice o placeholder (salvo richiesta esplicita).
- Preferire **modifiche chirurgiche** (str_replace su blocchi specifici) a riscritture complete.
- **Cross-platform fin dall'inizio**: nessuna API solo-macOS senza equivalente Windows. Codice OS-specifico sempre dentro `#ifdef _WIN32 / #else / #endif`, ben segnalato. Attualmente il sorgente non ha dipendenze macOS (CoreFoundation/IOKit/pbcopy rimossi); il port Windows richiede solo build system (VS/CMake) e test.
- Nessun framework di sistema extra nel link: solo ciò che serve a OFX.
- Debug: chiedere il messaggio d'errore esatto; distinguere bug **API OFX** / **algoritmo** / **build system**.
- Pubblicare solo da questa working copy: **non importare mai** la cronologia/i file della vecchia versione commerciale.

## Vincoli di progetto (decisioni prese)

- **Nessuna licenza/attivazione/trial/DRM**: il sistema commerciale è stato rimosso del tutto. Non reintrodurlo.
- **Niente DCTL**: valutato e scartato all'inizio dello sviluppo.
- **Mai committare segreti**: il vecchio master secret delle licenze è comparso in chat e in file passati ed è da considerare **compromesso**. `.gitignore` esclude `secrets/`, `*.hex`, `secret_obfuscated.h`. Nessun segreto in sorgenti, log o documentazione.
- Licenza **GPL-3.0-or-later** (header SPDX nel sorgente). Gli header OpenFX sono BSD, compatibili. Prima di accettare PR esterne valutare un CLA se si vuole mantenere la possibilità di rilascio commerciale separato.
- Branding: plugin ID `com.strandlabs.magicmatch`, label "MagicMatch", gruppo "Strand_Labs", https://www.strandlabs.eu/magicmatch.

## Architettura del plugin

- Un solo `OfxPlugin`, azioni: describe, describeInContext, createInstance, destroyInstance, instanceChanged (bottoni), render.
- Instance data (`struct Instance`): handle dei parametri e delle clip.
- Clip: `Source` (obbligatoria), `Reference` (opzionale), `SourceMask` e `ReferenceMask` (opzionali, `kOfxImageClipPropIsMask=1`, appaiono come Key input blu).
- Parametri: `autoMatch`, `matchWhitePoint` (checkbox, default on), `matchPower` (checkbox, default off), `grabReference`, `exportCDL`, `intensity`, `slopeR/G/B`, `offsetR/G/B`, `powerR/G/B`, `saturation`, `skinEnable`, `skinProtection`, `refStatus` (string read-only) + 7 Double nascosti (`kOfxParamPropSecret=1`) per le statistiche grabbate: `refNeutralR/G/B`, `refBlackPoint`, `refWhitePoint`, `refColorfulSat`, `refMeanLuma` (serializzati nel progetto Resolve).
- Render: CDL per pixel `out = (in*Slope + Offset)^Power`, poi saturazione globale; blend con `Intensity`; skin protection opzionale.

### Algoritmo Neutral-Point (Auto Match)

1. Statistiche per immagine (con maschera opzionale, peso = max(R,G,B), soglia 0.5):
   - **neutral gray point**: media pesata con peso `1/(1+20·sat²)`, escludendo luma >0.98 o <0.02;
   - **black/white point** = percentili 1° e 99° della luma;
   - **colorfulSat** (saturazione media dei pixel colorati).
2. **Slope** = refRatio/srcRatio per canale (ratio = canale neutral / luma neutral), clamp 0.7–1.4. Con `matchWhitePoint` attivo, moltiplicato per g = (ref.white−ref.black)/(src.white−src.black) (clamp 0.7–1.4, ignorato se range source < 0.05); slope finale clamp 0.5–2.0.
3. **Offset** uniforme = ref.black − src.black·avgSlope, clamp ±0.15.
4. **Power** 1.0; con `matchPower` attivo, p = ln(ref.meanLuma)/ln(src.meanLuma·avgSlope+offset), clamp 0.9–1.1, uguale sui 3 canali (ignorato se le medie sono fuori 0.05–0.95 o se il grab è di una versione senza `refMeanLuma`: rifare Grab Reference).
5. **Saturation** = ref.colorfulSat/src.colorfulSat, clamp 0.85–1.15.

Priorità reference in Auto Match: **Reference clip OFX → statistiche grabbate → errore** ("No reference. Grab one first.").
Si lavora al meglio in **spazio LOG** (LogC3, S-Log3, V-Log, ACEScct) con CST prima e dopo il nodo.
Lezioni apprese: mediane per sat/power → saturazione collassata (usare **medie**); il magenta da finestra in Rec.709 con maschera solo-volto non era un bug ma problema di spazio colore.

### Messaggi di status (≤35 caratteri, il campo stringa read-only di Resolve non va a capo)

`No reference grabbed.` · `No frame available.` · `Grabbed. Copy node to target clip.` · `Matched with Reference clip.` · `Matched using grabbed ref.` · `No reference. Grab one first.`

### Export CDL

XML ASC CDL v1.01 in `~/Documents/MagicMatch/CDL/MagicMatch_YYYYMMDD_HHMMSS.cdl` (su Windows: `%USERPROFILE%`).

## Roadmap / idee

- Port e test Windows (x86_64 e ARM64): CMake o progetto VS2022, `.ofx` in `C:\Program Files\Common Files\OFX\Plugins\MagicMatch.ofx.bundle\Contents\Win64\`.
- Code signing + notarizzazione macOS (poi rimuovere il passaggio `xattr -cr`).
- Possibile supporto altri host OFX (Nuke, Fusion, Flame): da verificare, in particolare gestione clip maschera e bottoni.
- Test automatici dell'algoritmo (statistiche e CDL) separati dal layer OFX.
