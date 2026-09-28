#include "character.h"
#include <M5Unified.h>
#include <LittleFS.h>
#include <AnimatedGIF.h>
#include <ArduinoJson.h>

// Renders into the dedicated pet canvas (left panel); see main.cpp.
extern M5Canvas petSpr;

static const char* STATE_NAMES[] = {
  "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"
};
static const uint8_t N_STATES = 7;

// Text mode: manifest has "mode":"text", states contain {frames:[...],delay:N}.
// Frames are short strings rendered at text size 2, centered. No GIF pipeline.
struct TextState {
  char     frames[8][20];
  uint8_t  nFrames;
  uint16_t delayMs;
};
static TextState textStates[N_STATES];
static bool      textMode = false;
static uint8_t   textFrame = 0;
static uint32_t  textNext = 0;

static bool    loaded = false;
static Palette pal = { 0xC2A6, 0x0000, 0xFFFF, 0x8410, 0x0000 };
static char    basePath[48];
static const uint8_t MAX_GIFS = 32;
static char    gifPaths[MAX_GIFS][32];
static uint8_t stateStart[N_STATES];
static uint8_t stateCount[N_STATES];
static uint8_t stateRot[N_STATES];
static uint8_t gifTotal = 0;
static uint8_t curState = 0xFF;

static AnimatedGIF gif;
static File        gifFile;
static int         gifX = 0, gifY = 0, gifW = 0, gifH = 0;
// Center the GIF in the pet canvas. Source art is 96px wide and up to
// ~140px tall, so it always fits the 150×180 panel at 1:1.
static void gifPlace() {
  gifX = (petSpr.width() - gifW) / 2;
  gifY = (petSpr.height() - gifH) / 2;
  if (gifY < 0) gifY = 0;
}
static uint32_t    nextFrameAt = 0;
static uint32_t    animPauseUntil = 0;
static uint32_t    variantStartedMs = 0;
static const uint32_t VARIANT_DWELL_MS = 5000;
static const uint32_t ANIM_PAUSE_MS    = 800;
static bool        gifOpen = false;

static uint16_t parseHexColor(const char* s, uint16_t fallback) {
  if (!s) return fallback;
  if (*s == '#') s++;
  uint32_t v = strtoul(s, nullptr, 16);
  return (uint16_t)(((v >> 19) & 0x1F) << 11 | ((v >> 10) & 0x3F) << 5 | ((v >> 3) & 0x1F));
}

// --- AnimatedGIF file callbacks (LittleFS) ------------------------------

static void* gifOpenCb(const char* fname, int32_t* pSize) {
  gifFile = LittleFS.open(fname, "r");
  if (!gifFile) return nullptr;
  *pSize = gifFile.size();
  return (void*)&gifFile;
}

static void gifCloseCb(void* handle) {
  File* f = (File*)handle;
  if (f) f->close();
}

static int32_t gifReadCb(GIFFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  File* f = (File*)pFile->fHandle;
  int32_t n = f->read(pBuf, iLen);
  pFile->iPos = f->position();
  return n;
}

static int32_t gifSeekCb(GIFFILE* pFile, int32_t iPosition) {
  File* f = (File*)pFile->fHandle;
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// --- Draw callback: one scanline → line buffer → pushImage ------------
// Transparent pixels get the character's bg color so each frame fully
// paints its region — no ghosting from prior frames.

static void gifDrawCb(GIFDRAW* d) {
  uint16_t* pal16 = d->pPalette;
  uint8_t*  src   = d->pPixels;
  uint8_t   t     = d->ucTransparent;
  bool      hasT  = d->ucHasTransparency;
  int       srcY  = d->iY + d->y;
  // GIFs are unoptimized full-frame (gifsicle --unoptimize --lossy) so
  // transparent always means background — no disposal/delta handling.
  // The -O2/-O3 sub-rect + delta-transparency path was tried and reverted:
  // disposal semantics are encoder-dependent.
  auto put = [&](int x, int y, uint8_t idx) {
    petSpr.drawPixel(x, y, (uint16_t)((hasT && idx == t) ? pal.bg : pal16[idx]));
  };

  int y = gifY + srcY;
  if (y < 0 || y >= petSpr.height()) return;
  int x0 = gifX + d->iX;
  int w  = d->iWidth;
  if (w > 256) w = 256;
  if (x0 < 0) { src -= x0; w += x0; x0 = 0; }
  if (x0 + w > petSpr.width()) w = petSpr.width() - x0;
  if (w <= 0) return;
  for (int i = 0; i < w; i++) put(x0 + i, y, src[i]);
}

// --- Public -------------------------------------------------------------

bool characterInit(const char* name) {
  if (!LittleFS.begin(false)) {
    // begin() fails if already mounted — that's fine on reload
    if (!LittleFS.open("/")) {
      Serial.println("[char] LittleFS mount failed");
      return false;
    }
  }

  // No name → scan /characters/ for the first directory present.
  // Makes the boot character whatever you last installed.
  static char scanned[24];
  if (!name) {
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e = d.openNextFile();
      while (e) {
        if (e.isDirectory()) {
          const char* n = strrchr(e.name(), '/');
          strncpy(scanned, n ? n + 1 : e.name(), sizeof(scanned) - 1);
          scanned[sizeof(scanned) - 1] = 0;
          name = scanned;
          break;
        }
        e = d.openNextFile();
      }
      d.close();
    }
    if (!name) { Serial.println("[char] no characters installed"); return false; }
  }

  snprintf(basePath, sizeof(basePath), "/characters/%s", name);
  char mpath[64];
  snprintf(mpath, sizeof(mpath), "%s/manifest.json", basePath);

  File mf = LittleFS.open(mpath, "r");
  if (!mf) {
    Serial.printf("[char] manifest not found: %s\n", mpath);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, mf);
  mf.close();
  if (err) {
    Serial.printf("[char] manifest parse: %s\n", err.c_str());
    return false;
  }

  JsonObject colors = doc["colors"];
  pal.body    = parseHexColor(colors["body"],    pal.body);
  pal.bg      = parseHexColor(colors["bg"],      pal.bg);
  pal.text    = parseHexColor(colors["text"],    pal.text);
  pal.textDim = parseHexColor(colors["textDim"], pal.textDim);
  pal.ink     = parseHexColor(colors["ink"],     pal.ink);

  const char* mode = doc["mode"];
  textMode = (mode && strcmp(mode, "text") == 0);

  JsonObject states = doc["states"];

  if (textMode) {
    for (uint8_t i = 0; i < N_STATES; i++) {
      TextState& ts = textStates[i];
      ts.nFrames = 0;
      ts.delayMs = 200;
      JsonObject st = states[STATE_NAMES[i]];
      if (st.isNull()) continue;
      ts.delayMs = st["delay"] | 200;
      JsonArray fr = st["frames"];
      for (JsonVariant v : fr) {
        if (ts.nFrames >= 8) break;
        const char* s = v.as<const char*>();
        strncpy(ts.frames[ts.nFrames], s ? s : "", 19);
        ts.frames[ts.nFrames][19] = 0;
        ts.nFrames++;
      }
    }
    loaded = true;
    Serial.printf("[char] loaded '%s' (text mode, %d states)\n", name, N_STATES);
    return true;
  }

  gifTotal = 0;
  for (uint8_t i = 0; i < N_STATES; i++) {
    stateStart[i] = gifTotal;
    stateCount[i] = 0;
    stateRot[i]   = 0;
    JsonVariant v = states[STATE_NAMES[i]];
    if (v.is<JsonArray>()) {
      for (JsonVariant e : v.as<JsonArray>()) {
        if (gifTotal >= MAX_GIFS) break;
        const char* fn = e.as<const char*>();
        if (fn) { snprintf(gifPaths[gifTotal], 32, "%s", fn); gifTotal++; stateCount[i]++; }
      }
    } else {
      const char* fn = v.as<const char*>();
      if (fn) { snprintf(gifPaths[gifTotal], 32, "%s", fn); gifTotal++; stateCount[i] = 1; }
    }
  }

  gif.begin(LITTLE_ENDIAN_PIXELS);
  loaded = true;
  Serial.printf("[char] loaded '%s' from %s\n", (const char*)doc["name"], basePath);
  return true;
}

bool characterLoaded() { return loaded; }
const Palette& characterPalette() { return pal; }

void characterClose() {
  if (gifOpen) { gif.close(); gifOpen = false; }
  loaded = false;
  textMode = false;
  curState = 0xFF;
}

void characterInvalidate() {
  if (!loaded) return;
  if (textMode) {
    petSpr.fillSprite(pal.bg);
    uint8_t s = curState; curState = 0xFF;
    characterSetState(s);
    return;
  }
  if (gifOpen) { gif.close(); gifOpen = false; }
  animPauseUntil = 0;
  uint8_t s = curState; curState = 0xFF;
  characterSetState(s);
}

void characterSetState(uint8_t s) {
  if (!loaded || s >= N_STATES || s == curState) return;

  if (textMode) {
    curState = s;
    textFrame = 0;
    textNext = 0;
    petSpr.fillSprite(pal.bg);
    return;
  }

  if (gifOpen) { gif.close(); gifOpen = false; }
  animPauseUntil = 0;
  curState = s;

  if (stateCount[s] == 0) {
    Serial.printf("[char] no gif for state %d\n", s);
    return;
  }

  uint8_t idx = stateStart[s] + stateRot[s];
  char full[80];
  snprintf(full, sizeof(full), "%s/%s", basePath, gifPaths[idx]);
  if (gif.open(full, gifOpenCb, gifCloseCb, gifReadCb, gifSeekCb, gifDrawCb)) {
    gifOpen = true;
    gifW = gif.getCanvasWidth();
    gifH = gif.getCanvasHeight();
    gifPlace();
    petSpr.fillSprite(pal.bg);
    nextFrameAt = 0;
    variantStartedMs = millis();
    Serial.printf("[char] %s: %dx%d @ (%d,%d) heap=%u\n",
      gifPaths[idx], gifW, gifH, gifX, gifY, ESP.getFreeHeap());
  } else {
    Serial.printf("[char] open failed: %s (err %d)\n", full, gif.getLastError());
  }
}

void characterTick() {
  if (!loaded) return;

  if (textMode) {
    TextState& ts = textStates[curState];
    if (ts.nFrames == 0) return;
    uint32_t now = millis();
    if (now < textNext) return;
    textNext = now + ts.delayMs;

    int cy = petSpr.height() / 2;
    petSpr.fillRect(0, cy - 14, petSpr.width(), 28, pal.bg);

    const char* line = ts.frames[textFrame];
    int len = strlen(line);
    int tw = len * 12;                                    // size-2 glyph width
    petSpr.setTextColor(pal.body, pal.bg);
    petSpr.setTextSize(2);
    petSpr.setCursor((petSpr.width() - tw) / 2, cy - 8);
    petSpr.print(line);

    textFrame = (textFrame + 1) % ts.nFrames;
    return;
  }

  uint32_t now = millis();

  if (!gifOpen) {
    // Between animations in a rotation: hold the last frame, then open
    // the next gif when the pause elapses.
    if (animPauseUntil && now >= animPauseUntil) {
      animPauseUntil = 0;
      uint8_t s = curState; curState = 0xFF;
      characterSetState(s);
    }
    return;
  }
  if (now < nextFrameAt) return;

  int delayMs = 0;
  if (!gif.playFrame(false, &delayMs)) {
    // End of animation. Single-gif states freeze on the last frame instead
    // of reopening — the LittleFS open + GIF header decode is a multi-ms
    // blocking burst, and during sleep state it was looping every ~4s,
    // possibly starving the BT controller. The sprite already holds the
    // last frame; just stop ticking. Multi-gif states (idle rotation)
    // still advance after a brief pause.
    if (stateCount[curState] == 1) {
      gif.close();
      gifOpen = false;
      return;
    }
    // Multi-variant: loop the same GIF until the dwell window elapses, then
    // rotate. Short bufo idles (~0.5s/loop) get ~10 plays instead of one
    // flash + 3s freeze.
    if (now - variantStartedMs < VARIANT_DWELL_MS) {
      gif.reset();
      nextFrameAt = now;
      return;
    }
    gif.close(); gifOpen = false;
    stateRot[curState] = (stateRot[curState] + 1) % stateCount[curState];
    animPauseUntil = now + ANIM_PAUSE_MS;
    return;
  }
  nextFrameAt = now + (delayMs > 0 ? delayMs : 100);
}
