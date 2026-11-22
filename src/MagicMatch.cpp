/*
 * MagicMatch - OFX camera-matching plugin for DaVinci Resolve
 *
 * Camera matching CDL plugin con algoritmo Neutral-Point.
 * Analizza due immagini (source e reference) e calcola i parametri
 * ASC CDL (Slope/Offset/Power/Saturation) che avvicinano il source alla
 * reference, preservando la natura delle immagini.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Alessio Pallante / StrandLabs
 * https://www.strandlabs.eu/magicmatch
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * IMPORTANTE - COLOR SPACE:
 *   Il CDL e' color-space-agnostic per definizione: la matematica e' la
 *   stessa, ma il risultato visivo dipende dallo spazio in cui viene
 *   applicato. Per camera matching professionale si raccomanda di
 *   operare in LOG SPACE (LogC3 per Alexa, S-Log3 per Sony, V-Log per
 *   Panasonic, ACEScct per pipeline ACES).
 *
 * Formula ASC CDL per canale (R, G, B):
 *     out = (in * Slope + Offset)^Power
 *   poi Saturation globale
 *
 * Parametri esposti:
 *   - Auto Match (button)      -- calcola e scrive i valori
 *   - Grab Reference (button)  -- memorizza le statistiche del frame corrente
 *   - Export CDL (button)      -- salva .cdl in ~/Documents/MagicMatch/CDL/
 *   - Intensity                -- blend con originale
 *   - Slope R/G/B              -- WB multiplier (default 1.0)
 *   - Offset R/G/B             -- black point (default 0.0)
 *   - Power R/G/B              -- gamma (default 1.0)
 *   - Saturation               -- saturazione globale
 *   - Skin Protection + Strength
 *
 * Input clips:
 *   - Source            (RGBA, obbligatorio)
 *   - Reference         (RGBA, opzionale)
 *   - SourceMask        (maschera, opzionale, Key input)
 *   - ReferenceMask     (maschera, opzionale, Key input)
 */

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cctype>
#include <algorithm>
#include <stdint.h>
#include <time.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <direct.h>
#else
  #include <sys/stat.h>
  #include <pwd.h>
  #include <unistd.h>
#endif

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include "ofxMemory.h"

// ─────────────────────────────────────────────────────────────────────
// GLOBAL SUITES
// ─────────────────────────────────────────────────────────────────────

static OfxHost*                gHost        = NULL;
static OfxPropertySuiteV1*     gPropSuite   = NULL;
static OfxImageEffectSuiteV1*  gEffectSuite = NULL;
static OfxParameterSuiteV1*    gParamSuite  = NULL;

// ─────────────────────────────────────────────────────────────────────
// PARAMETER / CLIP NAMES
// ─────────────────────────────────────────────────────────────────────

#define PARAM_AUTO_MATCH    "autoMatch"
#define PARAM_EXPORT_CDL    "exportCDL"
#define PARAM_INTENSITY     "intensity"

#define PARAM_SLOPE_R       "slopeR"
#define PARAM_SLOPE_G       "slopeG"
#define PARAM_SLOPE_B       "slopeB"

#define PARAM_OFFSET_R      "offsetR"
#define PARAM_OFFSET_G      "offsetG"
#define PARAM_OFFSET_B      "offsetB"

#define PARAM_POWER_R       "powerR"
#define PARAM_POWER_G       "powerG"
#define PARAM_POWER_B       "powerB"

#define PARAM_SATURATION    "saturation"

// Auto Match: se attivo, lo Slope include anche il fattore luma dato da
// (ref.white - ref.black) / (src.white - src.black) (esposizione/contrasto)
#define PARAM_MATCH_WHITE   "matchWhitePoint"

// Auto Match: se attivo, stima un Power comune ai 3 canali dalla luma media
// (richiede refMeanLuma, salvato da Grab Reference)
#define PARAM_MATCH_POWER   "matchPower"

#define PARAM_CLAMP_OUT     "clampOutput"
#define PARAM_SKIN_ENABLE   "skinEnable"
#define PARAM_SKIN_PROTECT  "skinProtection"

// Grab Reference — bottone + statistiche salvate (persistenti nel progetto)
#define PARAM_GRAB_REF      "grabReference"
#define PARAM_REF_NEUTRAL_R "refNeutralR"
#define PARAM_REF_NEUTRAL_G "refNeutralG"
#define PARAM_REF_NEUTRAL_B "refNeutralB"
#define PARAM_REF_BLACK     "refBlackPoint"
#define PARAM_REF_WHITE     "refWhitePoint"
#define PARAM_REF_SAT       "refColorfulSat"
#define PARAM_REF_MEAN      "refMeanLuma"
#define PARAM_REF_STATUS    "refStatus"

#define CLIP_SOURCE         "Source"
#define CLIP_REFERENCE      "Reference"
#define CLIP_OUTPUT         "Output"
#define CLIP_SOURCE_MASK    "SourceMask"
#define CLIP_REF_MASK       "ReferenceMask"

// ─────────────────────────────────────────────────────────────────────
// COLOR MATH
// ─────────────────────────────────────────────────────────────────────

static inline float clamp01(float v) {
    if (v < 0) return 0;
    if (v > 1) return 1;
    return v;
}

// Solo il limite inferiore: i valori > 1.0 (linear, ACEScg, HDR) restano intatti
static inline float floor0(float v) { return v < 0 ? 0 : v; }

static inline float luma(float r, float g, float b) {
    return 0.2126f*r + 0.7152f*g + 0.0722f*b;
}

static float skinWeight(float r, float g, float b) {
    float mx = std::max({r,g,b}), mn = std::min({r,g,b});
    float v = mx;
    if (mx < 0.001f) return 0;
    float s = (mx - mn) / mx;
    float h = 0;
    float d = mx - mn;
    if (d > 0.001f) {
        if      (mx == r) h = (g-b)/d + (g < b ? 6 : 0);
        else if (mx == g) h = (b-r)/d + 2;
        else              h = (r-g)/d + 4;
        h /= 6.0f;
    }
    if (!((h < 0.09f || h > 0.92f) && s > 0.10f && s < 0.85f && v > 0.20f))
        return 0;
    float hw = (h < 0.09f) ? (1.0f - h/0.09f*0.5f) : 0.5f;
    float sw = 1.0f - fabsf(s - 0.45f)/0.45f;
    return std::max(0.0f, std::min(1.0f, hw*sw));
}

// ─────────────────────────────────────────────────────────────────────
// STATISTICS — Neutral-point analysis per camera matching
// ─────────────────────────────────────────────────────────────────────

struct Stats {
    // Neutral gray point: media RGB dei pixel "vicini al grigio",
    // pesata inversamente alla saturation. Rappresenta la dominante.
    float neutralR, neutralG, neutralB;

    // Punti di nero e bianco della luminanza (esclusi outlier clipped)
    float blackPoint;      // 1° percentile luma
    float whitePoint;      // 99° percentile luma (excluding clipping)

    // Saturation media dei pixel effettivamente colorati (non gli sfondi grigi)
    float colorfulSat;

    // Info diagnostiche
    float meanLuma;        // luminanza media (per esposizione)
    long  sampleCount;     // quanti pixel sono stati analizzati

    // 0 = ok, 1 = memoria insufficiente, 2 = nessun pixel utilizzabile
    // (maschera vuota, frame tutto nero/clippato): i valori sono solo default
    int   error;
};

// Percentile con interpolazione lineare su array ordinato
static float percentile(float* sorted, long n, float p) {
    if (n <= 0) return 0;
    float idx = p * (n - 1);
    long i = (long)idx;
    if (i < 0) i = 0;
    if (i >= n-1) return sorted[n-1];
    float frac = idx - i;
    return sorted[i] * (1.0f - frac) + sorted[i+1] * frac;
}

static int cmpFloat(const void* a, const void* b) {
    float fa = *(const float*)a, fb = *(const float*)b;
    if (fa < fb) return -1;
    if (fa > fb) return 1;
    return 0;
}

// Legge il peso dal matte (prende il max dei canali come luminanza)
// Se maskData e' NULL, ritorna sempre 1.0 (nessuna maschera)
static inline float maskWeight(const float* maskData, int maskRB,
                                const OfxRectI* maskBounds, int maskNC,
                                int absX, int absY) {
    if (!maskData) return 1.0f;
    int mx = absX - maskBounds->x1;
    int my = absY - maskBounds->y1;
    int mw = maskBounds->x2 - maskBounds->x1;
    int mh = maskBounds->y2 - maskBounds->y1;
    if (mx < 0 || mx >= mw || my < 0 || my >= mh) return 0.0f;
    const float* row = (const float*)((const uint8_t*)maskData + my*maskRB);
    // Il matte arriva come RGBA (uso il max dei canali per robustezza)
    // oppure come Alpha puro (1 canale)
    if (maskNC == 1) return clamp01(row[mx]);
    float r = row[mx*4+0];
    float g = row[mx*4+1];
    float b = row[mx*4+2];
    float w = std::max({r, g, b});
    return clamp01(w);
}

// Verifica che l'immagine sia float con i canali attesi.
// Ritorna il numero di canali (4 = RGBA, 1 = Alpha, solo se allowAlpha) o 0 se non supportata.
static int imageChannels(OfxPropertySetHandle img, bool allowAlpha) {
    char* depth = NULL;
    char* comps = NULL;
    if (gPropSuite->propGetString(img, kOfxImageEffectPropPixelDepth, 0, &depth) != kOfxStatOK || !depth) return 0;
    if (gPropSuite->propGetString(img, kOfxImageEffectPropComponents, 0, &comps) != kOfxStatOK || !comps) return 0;
    if (strcmp(depth, kOfxBitDepthFloat) != 0) return 0;
    if (strcmp(comps, kOfxImageComponentRGBA) == 0) return 4;
    if (allowAlpha && strcmp(comps, kOfxImageComponentAlpha) == 0) return 1;
    return 0;
}

struct ImgView {
    float*   data;
    int      rb;
    OfxRectI bounds;
    int      nc;
};

// Legge puntatore, rowBytes e bounds; false se formato non supportato o dati nulli
static bool getView(OfxPropertySetHandle img, bool allowAlpha, ImgView& v) {
    v = ImgView{};
    v.nc = imageChannels(img, allowAlpha);
    if (v.nc == 0) return false;
    void* d = NULL;
    gPropSuite->propGetPointer(img, kOfxImagePropData, 0, &d);
    gPropSuite->propGetInt(img, kOfxImagePropRowBytes, 0, &v.rb);
    gPropSuite->propGetIntN(img, kOfxImagePropBounds, 4, (int*)&v.bounds);
    v.data = (float*)d;
    return d != NULL;
}

// Statistiche neutral-point per camera matching.
// Tre passate sui pixel:
//   1. Media pesata per neutralita' (meno satura = piu' peso) -> neutral gray point
//   2. Percentili luma 1/99 -> black point, white point
//   3. Media saturation dei pixel colorati -> colorfulness
static Stats computeStats(const float* data, int rowBytes,
                           const OfxRectI* bounds,
                           const float* maskData, int maskRB,
                           const OfxRectI* maskBounds, int maskNC) {
    Stats s{};
    int w = bounds->x2 - bounds->x1;
    int h = bounds->y2 - bounds->y1;
    long total = (long)w * h;

    int stride = 1;
    if (total > 100000) stride = (int)sqrtf(total / 100000.0f);
    if (stride < 1) stride = 1;

    long capacity = ((long)(w/stride + 1)) * ((long)(h/stride + 1));
    float* luArr = (float*)malloc(capacity * sizeof(float));

    if (!luArr) {
        s.error = 1;
        s.neutralR = s.neutralG = s.neutralB = 0.5f;
        s.blackPoint = 0.05f; s.whitePoint = 0.95f;
        s.colorfulSat = 0.3f; s.meanLuma = 0.5f;
        s.sampleCount = 0;
        return s;
    }

    bool useMask = (maskData != NULL && maskBounds != NULL);

    // Accumulatori pesati
    double wSumR = 0, wSumG = 0, wSumB = 0;   // numeratore neutral (pesato)
    double wSumTotal = 0;                      // denominatore pesi neutral
    double sumSatColored = 0;                  // numeratore saturation
    double sumWeightColored = 0;               // denominatore saturation
    double sumLuma = 0;
    long n = 0;
    long luCount = 0;

    for (int y = 0; y < h; y += stride) {
        int absY = y + bounds->y1;
        const float* row = (const float*)((const uint8_t*)data + y*rowBytes);
        for (int x = 0; x < w; x += stride) {
            int absX = x + bounds->x1;

            float wgt = useMask ? maskWeight(maskData, maskRB, maskBounds, maskNC, absX, absY) : 1.0f;
            if (wgt < 0.5f) continue;

            float r = floor0(row[x*4+0]);
            float g = floor0(row[x*4+1]);
            float b = floor0(row[x*4+2]);
            float mx = std::max({r,g,b});
            float mn = std::min({r,g,b});
            float lu = luma(r, g, b);

            // Salta pixel clipped (sopra 0.98) e very dark (sotto 0.02):
            // entrambi inquinano le statistiche di bilanciamento.
            bool excludeForNeutral = (lu > 0.98f || lu < 0.02f);

            // Peso per "neutralita'": inverso della saturation.
            // Pixel con saturation zero = peso 1.0
            // Pixel molto saturi = peso molto basso
            // Formula: 1 / (1 + k * saturation^2) con k=20
            float sat = (mx > 0.001f) ? (mx - mn) / mx : 0;
            if (!excludeForNeutral) {
                float neutralW = 1.0f / (1.0f + 20.0f * sat * sat);
                wSumR += r * neutralW;
                wSumG += g * neutralW;
                wSumB += b * neutralW;
                wSumTotal += neutralW;

                // Saturation media pesata per i pixel colorati (inverso)
                // Pixel molto saturi contribuiscono di piu' qui
                float colorW = sat;  // solo i colorati contribuiscono significativamente
                sumSatColored += sat * colorW;
                sumWeightColored += colorW;
            }

            // Luminanza per percentili (tutti i pixel, compresi clipped — saranno
            // esclusi dai percentili 1 e 99 naturalmente se sono outlier)
            luArr[luCount++] = lu;
            sumLuma += lu;
            n++;

            if (luCount >= capacity) break;
        }
        if (luCount >= capacity) break;
    }

    if (n == 0 || wSumTotal < 0.001) {
        free(luArr);
        s.error = 2;
        s.neutralR = s.neutralG = s.neutralB = 0.5f;
        s.blackPoint = 0.05f; s.whitePoint = 0.95f;
        s.colorfulSat = 0.3f; s.meanLuma = 0.5f;
        s.sampleCount = n;
        return s;
    }

    // Neutral gray point — qui si rivela la dominante
    s.neutralR = (float)(wSumR / wSumTotal);
    s.neutralG = (float)(wSumG / wSumTotal);
    s.neutralB = (float)(wSumB / wSumTotal);

    // Saturation dei pixel colorati
    s.colorfulSat = (sumWeightColored > 0.001)
                    ? (float)(sumSatColored / sumWeightColored)
                    : 0.3f;

    s.meanLuma = (float)(sumLuma / n);

    // Percentili di luminanza per black/white point
    qsort(luArr, luCount, sizeof(float), cmpFloat);
    s.blackPoint = percentile(luArr, luCount, 0.01f);
    s.whitePoint = percentile(luArr, luCount, 0.99f);

    // Clamp per sicurezza
    if (s.blackPoint < 0.0f) s.blackPoint = 0.0f;
    if (s.whitePoint <= s.blackPoint + 0.01f) s.whitePoint = s.blackPoint + 0.01f;

    s.sampleCount = n;
    free(luArr);
    return s;
}

// ─────────────────────────────────────────────────────────────────────
// CDL CALCULATION — Neutral-point camera matching
//
// Approccio ispirato a Colourlab AI / Auto Balance:
//   1. Da ogni immagine si estrae un "punto neutro" e un black/white point
//   2. Il CDL porta source attraverso "neutro" e poi verso reference
//
// Matematica:
//   - Slope per canale = WB multiplier per neutralizzare la dominante del source
//     e applicare la dominante del reference
//   - Offset per canale = allineamento del black point (via luminanza)
//   - Saturation = ratio delle saturation colorful
//   - Power = 1.0 di default (opzionale: stima dalla luma media, 0.9-1.1)
// ─────────────────────────────────────────────────────────────────────

static Stats statsFrom(const ImgView& img, const ImgView* mask) {
    return computeStats(img.data, img.rb, &img.bounds,
                        mask ? mask->data : NULL, mask ? mask->rb : 0,
                        mask ? &mask->bounds : NULL, mask ? mask->nc : 4);
}

// Messaggio di status (<= 35 caratteri) per una Stats con error != 0
// who: 0 = source, 1 = reference, 2 = grab
static const char* statsError(const Stats& s, int who) {
    if (s.error == 1) return "Analysis failed: out of memory.";
    static const char* const m[] = {"Source: no usable pixels.",
                                    "Reference: no usable pixels.",
                                    "Grab failed: no usable pixels."};
    return m[who];
}

struct CDL {
    float slopeR,  slopeG,  slopeB;
    float offsetR, offsetG, offsetB;
    float powerR,  powerG,  powerB;
    float saturation;
};

static CDL computeCDL(const Stats& src, const Stats& ref, bool matchWhitePoint, bool matchPower) {
    CDL c{};

    // ── Step 1: White Balance ────────────────────────────────────────
    // Per ogni canale, calcoliamo due multiplier:
    //   a) Quello che porta il source a neutro (diviso per il suo punto neutro)
    //   b) Quello che applica la dominante del reference (moltiplicato per il suo punto neutro)
    //
    // Combinato: slope[c] = ref.neutral[c] / src.neutral[c]
    //
    // Il gray neutro della reference diventa il gray di destinazione del source.

    // Luma del neutral point (usata per normalizzare — vogliamo che il WB non
    // sposti l'esposizione media)
    float srcNeutralLuma = luma(src.neutralR, src.neutralG, src.neutralB);
    float refNeutralLuma = luma(ref.neutralR, ref.neutralG, ref.neutralB);
    if (srcNeutralLuma < 0.001f) srcNeutralLuma = 0.001f;
    if (refNeutralLuma < 0.001f) refNeutralLuma = 0.001f;

    // Rapporti canale/luma = "quanto questo canale devia dal grigio"
    float srcRatioR = src.neutralR / srcNeutralLuma;
    float srcRatioG = src.neutralG / srcNeutralLuma;
    float srcRatioB = src.neutralB / srcNeutralLuma;
    float refRatioR = ref.neutralR / refNeutralLuma;
    float refRatioG = ref.neutralG / refNeutralLuma;
    float refRatioB = ref.neutralB / refNeutralLuma;

    if (srcRatioR < 0.1f) srcRatioR = 0.1f;
    if (srcRatioG < 0.1f) srcRatioG = 0.1f;
    if (srcRatioB < 0.1f) srcRatioB = 0.1f;

    // Slope per canale: neutralizza la dominante del source, applica quella del reference
    c.slopeR = refRatioR / srcRatioR;
    c.slopeG = refRatioG / srcRatioG;
    c.slopeB = refRatioB / srcRatioB;

    // Clamp: per camera matching i multiplier devono essere moderati
    // (se escono di molto da [0.7 - 1.4], probabilmente c'e' un problema di esposizione)
    c.slopeR = std::max(0.7f, std::min(1.4f, c.slopeR));
    c.slopeG = std::max(0.7f, std::min(1.4f, c.slopeG));
    c.slopeB = std::max(0.7f, std::min(1.4f, c.slopeB));

    // ── Step 1b: White point (esposizione/contrasto) — opzionale ─────
    // Fit a due punti sulla luma: dopo slope e offset, il range
    // [src.black, src.white] deve coincidere con [ref.black, ref.white].
    // Fattore g = (ref.white - ref.black) / (src.white - src.black),
    // clamp 0.7-1.4 come per il WB; se il range del source e' troppo
    // piccolo (< 0.05) il fattore non e' affidabile e viene ignorato.
    // Il risultato finale dello slope e' limitato a 0.5-2.0.
    if (matchWhitePoint) {
        float srcRange = src.whitePoint - src.blackPoint;
        float refRange = ref.whitePoint - ref.blackPoint;
        if (srcRange >= 0.05f && refRange > 0.0f) {
            float g = std::max(0.7f, std::min(1.4f, refRange / srcRange));
            c.slopeR = std::max(0.5f, std::min(2.0f, c.slopeR * g));
            c.slopeG = std::max(0.5f, std::min(2.0f, c.slopeG * g));
            c.slopeB = std::max(0.5f, std::min(2.0f, c.slopeB * g));
        }
    }

    // ── Step 2: Luminance alignment (exposure + black point) ─────────
    // Dopo lo slope, vogliamo anche che:
    //   - Il black point del source coincida con quello del reference
    //   - La luminanza media resti coerente
    //
    // Calcoliamo l'offset di luminanza e lo applichiamo uniformemente ai 3 canali.
    // Questo sposta il black point senza toccare la dominante (già corretta dallo slope).

    // Black point del source dopo slope (assumiamo slope uniforme per semplicita')
    float avgSlope = (c.slopeR + c.slopeG + c.slopeB) / 3.0f;
    float srcBlackAfterSlope = src.blackPoint * avgSlope;

    // Offset uniforme per portare il black point allineato
    float blackOffset = ref.blackPoint - srcBlackAfterSlope;
    blackOffset = std::max(-0.15f, std::min(0.15f, blackOffset));

    c.offsetR = blackOffset;
    c.offsetG = blackOffset;
    c.offsetB = blackOffset;

    // ── Step 3: Power (gamma) ────────────────────────────────────────
    // Per camera matching, lasciamo Power=1.0 di default.
    // Il power e' una curva non lineare e causa piu' problemi che soluzioni
    // quando le immagini hanno composizioni diverse.
    // L'utente puo' sempre toccarlo manualmente dopo il match.
    c.powerR = 1.0f;
    c.powerG = 1.0f;
    c.powerB = 1.0f;

    // Opzionale (checkbox Match Midtones): Power comune ai 3 canali che porta
    // la luma media del source (dopo slope e offset) a quella della reference:
    //   mSrc^p = mRef  ->  p = ln(mRef) / ln(mSrc)
    // Clamp stretto 0.9-1.1. Ignorato se le medie non sono affidabili
    // (fuori da 0.05-0.95, oppure ref.meanLuma assente in un grab vecchio).
    if (matchPower) {
        float mSrc = src.meanLuma * avgSlope + blackOffset;
        float mRef = ref.meanLuma;
        if (mSrc > 0.05f && mSrc < 0.95f && mRef > 0.05f && mRef < 0.95f) {
            float p = logf(mRef) / logf(mSrc);
            p = std::max(0.9f, std::min(1.1f, p));
            c.powerR = c.powerG = c.powerB = p;
        }
    }

    // ── Step 4: Saturation ───────────────────────────────────────────
    // Rapporto tra saturation media dei pixel colorati di reference e source.
    // Piu' robusto della media su tutti i pixel perche' ignora i grigi/sfondi.
    // Clamp stretto (0.85-1.15): per camera matching non servono mai
    // variazioni piu' ampie, ed evita over-saturation quando si analizza
    // senza maschera.
    if (src.colorfulSat > 0.01f) {
        c.saturation = ref.colorfulSat / src.colorfulSat;
        c.saturation = std::max(0.85f, std::min(1.15f, c.saturation));
    } else {
        c.saturation = 1.0f;
    }

    return c;
}

// ─────────────────────────────────────────────────────────────────────
// ASC CDL FORMULA (applicata per pixel)
// ─────────────────────────────────────────────────────────────────────

static inline float applyCDLChannel(float in, float slope, float offset, float power) {
    // (in * slope + offset) ^ power
    float v = in * slope + offset;
    if (v <= 0.0f) return 0.0f;
    return powf(v, power);
}

static inline void applySaturation(float& r, float& g, float& b, float sat) {
    float lu = luma(r, g, b);
    r = lu + (r - lu) * sat;
    g = lu + (g - lu) * sat;
    b = lu + (b - lu) * sat;
}

// ─────────────────────────────────────────────────────────────────────
// CDL FILE EXPORT (XML ASC CDL v1.01)
// ─────────────────────────────────────────────────────────────────────

#ifdef _WIN32
  #define MM_MKDIR(p) _mkdir(p)
#else
  #define MM_MKDIR(p) mkdir((p), 0755)
#endif

static void ensureDirectory(const char* path) {
    // Crea ricorsivamente (errori ignorati: esiste gia' o verra' rilevato in fopen)
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp)-1);
    tmp[sizeof(tmp)-1] = 0;
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            MM_MKDIR(tmp);
            *p = c;
        }
    }
    MM_MKDIR(tmp);
}

static bool exportCDLFile(const CDL& cdl, char* outPath, size_t outPathSize) {
    // Costruisci path ~/Documents/MagicMatch/CDL/
#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
#else
    const char* home = getenv("HOME");
    if (!home || !*home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
#endif
    if (!home || !*home) return false;

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/Documents/MagicMatch/CDL", home);
    ensureDirectory(dir);

    // Nome file con timestamp
    time_t now = time(NULL);
    struct tm* tm_now = localtime(&now);
    char filename[256];
    snprintf(filename, sizeof(filename),
             "MagicMatch_%04d%02d%02d_%02d%02d%02d.cdl",
             tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday,
             tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec);

    snprintf(outPath, outPathSize, "%s/%s", dir, filename);

    FILE* f = fopen(outPath, "w");
    if (!f) return false;

    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<ColorDecisionList xmlns=\"urn:ASC:CDL:v1.01\">\n"
        "  <ColorDecision>\n"
        "    <ColorCorrection id=\"MagicMatch_%04d%02d%02d_%02d%02d%02d\">\n"
        "      <SOPNode>\n"
        "        <Slope>%.6f %.6f %.6f</Slope>\n"
        "        <Offset>%.6f %.6f %.6f</Offset>\n"
        "        <Power>%.6f %.6f %.6f</Power>\n"
        "      </SOPNode>\n"
        "      <SatNode>\n"
        "        <Saturation>%.6f</Saturation>\n"
        "      </SatNode>\n"
        "    </ColorCorrection>\n"
        "  </ColorDecision>\n"
        "</ColorDecisionList>\n",
        tm_now->tm_year + 1900, tm_now->tm_mon + 1, tm_now->tm_mday,
        tm_now->tm_hour, tm_now->tm_min, tm_now->tm_sec,
        cdl.slopeR,  cdl.slopeG,  cdl.slopeB,
        cdl.offsetR, cdl.offsetG, cdl.offsetB,
        cdl.powerR,  cdl.powerG,  cdl.powerB,
        cdl.saturation);

    fclose(f);
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// INSTANCE DATA
// ─────────────────────────────────────────────────────────────────────

struct Instance {
    OfxParamHandle pAutoMatch;
    OfxParamHandle pExportCDL;
    OfxParamHandle pIntensity;
    OfxParamHandle pSlopeR,  pSlopeG,  pSlopeB;
    OfxParamHandle pOffsetR, pOffsetG, pOffsetB;
    OfxParamHandle pPowerR,  pPowerG,  pPowerB;
    OfxParamHandle pSaturation;
    OfxParamHandle pMatchWhite;
    OfxParamHandle pMatchPower;
    OfxParamHandle pRefMean;
    OfxParamHandle pClampOut;
    OfxParamHandle pSkinEnable;
    OfxParamHandle pSkinProtect;
    // Grab Reference
    OfxParamHandle pGrabRef;
    OfxParamHandle pRefNeutralR, pRefNeutralG, pRefNeutralB;
    OfxParamHandle pRefBlack, pRefWhite, pRefSat;
    OfxParamHandle pRefStatus;
    OfxImageClipHandle cSource;
    OfxImageClipHandle cReference;
    OfxImageClipHandle cOutput;
    OfxImageClipHandle cSourceMask;
    OfxImageClipHandle cRefMask;
};

// ─────────────────────────────────────────────────────────────────────
// GRAB REFERENCE — campiona le statistiche del frame corrente e le salva
// come parametri persistenti nel progetto Resolve.
// L'utente naviga al frame della reference (es. CAM A), clicca il bottone,
// poi torna alla clip da matchare (es. CAM B) e clicca Auto Match.
// ─────────────────────────────────────────────────────────────────────

static void runGrabReference(Instance* inst, double time) {
    OfxPropertySetHandle srcH = NULL, srcMaskH = NULL;
    gEffectSuite->clipGetImage(inst->cSource, time, NULL, &srcH);
    if (!srcH) {
        gParamSuite->paramSetValue(inst->pRefStatus, "No frame available.");
        return;
    }
    // Maschera opzionale sul source (primo key input)
    gEffectSuite->clipGetImage(inst->cSourceMask, time, NULL, &srcMaskH);

    auto fail = [&](const char* msg) {
        gParamSuite->paramSetValue(inst->pRefStatus, msg);
        gEffectSuite->clipReleaseImage(srcH);
        if (srcMaskH) gEffectSuite->clipReleaseImage(srcMaskH);
    };

    ImgView sv, mv{};
    if (!getView(srcH, false, sv))                  { fail("Unsupported image format."); return; }
    if (srcMaskH && !getView(srcMaskH, true, mv))   { fail("Unsupported mask format.");  return; }

    Stats ref = statsFrom(sv, srcMaskH ? &mv : NULL);
    if (ref.error) { fail(statsError(ref, 2)); return; }  // i parametri salvati non vengono toccati

    // Salva le statistiche come parametri — Resolve le serializza nel progetto
    gParamSuite->paramSetValue(inst->pRefNeutralR, (double)ref.neutralR);
    gParamSuite->paramSetValue(inst->pRefNeutralG, (double)ref.neutralG);
    gParamSuite->paramSetValue(inst->pRefNeutralB, (double)ref.neutralB);
    gParamSuite->paramSetValue(inst->pRefBlack,    (double)ref.blackPoint);
    gParamSuite->paramSetValue(inst->pRefWhite,    (double)ref.whitePoint);
    gParamSuite->paramSetValue(inst->pRefSat,      (double)ref.colorfulSat);
    gParamSuite->paramSetValue(inst->pRefMean,     (double)ref.meanLuma);

    gParamSuite->paramSetValue(inst->pRefStatus, "Grabbed. Copy node to target clip.");

    gEffectSuite->clipReleaseImage(srcH);
    if (srcMaskH) gEffectSuite->clipReleaseImage(srcMaskH);
}

// ─────────────────────────────────────────────────────────────────────
// AUTO MATCH — calcola e scrive i parametri CDL
//
// Priorita' per la reference:
//   1. Reference clip OFX collegato (triangolino verde) → usa quello
//   2. Statistiche salvate con Grab Reference → usa quelle
//   3. Nessuna reference disponibile → errore
// ─────────────────────────────────────────────────────────────────────

static void runAutoMatch(Instance* inst, double time) {
    OfxPropertySetHandle srcH = NULL, refH = NULL;
    OfxPropertySetHandle srcMaskH = NULL, refMaskH = NULL;

    gEffectSuite->clipGetImage(inst->cSource, time, NULL, &srcH);
    if (!srcH) return;

    // Maschere opzionali
    gEffectSuite->clipGetImage(inst->cSourceMask, time, NULL, &srcMaskH);
    gEffectSuite->clipGetImage(inst->cRefMask,    time, NULL, &refMaskH);

    // Rilascia tutte le immagini ottenute (anche la maschera reference
    // quando non c'e' un Reference clip)
    auto cleanup = [&]() {
        if (srcH)     gEffectSuite->clipReleaseImage(srcH);
        if (refH)     gEffectSuite->clipReleaseImage(refH);
        if (srcMaskH) gEffectSuite->clipReleaseImage(srcMaskH);
        if (refMaskH) gEffectSuite->clipReleaseImage(refMaskH);
    };
    auto fail = [&](const char* msg) {
        gParamSuite->paramSetValue(inst->pRefStatus, msg);
        cleanup();
    };

    ImgView sv, smv{};
    if (!getView(srcH, false, sv))                    { fail("Unsupported image format."); return; }
    if (srcMaskH && !getView(srcMaskH, true, smv))    { fail("Unsupported mask format.");  return; }

    Stats src = statsFrom(sv, srcMaskH ? &smv : NULL);
    if (src.error) { fail(statsError(src, 0)); return; }

    // ── Ottieni le statistiche della reference ────────────────────────
    Stats ref{};
    bool  refFromClip = false;

    // Priorita' 1: Reference clip OFX collegato
    gEffectSuite->clipGetImage(inst->cReference, time, NULL, &refH);
    if (refH) {
        ImgView rv, rmv{};
        if (!getView(refH, false, rv))                  { fail("Unsupported image format."); return; }
        if (refMaskH && !getView(refMaskH, true, rmv))  { fail("Unsupported mask format.");  return; }

        ref = statsFrom(rv, refMaskH ? &rmv : NULL);
        if (ref.error) { fail(statsError(ref, 1)); return; }
        refFromClip = true;
        gParamSuite->paramSetValue(inst->pRefStatus, "Matched with Reference clip.");
    }

    // Priorita' 2: Statistiche salvate con Grab Reference
    if (!refFromClip) {
        double nR, nG, nB, bp, wp, cs, ml = 0.0;
        gParamSuite->paramGetValueAtTime(inst->pRefNeutralR, time, &nR);
        gParamSuite->paramGetValueAtTime(inst->pRefNeutralG, time, &nG);
        gParamSuite->paramGetValueAtTime(inst->pRefNeutralB, time, &nB);
        gParamSuite->paramGetValueAtTime(inst->pRefBlack,    time, &bp);
        gParamSuite->paramGetValueAtTime(inst->pRefWhite,    time, &wp);
        gParamSuite->paramGetValueAtTime(inst->pRefSat,      time, &cs);
        gParamSuite->paramGetValueAtTime(inst->pRefMean,     time, &ml);

        // Se nessun Grab e' stato fatto, tutti i valori sono 0 — non procedere
        if (nR < 0.001f && nG < 0.001f && nB < 0.001f) {
            fail("No reference. Grab one first.");
            return;
        }

        ref.neutralR    = (float)nR;
        ref.neutralG    = (float)nG;
        ref.neutralB    = (float)nB;
        ref.blackPoint  = (float)bp;
        ref.whitePoint  = (float)wp;
        ref.colorfulSat = (float)cs;
        ref.meanLuma    = (float)ml; // 0 = grab di versione precedente: Power ignorato
        ref.sampleCount = 1; // marcato come "da parametri salvati"
    }

    int matchWhite = 1;
    gParamSuite->paramGetValueAtTime(inst->pMatchWhite, time, &matchWhite);

    int matchPower = 0;
    gParamSuite->paramGetValueAtTime(inst->pMatchPower, time, &matchPower);

    CDL cdl = computeCDL(src, ref, matchWhite != 0, matchPower != 0);

    gParamSuite->paramSetValue(inst->pSlopeR,     (double)cdl.slopeR);
    gParamSuite->paramSetValue(inst->pSlopeG,     (double)cdl.slopeG);
    gParamSuite->paramSetValue(inst->pSlopeB,     (double)cdl.slopeB);
    gParamSuite->paramSetValue(inst->pOffsetR,    (double)cdl.offsetR);
    gParamSuite->paramSetValue(inst->pOffsetG,    (double)cdl.offsetG);
    gParamSuite->paramSetValue(inst->pOffsetB,    (double)cdl.offsetB);
    gParamSuite->paramSetValue(inst->pPowerR,     (double)cdl.powerR);
    gParamSuite->paramSetValue(inst->pPowerG,     (double)cdl.powerG);
    gParamSuite->paramSetValue(inst->pPowerB,     (double)cdl.powerB);
    gParamSuite->paramSetValue(inst->pSaturation, (double)cdl.saturation);
    gParamSuite->paramSetValue(inst->pIntensity,  1.0);

    // Aggiorna il ref status per indicare quale source e' stata usata
    if (!refFromClip) {
        gParamSuite->paramSetValue(inst->pRefStatus, "Matched using grabbed ref.");
    }

    cleanup();
}

// ─────────────────────────────────────────────────────────────────────
// EXPORT CDL (legge i parametri correnti e scrive su file)
// ─────────────────────────────────────────────────────────────────────

static void runExportCDL(Instance* inst, double time) {
    CDL cdl{};
    double v;

    gParamSuite->paramGetValueAtTime(inst->pSlopeR,  time, &v); cdl.slopeR  = (float)v;
    gParamSuite->paramGetValueAtTime(inst->pSlopeG,  time, &v); cdl.slopeG  = (float)v;
    gParamSuite->paramGetValueAtTime(inst->pSlopeB,  time, &v); cdl.slopeB  = (float)v;
    gParamSuite->paramGetValueAtTime(inst->pOffsetR, time, &v); cdl.offsetR = (float)v;
    gParamSuite->paramGetValueAtTime(inst->pOffsetG, time, &v); cdl.offsetG = (float)v;
    gParamSuite->paramGetValueAtTime(inst->pOffsetB, time, &v); cdl.offsetB = (float)v;
    gParamSuite->paramGetValueAtTime(inst->pPowerR,  time, &v); cdl.powerR  = (float)v;
    gParamSuite->paramGetValueAtTime(inst->pPowerG,  time, &v); cdl.powerG  = (float)v;
    gParamSuite->paramGetValueAtTime(inst->pPowerB,  time, &v); cdl.powerB  = (float)v;
    gParamSuite->paramGetValueAtTime(inst->pSaturation, time, &v); cdl.saturation = (float)v;

    char outPath[1024];
    exportCDLFile(cdl, outPath, sizeof(outPath));
}

// ─────────────────────────────────────────────────────────────────────
// OFX ACTIONS
// ─────────────────────────────────────────────────────────────────────

static OfxStatus onLoad() {
    gPropSuite   = (OfxPropertySuiteV1*)   gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    gEffectSuite = (OfxImageEffectSuiteV1*) gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    gParamSuite  = (OfxParameterSuiteV1*)   gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
    if (!gPropSuite || !gEffectSuite || !gParamSuite) return kOfxStatErrMissingHostFeature;
    return kOfxStatOK;
}

static OfxStatus onDescribe(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props;
    gEffectSuite->getPropertySet(effect, &props);

    gPropSuite->propSetString(props, kOfxPropLabel,      0, "MagicMatch");
    gPropSuite->propSetString(props, kOfxPropShortLabel, 0, "MagicMatch");
    gPropSuite->propSetString(props, kOfxPropLongLabel,  0, "MagicMatch");
    gPropSuite->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, "Color");

    // Icona: file in <bundle>/Contents/Resources (copiati dal Makefile).
    // [0] = SVG, [1] = PNG: l'host usa il primo formato che supporta.
    gPropSuite->propSetString(props, kOfxPropIcon, 0, "com.strandlabs.magicmatch.svg");
    gPropSuite->propSetString(props, kOfxPropIcon, 1, "com.strandlabs.magicmatch.png");

    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextGeneral);
    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextFilter);
    gPropSuite->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gPropSuite->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    gPropSuite->propSetInt(props, kOfxImageEffectPluginPropSingleInstance, 0, 0);
    gPropSuite->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 1);
    gPropSuite->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, 0);
    gPropSuite->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);

    return kOfxStatOK;
}

static void defineSlider(OfxParamSetHandle paramSet, const char* name,
                          const char* label, double def,
                          double mn, double mx, double dmn, double dmx) {
    OfxPropertySetHandle pp;
    gParamSuite->paramDefine(paramSet, kOfxParamTypeDouble, name, &pp);
    gPropSuite->propSetString(pp, kOfxPropLabel,           0, label);
    gPropSuite->propSetDouble(pp, kOfxParamPropDefault,    0, def);
    gPropSuite->propSetDouble(pp, kOfxParamPropMin,        0, mn);
    gPropSuite->propSetDouble(pp, kOfxParamPropMax,        0, mx);
    gPropSuite->propSetDouble(pp, kOfxParamPropDisplayMin, 0, dmn);
    gPropSuite->propSetDouble(pp, kOfxParamPropDisplayMax, 0, dmx);
}

static OfxStatus onDescribeInContext(OfxImageEffectHandle effect, OfxPropertySetHandle) {
    OfxPropertySetHandle clipProps;

    gEffectSuite->clipDefine(effect, CLIP_SOURCE, &clipProps);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);

    gEffectSuite->clipDefine(effect, CLIP_REFERENCE, &clipProps);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gPropSuite->propSetInt   (clipProps, kOfxImageClipPropOptional, 0, 1);

    gEffectSuite->clipDefine(effect, CLIP_OUTPUT, &clipProps);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);

    // ── Source Mask (opzionale, key input) ────────────────────────
    gEffectSuite->clipDefine(effect, CLIP_SOURCE_MASK, &clipProps);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentAlpha);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gPropSuite->propSetInt(clipProps, kOfxImageClipPropOptional, 0, 1);
    gPropSuite->propSetInt(clipProps, kOfxImageClipPropIsMask, 0, 1);
    gPropSuite->propSetString(clipProps, kOfxPropLabel, 0, "Source Mask");

    // ── Reference Mask (opzionale, key input) ─────────────────────
    gEffectSuite->clipDefine(effect, CLIP_REF_MASK, &clipProps);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedComponents, 1, kOfxImageComponentAlpha);
    gPropSuite->propSetString(clipProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);
    gPropSuite->propSetInt(clipProps, kOfxImageClipPropOptional, 0, 1);
    gPropSuite->propSetInt(clipProps, kOfxImageClipPropIsMask, 0, 1);
    gPropSuite->propSetString(clipProps, kOfxPropLabel, 0, "Reference Mask");

    OfxParamSetHandle paramSet;
    gEffectSuite->getParamSet(effect, &paramSet);
    OfxPropertySetHandle pp;

    // ── Match White Point (opzione di Auto Match) ─────────────
    gParamSuite->paramDefine(paramSet, kOfxParamTypeBoolean, PARAM_MATCH_WHITE, &pp);
    gPropSuite->propSetString(pp, kOfxPropLabel, 0, "Match White Point");
    gPropSuite->propSetInt(pp, kOfxParamPropDefault, 0, 1);
    gPropSuite->propSetString(pp, kOfxParamPropHint, 0,
        "Auto Match also aligns exposure/contrast using the white and black "
        "points (1st/99th luma percentile). Turn off to match only white "
        "balance, black point and saturation. Applies at the next Auto Match.");

    // ── Match Midtones / Power (opzione di Auto Match) ────────
    gParamSuite->paramDefine(paramSet, kOfxParamTypeBoolean, PARAM_MATCH_POWER, &pp);
    gPropSuite->propSetString(pp, kOfxPropLabel, 0, "Match Midtones (Power)");
    gPropSuite->propSetInt(pp, kOfxParamPropDefault, 0, 0);
    gPropSuite->propSetString(pp, kOfxParamPropHint, 0,
        "Auto Match also estimates a Power (gamma) from the mean luma, "
        "limited to 0.9-1.1. Off by default. With a reference grabbed by an "
        "older version, Grab Reference again. Applies at the next Auto Match.");

    // ── Auto Match button ─────────────────────────────────────
    gParamSuite->paramDefine(paramSet, kOfxParamTypePushButton, PARAM_AUTO_MATCH, &pp);
    gPropSuite->propSetString(pp, kOfxPropLabel, 0, "Auto Match");
    gPropSuite->propSetString(pp, kOfxParamPropHint, 0,
        "Analyze Source and Reference, then write CDL parameters below. "
        "For best results, operate in a log color space (LogC3, S-Log3, "
        "V-Log, ACEScct). Optional mask inputs (blue triangles) restrict "
        "analysis to specific areas.");

    // ── Grab Reference button ─────────────────────────────────
    // Permette di campionare le statistiche della reference senza usare
    // un clip esterno. L'utente naviga al frame della reference (CAM A),
    // clicca Grab Reference, poi torna alla clip da matchare (CAM B)
    // e clicca Auto Match. Le statistiche sono salvate nel progetto.
    gParamSuite->paramDefine(paramSet, kOfxParamTypePushButton, PARAM_GRAB_REF, &pp);
    gPropSuite->propSetString(pp, kOfxPropLabel, 0, "Grab Reference");
    gPropSuite->propSetString(pp, kOfxParamPropHint, 0,
        "Sample the current frame as reference. "
        "Navigate to your reference clip (e.g. CAM A), click this button, "
        "then navigate to the clip you want to match (e.g. CAM B) and click Auto Match. "
        "No external clip needed. Saved in the project.");

    // ── Reference status (read-only) ──────────────────────────
    gParamSuite->paramDefine(paramSet, kOfxParamTypeString, PARAM_REF_STATUS, &pp);
    gPropSuite->propSetString(pp, kOfxPropLabel, 0, "Ref Status");
    gPropSuite->propSetString(pp, kOfxParamPropDefault, 0, "No reference grabbed.");
    gPropSuite->propSetInt(pp, kOfxParamPropEnabled, 0, 0); // read-only

    // ── Statistiche reference salvate (parametri nascosti) ────
    // Questi parametri non sono visibili nel pannello ma vengono
    // serializzati automaticamente da Resolve nel progetto .drp
    // Il valore 0 indica "nessun grab eseguito"
    auto defineHidden = [&](const char* name) {
        OfxPropertySetHandle hpp;
        gParamSuite->paramDefine(paramSet, kOfxParamTypeDouble, name, &hpp);
        gPropSuite->propSetDouble(hpp, kOfxParamPropDefault, 0, 0.0);
        gPropSuite->propSetInt(hpp, kOfxParamPropSecret, 0, 1);
    };
    defineHidden(PARAM_REF_NEUTRAL_R);
    defineHidden(PARAM_REF_NEUTRAL_G);
    defineHidden(PARAM_REF_NEUTRAL_B);
    defineHidden(PARAM_REF_BLACK);
    defineHidden(PARAM_REF_WHITE);
    defineHidden(PARAM_REF_SAT);
    defineHidden(PARAM_REF_MEAN);

    // ── Export CDL button ─────────────────────────────────────
    gParamSuite->paramDefine(paramSet, kOfxParamTypePushButton, PARAM_EXPORT_CDL, &pp);
    gPropSuite->propSetString(pp, kOfxPropLabel, 0, "Export CDL");
    gPropSuite->propSetString(pp, kOfxParamPropHint, 0,
        "Save current values as .cdl file in ~/Documents/MagicMatch/CDL/");

    // ── Intensity ─────────────────────────────────────────────
    defineSlider(paramSet, PARAM_INTENSITY, "Intensity", 1.0, 0.0, 1.0, 0.0, 1.0);

    // ── Slope (multiplicative) ────────────────────────────────
    defineSlider(paramSet, PARAM_SLOPE_R, "Slope R", 1.0, 0.0, 4.0, 0.0, 2.0);
    defineSlider(paramSet, PARAM_SLOPE_G, "Slope G", 1.0, 0.0, 4.0, 0.0, 2.0);
    defineSlider(paramSet, PARAM_SLOPE_B, "Slope B", 1.0, 0.0, 4.0, 0.0, 2.0);

    // ── Offset (additive) ─────────────────────────────────────
    defineSlider(paramSet, PARAM_OFFSET_R, "Offset R", 0.0, -1.0, 1.0, -0.5, 0.5);
    defineSlider(paramSet, PARAM_OFFSET_G, "Offset G", 0.0, -1.0, 1.0, -0.5, 0.5);
    defineSlider(paramSet, PARAM_OFFSET_B, "Offset B", 0.0, -1.0, 1.0, -0.5, 0.5);

    // ── Power (gamma) ─────────────────────────────────────────
    defineSlider(paramSet, PARAM_POWER_R, "Power R", 1.0, 0.1, 4.0, 0.5, 2.0);
    defineSlider(paramSet, PARAM_POWER_G, "Power G", 1.0, 0.1, 4.0, 0.5, 2.0);
    defineSlider(paramSet, PARAM_POWER_B, "Power B", 1.0, 0.1, 4.0, 0.5, 2.0);

    // ── Saturation ────────────────────────────────────────────
    defineSlider(paramSet, PARAM_SATURATION, "Saturation", 1.0, 0.0, 3.0, 0.0, 2.0);

    // ── Skin protection ───────────────────────────────────────
    gParamSuite->paramDefine(paramSet, kOfxParamTypeBoolean, PARAM_SKIN_ENABLE, &pp);
    gPropSuite->propSetString(pp, kOfxPropLabel, 0, "Skin Protection");
    gPropSuite->propSetInt(pp, kOfxParamPropDefault, 0, 0);

    defineSlider(paramSet, PARAM_SKIN_PROTECT, "Skin Strength", 0.85, 0.0, 1.0, 0.0, 1.0);

    // ── Clamp Output ──────────────────────────────────────────
    gParamSuite->paramDefine(paramSet, kOfxParamTypeBoolean, PARAM_CLAMP_OUT, &pp);
    gPropSuite->propSetString(pp, kOfxPropLabel, 0, "Clamp Output");
    gPropSuite->propSetInt(pp, kOfxParamPropDefault, 0, 0);
    gPropSuite->propSetString(pp, kOfxParamPropHint, 0,
        "Limit the output to 0-1. Off by default so that values above 1.0 "
        "(linear, ACEScg, HDR) are preserved.");

    return kOfxStatOK;
}

static OfxStatus onCreateInstance(OfxImageEffectHandle effect) {
    Instance* inst = new Instance();
    OfxParamSetHandle paramSet;
    gEffectSuite->getParamSet(effect, &paramSet);

    gParamSuite->paramGetHandle(paramSet, PARAM_AUTO_MATCH,    &inst->pAutoMatch,   0);
    gParamSuite->paramGetHandle(paramSet, PARAM_EXPORT_CDL,    &inst->pExportCDL,   0);
    gParamSuite->paramGetHandle(paramSet, PARAM_INTENSITY,     &inst->pIntensity,   0);
    gParamSuite->paramGetHandle(paramSet, PARAM_SLOPE_R,       &inst->pSlopeR,      0);
    gParamSuite->paramGetHandle(paramSet, PARAM_SLOPE_G,       &inst->pSlopeG,      0);
    gParamSuite->paramGetHandle(paramSet, PARAM_SLOPE_B,       &inst->pSlopeB,      0);
    gParamSuite->paramGetHandle(paramSet, PARAM_OFFSET_R,      &inst->pOffsetR,     0);
    gParamSuite->paramGetHandle(paramSet, PARAM_OFFSET_G,      &inst->pOffsetG,     0);
    gParamSuite->paramGetHandle(paramSet, PARAM_OFFSET_B,      &inst->pOffsetB,     0);
    gParamSuite->paramGetHandle(paramSet, PARAM_POWER_R,       &inst->pPowerR,      0);
    gParamSuite->paramGetHandle(paramSet, PARAM_POWER_G,       &inst->pPowerG,      0);
    gParamSuite->paramGetHandle(paramSet, PARAM_POWER_B,       &inst->pPowerB,      0);
    gParamSuite->paramGetHandle(paramSet, PARAM_SATURATION,    &inst->pSaturation,  0);
    gParamSuite->paramGetHandle(paramSet, PARAM_MATCH_WHITE,   &inst->pMatchWhite,  0);
    gParamSuite->paramGetHandle(paramSet, PARAM_MATCH_POWER,   &inst->pMatchPower,  0);
    gParamSuite->paramGetHandle(paramSet, PARAM_REF_MEAN,      &inst->pRefMean,     0);
    gParamSuite->paramGetHandle(paramSet, PARAM_CLAMP_OUT,     &inst->pClampOut,    0);
    gParamSuite->paramGetHandle(paramSet, PARAM_SKIN_ENABLE,   &inst->pSkinEnable,  0);
    gParamSuite->paramGetHandle(paramSet, PARAM_SKIN_PROTECT,  &inst->pSkinProtect, 0);

    // Grab Reference handles
    gParamSuite->paramGetHandle(paramSet, PARAM_GRAB_REF,      &inst->pGrabRef,     0);
    gParamSuite->paramGetHandle(paramSet, PARAM_REF_NEUTRAL_R, &inst->pRefNeutralR, 0);
    gParamSuite->paramGetHandle(paramSet, PARAM_REF_NEUTRAL_G, &inst->pRefNeutralG, 0);
    gParamSuite->paramGetHandle(paramSet, PARAM_REF_NEUTRAL_B, &inst->pRefNeutralB, 0);
    gParamSuite->paramGetHandle(paramSet, PARAM_REF_BLACK,     &inst->pRefBlack,    0);
    gParamSuite->paramGetHandle(paramSet, PARAM_REF_WHITE,     &inst->pRefWhite,    0);
    gParamSuite->paramGetHandle(paramSet, PARAM_REF_SAT,       &inst->pRefSat,      0);
    gParamSuite->paramGetHandle(paramSet, PARAM_REF_STATUS,    &inst->pRefStatus,   0);

    gEffectSuite->clipGetHandle(effect, CLIP_SOURCE,      &inst->cSource,     0);
    gEffectSuite->clipGetHandle(effect, CLIP_REFERENCE,   &inst->cReference,  0);
    gEffectSuite->clipGetHandle(effect, CLIP_OUTPUT,      &inst->cOutput,     0);
    gEffectSuite->clipGetHandle(effect, CLIP_SOURCE_MASK, &inst->cSourceMask, 0);
    gEffectSuite->clipGetHandle(effect, CLIP_REF_MASK,    &inst->cRefMask,    0);

    OfxPropertySetHandle effectProps;
    gEffectSuite->getPropertySet(effect, &effectProps);
    gPropSuite->propSetPointer(effectProps, kOfxPropInstanceData, 0, (void*)inst);

    return kOfxStatOK;
}

static OfxStatus onDestroyInstance(OfxImageEffectHandle effect) {
    OfxPropertySetHandle props;
    gEffectSuite->getPropertySet(effect, &props);
    void* p = NULL;
    gPropSuite->propGetPointer(props, kOfxPropInstanceData, 0, &p);
    delete (Instance*)p;
    return kOfxStatOK;
}

// Handler per push button — chiamato quando l'utente clicca
static OfxStatus onInstanceChanged(OfxImageEffectHandle effect,
                                    OfxPropertySetHandle inArgs) {
    OfxPropertySetHandle effectProps;
    gEffectSuite->getPropertySet(effect, &effectProps);
    void* p = NULL;
    gPropSuite->propGetPointer(effectProps, kOfxPropInstanceData, 0, &p);
    Instance* inst = (Instance*)p;
    if (!inst) return kOfxStatOK;

    char* paramName = NULL;
    gPropSuite->propGetString(inArgs, kOfxPropName, 0, &paramName);
    if (!paramName) return kOfxStatOK;

    double time = 0;
    gPropSuite->propGetDouble(inArgs, kOfxPropTime, 0, &time);

    if (strcmp(paramName, PARAM_AUTO_MATCH) == 0) {
        runAutoMatch(inst, time);
    } else if (strcmp(paramName, PARAM_GRAB_REF) == 0) {
        runGrabReference(inst, time);
    } else if (strcmp(paramName, PARAM_EXPORT_CDL) == 0) {
        runExportCDL(inst, time);
    }

    return kOfxStatOK;
}

// ─────────────────────────────────────────────────────────────────────
// RENDER
// ─────────────────────────────────────────────────────────────────────

static OfxStatus onRender(OfxImageEffectHandle effect, OfxPropertySetHandle inArgs) {
    OfxPropertySetHandle effectProps;
    gEffectSuite->getPropertySet(effect, &effectProps);
    void* p = NULL;
    gPropSuite->propGetPointer(effectProps, kOfxPropInstanceData, 0, &p);
    Instance* inst = (Instance*)p;
    if (!inst) return kOfxStatErrBadHandle;

    double time = 0;
    gPropSuite->propGetDouble(inArgs, kOfxPropTime, 0, &time);

    OfxRectI renderWin;
    gPropSuite->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, (int*)&renderWin);

    // Leggi tutti i parametri
    double intensity = 1.0;
    double slopeR=1, slopeG=1, slopeB=1;
    double offsetR=0, offsetG=0, offsetB=0;
    double powerR=1, powerG=1, powerB=1;
    double sat = 1.0;
    int    skinEnable = 0;
    double skinStr = 0.85;
    int    clampOut = 0;

    gParamSuite->paramGetValueAtTime(inst->pIntensity,    time, &intensity);
    gParamSuite->paramGetValueAtTime(inst->pSlopeR,       time, &slopeR);
    gParamSuite->paramGetValueAtTime(inst->pSlopeG,       time, &slopeG);
    gParamSuite->paramGetValueAtTime(inst->pSlopeB,       time, &slopeB);
    gParamSuite->paramGetValueAtTime(inst->pOffsetR,      time, &offsetR);
    gParamSuite->paramGetValueAtTime(inst->pOffsetG,      time, &offsetG);
    gParamSuite->paramGetValueAtTime(inst->pOffsetB,      time, &offsetB);
    gParamSuite->paramGetValueAtTime(inst->pPowerR,       time, &powerR);
    gParamSuite->paramGetValueAtTime(inst->pPowerG,       time, &powerG);
    gParamSuite->paramGetValueAtTime(inst->pPowerB,       time, &powerB);
    gParamSuite->paramGetValueAtTime(inst->pSaturation,   time, &sat);
    gParamSuite->paramGetValueAtTime(inst->pSkinEnable,   time, &skinEnable);
    gParamSuite->paramGetValueAtTime(inst->pSkinProtect,  time, &skinStr);
    gParamSuite->paramGetValueAtTime(inst->pClampOut,     time, &clampOut);

    OfxPropertySetHandle srcImg = NULL, outImg = NULL;
    gEffectSuite->clipGetImage(inst->cSource, time, NULL, &srcImg);
    gEffectSuite->clipGetImage(inst->cOutput, time, NULL, &outImg);
    if (!srcImg || !outImg) {
        if (srcImg) gEffectSuite->clipReleaseImage(srcImg);
        if (outImg) gEffectSuite->clipReleaseImage(outImg);
        return kOfxStatFailed;
    }

    // Solo float RGBA: altrimenti si leggerebbe memoria con layout sbagliato
    ImgView sv, ov;
    if (!getView(srcImg, false, sv) || !getView(outImg, false, ov)) {
        gEffectSuite->clipReleaseImage(srcImg);
        gEffectSuite->clipReleaseImage(outImg);
        return kOfxStatErrImageFormat;
    }
    void* srcData = sv.data; void* outData = ov.data;
    int srcRB = sv.rb, outRB = ov.rb;
    OfxRectI srcBounds = sv.bounds, outBounds = ov.bounds;

    for (int y = renderWin.y1; y < renderWin.y2; y++) {
        int sy = y - srcBounds.y1;
        int oy = y - outBounds.y1;
        if (sy < 0 || sy >= (srcBounds.y2 - srcBounds.y1)) continue;

        const float* srcRow = (const float*)((const uint8_t*)srcData + sy*srcRB);
        float*       outRow = (float*)((uint8_t*)outData + oy*outRB);

        for (int x = renderWin.x1; x < renderWin.x2; x++) {
            int sx = x - srcBounds.x1;
            int ox = x - outBounds.x1;
            if (sx < 0 || sx >= (srcBounds.x2 - srcBounds.x1)) continue;

            float r = srcRow[sx*4+0];
            float g = srcRow[sx*4+1];
            float b = srcRow[sx*4+2];
            float a = srcRow[sx*4+3];

            // Applica CDL (SOP + Sat)
            float r2 = applyCDLChannel(r, (float)slopeR, (float)offsetR, (float)powerR);
            float g2 = applyCDLChannel(g, (float)slopeG, (float)offsetG, (float)powerG);
            float b2 = applyCDLChannel(b, (float)slopeB, (float)offsetB, (float)powerB);
            applySaturation(r2, g2, b2, (float)sat);

            // Intensity blend con originale
            r2 = r + (r2 - r) * (float)intensity;
            g2 = g + (g2 - g) * (float)intensity;
            b2 = b + (b2 - b) * (float)intensity;

            // Skin protection
            if (skinEnable) {
                float w = skinWeight(r, g, b) * (float)skinStr;
                r2 = r + (r2 - r) * (1.0f - w);
                g2 = g + (g2 - g) * (1.0f - w);
                b2 = b + (b2 - b) * (1.0f - w);
            }

            // Output non limitato a 1.0 (linear/ACEScg/HDR) salvo Clamp Output
            outRow[ox*4+0] = clampOut ? clamp01(r2) : r2;
            outRow[ox*4+1] = clampOut ? clamp01(g2) : g2;
            outRow[ox*4+2] = clampOut ? clamp01(b2) : b2;
            outRow[ox*4+3] = a;
        }
    }

    gEffectSuite->clipReleaseImage(srcImg);
    gEffectSuite->clipReleaseImage(outImg);
    return kOfxStatOK;
}

// ─────────────────────────────────────────────────────────────────────
// MAIN ENTRY
// ─────────────────────────────────────────────────────────────────────

static OfxStatus pluginMain(const char* action, const void* handle,
                             OfxPropertySetHandle inArgs, OfxPropertySetHandle) {
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;

    if (strcmp(action, kOfxActionLoad) == 0)                         return onLoad();
    if (strcmp(action, kOfxActionUnload) == 0)                       return kOfxStatOK;
    if (strcmp(action, kOfxActionDescribe) == 0)                     return onDescribe(effect);
    if (strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) return onDescribeInContext(effect, inArgs);
    if (strcmp(action, kOfxActionCreateInstance) == 0)               return onCreateInstance(effect);
    if (strcmp(action, kOfxActionDestroyInstance) == 0)              return onDestroyInstance(effect);
    if (strcmp(action, kOfxActionInstanceChanged) == 0)              return onInstanceChanged(effect, inArgs);
    if (strcmp(action, kOfxImageEffectActionRender) == 0)            return onRender(effect, inArgs);
    return kOfxStatReplyDefault;
}

static void setHostFunc(OfxHost* host) { gHost = host; }

static OfxPlugin gPlugin = {
    kOfxImageEffectPluginApi,
    kOfxImageEffectPluginApiVersion,
    "com.strandlabs.magicmatch",
    1, 0,
    setHostFunc,
    pluginMain
};

#ifdef _WIN32
  #define OFX_EXPORT __declspec(dllexport)
#else
  #define OFX_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {
    OFX_EXPORT OfxPlugin* OfxGetPlugin(int nth) {
        return (nth == 0) ? &gPlugin : NULL;
    }
    OFX_EXPORT int OfxGetNumberOfPlugins(void) {
        return 1;
    }
}
