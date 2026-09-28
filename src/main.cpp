#include <M5Unified.h>
#include <LittleFS.h>
#include <stdarg.h>
#include "ble_bridge.h"
#include "data.h"
#include "buddy.h"

// M5Stack CoreS3: 320×240 landscape, capacitive touch, no front buttons.
//
//   ┌──────────────────────── status bar ────────────────────────┐  20px
//   │  pet panel      │  content panel (chat / pet / info)       │ 180px
//   │  150×180        │  170×180                                 │
//   ├──────── chat ─────── pet ─────── info ─────── menu ────────┤  40px tabs
//
// The pet lives in its own canvas (petSpr) and is composited into the
// full-screen UI sprite each frame, so overlays never disturb its state.
// Approval prompts replace the tab bar with two large DENY / APPROVE
// buttons; menus are full-screen tile grids sized for fingers.

M5Canvas spr(&M5.Display);   // full-screen UI (PSRAM)
M5Canvas petSpr(&spr);       // pet panel, pushed into spr

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple devices
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}

#include "character.h"
#include "stats.h"

// ──────────────── layout ────────────────
const int W = 320, H = 240;
const int BAR_H = 20;                 // status bar
const int TAB_H = 40;                 // bottom tab bar
const int TAB_Y = H - TAB_H;
const int PET_W = 150;                // left pet panel
const int PET_H = TAB_Y - BAR_H;
const int PX = PET_W, PY = BAR_H;     // right content panel origin
const int PW = W - PET_W, PH = TAB_Y - BAR_H;
const int BTN_Y = 178;                // approval button row (covers tabs)
const int LH = 10;                    // text line height at size 1
const int PANEL_COLS = (PW - 10) / 6; // size-1 glyphs per content row (26)

// Colors used across multiple UI surfaces. Typed uint16_t so LovyanGFX
// treats them as RGB565 (a bare int literal would be read as RGB888).
const uint16_t HOT     = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL   = 0x2104;   // overlay/tile background
const uint16_t C_GREEN = 0x07E0;
const uint16_t C_RED   = 0xF800;
const uint16_t C_CYAN  = 0x07FF;
const uint16_t C_YEL   = 0xFFE0;
const uint16_t C_WHITE = 0xFFFF;
const uint16_t C_BLACK = 0x0000;
const uint16_t DENY_BG = 0x9000;
const uint16_t OK_BG   = 0x04A0;

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;

// Overlays
bool    menuOpen     = false;
bool    settingsOpen = false;
bool    resetOpen    = false;
uint8_t brightLevel  = 3;          // 0..4 → BRIGHT[]
static const uint8_t BRIGHT[5] = { 20, 60, 110, 170, 255 };

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint8_t maxScroll = 0;             // set by drawChat, read by drag handler
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowTouch = false;
bool     buddyMode = false;
bool     gifAvailable = false;
bool     inPrompt = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;
// Touches during the first moments of a prompt are ignored so a finger
// already on the glass (or a grab to pick the device up) can't approve.
const uint32_t PROMPT_ARM_MS = 600;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { M5.Display.setBrightness(BRIGHT[brightLevel]); }
static void applyRotation()   { M5.Display.setRotation(settings().flip ? 3 : 1); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5.Display.wakeup();
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
static void screenSleep() {
  M5.Display.sleep();
  screenOff = true;
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Speaker.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_CONTROLS = 1;
const uint8_t INFO_PG_CREDITS = 5;

// ──────────────── attention LED ────────────────
// CoreS3 has no user LED on a GPIO, but the AXP2101 CHGLED can be driven
// manually (same register M5Unified uses for Core2). We save its boot
// value and hand it back to the charger when attention ends.
static bool    _ledCtl = false;
static uint8_t _ledDefault = 0;
static int8_t  _ledState = -1;   // -1 = charger-controlled
static void ledInit() {
  _ledCtl = M5.Power.getType() == m5::Power_Class::pmic_t::pmic_axp2101;
  if (_ledCtl) _ledDefault = M5.Power.Axp2101.readRegister8(0x69);
}
static void ledSet(int8_t s) {
  if (!_ledCtl || s == _ledState) return;
  _ledState = s;
  if (s < 0) M5.Power.Axp2101.writeRegister8(0x69, _ledDefault);
  else       M5.Power.Axp2101.writeRegister8(0x69, s ? 0x35 : 0x05);
}

// ──────────────── menus ────────────────
const char* menuItems[] = { "settings", "help", "about", "demo", "turn off" };
const uint8_t MENU_N = 5;

const char* settingsItems[] = { "brightness", "sound", "bluetooth", "alert blink",
                                "transcript", "flip screen", "pet", "reset" };
const uint8_t SETTINGS_N = 8;

const char* resetItems[] = { "delete char", "factory reset" };
const uint8_t RESET_N = 2;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3: s.led = !s.led; break;
    case 4: s.hud = !s.hud; break;
    case 5: s.flip = !s.flip; applyRotation(); break;
    case 6: nextPet(); return;
    case 7: resetOpen = true; resetConfirmIdx = 0xFF; return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
    bleClearBonds();
  }
  delay(300);
  ESP.restart();
}

static void menuConfirm(uint8_t idx) {
  switch (idx) {
    case 0: settingsOpen = true; menuOpen = false; break;
    case 1:
    case 2:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (idx == 1) ? INFO_PG_CONTROLS : INFO_PG_CREDITS;
      break;
    case 3: dataSetDemo(!dataDemo()); break;
    case 4: M5.Power.powerOff(); break;
  }
}

// ──────────────── tile grid (menus) ────────────────
// Two columns, full width, big enough to hit with a thumb.
const int GRID_Y   = BAR_H + 36;
const int GRID_GAP = 6;
const int TILE_W   = (W - GRID_GAP * 3) / 2;

static void tileRect(uint8_t i, int th, int& x, int& y) {
  x = GRID_GAP + (i % 2) * (TILE_W + GRID_GAP);
  y = GRID_Y + (i / 2) * (th + GRID_GAP);
}

static int tileHit(int tx, int ty, uint8_t n, int th) {
  for (uint8_t i = 0; i < n; i++) {
    int x, y; tileRect(i, th, x, y);
    if (tx >= x && tx < x + TILE_W && ty >= y && ty < y + th) return i;
  }
  return -1;
}

// Label only → size 2 centered. Label + value → small label on top,
// big value below (settings).
static void drawTile(uint8_t i, int th, const char* label, const char* value,
                     uint16_t valCol, uint16_t border) {
  const Palette& p = characterPalette();
  int x, y; tileRect(i, th, x, y);
  spr.fillRoundRect(x, y, TILE_W, th, 8, PANEL);
  spr.drawRoundRect(x, y, TILE_W, th, 8, border);
  if (!value) {
    spr.setTextDatum(middle_center);
    spr.setTextSize(2);
    spr.setTextColor(p.text, PANEL);
    spr.drawString(label, x + TILE_W / 2, y + th / 2);
    spr.setTextDatum(top_left);
    return;
  }
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, PANEL);
  spr.setCursor(x + 10, y + 7);
  spr.print(label);
  spr.setTextSize(2);
  spr.setTextColor(valCol, PANEL);
  spr.setCursor(x + 10, y + th - 22);
  spr.print(value);
}

// Overlay chrome: title at left, back/close button at right.
const int HDR_BTN_W = 84;
static void drawOverlayHeader(const char* title, const char* btn, uint16_t accent) {
  const Palette& p = characterPalette();
  spr.fillRect(0, BAR_H, W, H - BAR_H, p.bg);
  spr.setTextSize(2);
  spr.setTextColor(accent, p.bg);
  spr.setCursor(10, BAR_H + 10);
  spr.print(title);
  int bx = W - HDR_BTN_W - GRID_GAP;
  spr.fillRoundRect(bx, BAR_H + 4, HDR_BTN_W, 28, 6, PANEL);
  spr.setTextDatum(middle_center);
  spr.setTextColor(p.text, PANEL);
  spr.drawString(btn, bx + HDR_BTN_W / 2, BAR_H + 18);
  spr.setTextDatum(top_left);
}
static bool overlayHeaderHit(int x, int y) {
  return y < GRID_Y - 2 && x >= W - HDR_BTN_W - GRID_GAP * 2;
}

const int MENU_TH = 44, SETTINGS_TH = 40, RESET_TH = 60;

static void drawMenu() {
  const Palette& p = characterPalette();
  drawOverlayHeader("menu", "close", p.body);
  for (uint8_t i = 0; i < MENU_N; i++) {
    if (i == 3) drawTile(i, MENU_TH, "demo", dataDemo() ? "on" : "off",
                         dataDemo() ? C_GREEN : p.textDim, p.textDim);
    else drawTile(i, MENU_TH, menuItems[i], nullptr, 0, i == 4 ? HOT : p.textDim);
  }
}

static void drawSettings() {
  const Palette& p = characterPalette();
  drawOverlayHeader("settings", "back", p.body);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.led, s.hud, s.flip };
  char buf[12];
  for (uint8_t i = 0; i < SETTINGS_N; i++) {
    const char* v = buf;
    uint16_t col = p.text;
    if (i == 0) {
      snprintf(buf, sizeof(buf), "%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      v = vals[i-1] ? "on" : "off";
      col = vals[i-1] ? C_GREEN : p.textDim;
    } else if (i == 6) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      snprintf(buf, sizeof(buf), "%u/%u", pos, total);
    } else {
      v = "...";
      col = HOT;
    }
    drawTile(i, SETTINGS_TH, settingsItems[i], v, col, i == 7 ? HOT : p.textDim);
  }
}

static void drawReset() {
  const Palette& p = characterPalette();
  drawOverlayHeader("reset", "back", HOT);
  for (uint8_t i = 0; i < RESET_N; i++) {
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    drawTile(i, RESET_TH, armed ? "really?" : resetItems[i], nullptr, 0, HOT);
  }
  spr.setTextSize(1);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(GRID_GAP + 4, GRID_Y + RESET_TH + 16);
  spr.print("tap twice to confirm. the device reboots.");
  spr.setCursor(GRID_GAP + 4, GRID_Y + RESET_TH + 30);
  spr.print("factory reset also forgets BLE pairing.");
}

// ──────────────── sensors (cached at 1Hz) ────────────────
// RTC, PMIC and IMU share the internal I2C bus; polling RTC and battery
// every frame starves the IMU reads. Refresh once per second instead.
static m5::rtc_datetime_t _clk;
uint32_t _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool    _onUsb    = false;
static bool    _charging = false;
static int     _batPct   = 0;
static int     _batMv    = 0;
static int     _batMa    = 0;
static int     _vbusMv   = 0;
static void refreshSensors() {
  if (_clkLastRead && millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _vbusMv   = M5.Power.getVBUSVoltage();
  _batMv    = M5.Power.getBatteryVoltage();
  _batMa    = M5.Power.getBatteryCurrent();
  _batPct   = M5.Power.getBatteryLevel();
  if (_batPct < 0) _batPct = 0; if (_batPct > 100) _batPct = 100;
  _charging = M5.Power.isCharging() == m5::Power_Class::is_charging;
  _onUsb    = _vbusMv > 4000 || _charging;
  M5.Rtc.getDateTime(&_clk);
}

static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
static uint8_t clockDow() { return (uint8_t)_clk.date.weekDay % 7; }

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return false;
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}

// ──────────────── status bar + tabs ────────────────
static void drawStatusBar(const Palette& p) {
  spr.fillRect(0, 0, W, BAR_H, p.bg);
  spr.drawFastHLine(0, BAR_H - 1, W, PANEL);
  spr.setTextSize(1);

  spr.setTextColor(p.text, p.bg);
  spr.setCursor(6, 6);
  if (ownerName()[0]) spr.printf("%.14s's %.14s", ownerName(), petName());
  else spr.print(petName());

  // Right to left: battery, clock, BT.
  int x = W - 6;
  int bw = 18, bh = 9, by = 5;
  x -= bw + 2;
  uint16_t bcol = _charging ? C_GREEN : (_batPct <= 20 ? HOT : p.textDim);
  spr.drawRect(x, by, bw, bh, bcol);
  spr.fillRect(x + bw, by + 3, 2, 3, bcol);
  int fill = (bw - 4) * _batPct / 100;
  if (fill > 0) spr.fillRect(x + 2, by + 2, fill, bh - 4, bcol);
  char pct[6]; snprintf(pct, sizeof(pct), "%d%%", _batPct);
  x -= strlen(pct) * 6 + 4;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(x, 6); spr.print(pct);

  if (dataRtcValid()) {
    char hm[6]; snprintf(hm, sizeof(hm), "%02d:%02d", _clk.time.hours, _clk.time.minutes);
    x -= 5 * 6 + 10;
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(x, 6); spr.print(hm);
  }

  bool linked = settings().bt && dataBtActive();
  uint16_t btCol = linked ? C_GREEN : (settings().bt ? HOT : p.textDim);
  x -= 2 * 6 + 10;
  spr.fillCircle(x - 6, 10, 3, btCol);
  spr.setTextColor(btCol, p.bg);
  spr.setCursor(x, 6); spr.print("BT");

  if (dataDemo()) {
    x -= 4 * 6 + 10;
    spr.setTextColor(C_YEL, p.bg);
    spr.setCursor(x, 6); spr.print("DEMO");
  }
}

const char* tabNames[] = { "chat", "pet", "info", "menu" };
const uint8_t TAB_N = 4;
static void drawTabs(const Palette& p) {
  spr.fillRect(0, TAB_Y, W, TAB_H, p.bg);
  spr.drawFastHLine(0, TAB_Y, W, PANEL);
  int tw = W / TAB_N;
  spr.setTextDatum(middle_center);
  spr.setTextSize(2);
  for (uint8_t i = 0; i < TAB_N; i++) {
    bool sel = (i == 3) ? menuOpen : (i == displayMode);
    int x = i * tw;
    if (sel) {
      spr.fillRoundRect(x + 4, TAB_Y + 5, tw - 8, TAB_H - 9, 8, p.body);
      spr.setTextColor(p.bg, p.body);
    } else {
      spr.setTextColor(p.textDim, p.bg);
    }
    spr.drawString(tabNames[i], x + tw / 2, TAB_Y + TAB_H / 2 + 1);
  }
  // Waiting-approval badge on the chat tab when you're elsewhere
  if (tama.sessionsWaiting > 0 && displayMode != DISP_NORMAL) {
    spr.fillCircle(tw - 12, TAB_Y + 10, 5, HOT);
  }
  spr.setTextDatum(top_left);
}

// ──────────────── content panel helpers ────────────────
static void panelClear(const Palette& p) { spr.fillRect(PX, PY, PW, PH, p.bg); }

// Screen title row + "n/N >" pager hint. Tapping anywhere in the panel
// (or swiping) advances the page.
static void panelHeader(const Palette& p, int& y, const char* title,
                        const char* section, uint8_t page, uint8_t pages) {
  spr.setTextSize(1);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(PX + 6, y); spr.print(title);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - 40, y); spr.printf("%u/%u >", page + 1, pages);
  y += 12;
  if (section) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(PX + 6, y); spr.print(section);
    y += 13;
  }
}

// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static const uint8_t WRAP_MAX = 31;
static uint8_t wrapInto(const char* in, char out[][WRAP_MAX + 1], uint8_t maxRows, uint8_t width) {
  if (width > WRAP_MAX) width = WRAP_MAX;
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

// ──────────────── chat (home) ────────────────
// Session summary, a clock when things are quiet, then the transcript.
// Drag the panel to scroll history; tap it to jump back to the newest.
static void drawChat(const Palette& p) {
  panelClear(p);
  int y = PY + 5;
  spr.setTextSize(1);

  if (tama.connected) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(PX + 6, y);
    spr.printf("%u open  ", tama.sessionsTotal);
    spr.setTextColor(tama.sessionsRunning ? C_CYAN : p.textDim, p.bg);
    spr.printf("%u busy  ", tama.sessionsRunning);
    spr.setTextColor(tama.sessionsWaiting ? HOT : p.textDim, p.bg);
    spr.printf("%u wait", tama.sessionsWaiting);
  } else {
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(PX + 6, y);
    spr.print("not connected");
  }
  y += 14;

  if (xferActive()) {
    uint32_t done = xferProgress(), total = xferTotal();
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(PX + 6, y);
    spr.printf("installing %luK/%luK", done / 1024, total / 1024);
    y += 12;
    int barW = PW - 12;
    spr.drawRect(PX + 6, y, barW, 8, p.textDim);
    if (total > 0) {
      int fill = (int)((uint64_t)barW * done / total);
      if (fill > 1) spr.fillRect(PX + 7, y + 1, fill - 1, 6, p.body);
    }
    y += 16;
  }

  bool quiet = tama.sessionsRunning == 0 && tama.sessionsWaiting == 0;
  if (quiet && dataRtcValid()) {
    char hm[6]; snprintf(hm, sizeof(hm), "%02d:%02d", _clk.time.hours, _clk.time.minutes);
    uint8_t mi = (_clk.date.month >= 1 && _clk.date.month <= 12) ? _clk.date.month - 1 : 0;
    char dl[16]; snprintf(dl, sizeof(dl), "%s %s %02d", DOW[clockDow()], MON[mi], _clk.date.date);
    spr.setTextDatum(top_center);
    spr.setTextSize(4); spr.setTextColor(p.text, p.bg);
    spr.drawString(hm, PX + PW / 2, y + 2);
    spr.setTextSize(1); spr.setTextColor(p.textDim, p.bg);
    spr.drawString(dl, PX + PW / 2, y + 38);
    spr.setTextDatum(top_left);
    y += 54;
    spr.drawFastHLine(PX + 6, y - 6, PW - 12, PANEL);
  }

  const int bottom = PY + PH - 4;
  const int SHOW = (bottom - y) / LH;

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (!settings().hud) {
    maxScroll = 0;
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(PX + 6, y);
    spr.print("transcript hidden");
    return;
  }

  if (tama.nLines == 0) {
    maxScroll = 0;
    static char one[4][WRAP_MAX + 1];
    uint8_t n = wrapInto(tama.msg, one, 4, PANEL_COLS);
    spr.setTextColor(p.text, p.bg);
    for (uint8_t i = 0; i < n; i++) {
      spr.setCursor(PX + 6, y + i * LH); spr.print(one[i]);
    }
    return;
  }

  // Wrap all transcript lines into a flat display buffer. Track which
  // transcript index each display row came from, so we can dim older ones.
  static char disp[40][WRAP_MAX + 1];
  static uint8_t srcOf[40];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 40; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 40 - nDisp, PANEL_COLS);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  maxScroll = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxScroll) msgScroll = maxScroll;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(PX + 4, y + i * LH);
    spr.print(disp[row]);
  }

  // Scrollbar on the right edge when there's history to scroll through
  if (maxScroll > 0) {
    int trackY = y, trackH = bottom - y;
    int thumbH = trackH * SHOW / nDisp; if (thumbH < 8) thumbH = 8;
    int thumbY = trackY + (trackH - thumbH) * (maxScroll - msgScroll) / maxScroll;
    spr.drawFastVLine(W - 3, trackY, trackH, PANEL);
    spr.fillRect(W - 4, thumbY, 3, thumbH, msgScroll ? p.body : p.textDim);
  }
}

// ──────────────── approval ────────────────
static void drawApproval(const Palette& p) {
  spr.fillRect(PX, PY, PW, BTN_Y - PY, p.bg);
  spr.fillRect(0, BTN_Y, W, H - BTN_Y, p.bg);
  int y = PY + 6;

  spr.setTextSize(1);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(PX + 6, y); spr.print("approve?");
  spr.setTextColor(waited >= 10 ? HOT : p.textDim, p.bg);
  spr.setCursor(W - 40, y); spr.printf("%4lus", (unsigned long)waited);
  y += 14;

  // Size 2 when it fits one line (~13 chars at 12px on the 170px panel)
  int toolLen = strlen(tama.promptTool);
  bool big = toolLen <= 13;
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(big ? 2 : 1);
  spr.setCursor(PX + 6, y);
  spr.print(tama.promptTool);
  spr.setTextSize(1);
  y += big ? 24 : 14;

  static char hint[6][WRAP_MAX + 1];
  uint8_t n = wrapInto(tama.promptHint, hint, 6, PANEL_COLS);
  spr.setTextColor(p.textDim, p.bg);
  for (uint8_t i = 0; i < n; i++) {
    spr.setCursor(PX + 6, y + i * LH); spr.print(hint[i]);
  }
  if (tama.sessionsWaiting > 1) {
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(PX + 6, BTN_Y - 14);
    spr.printf("+%u more waiting", tama.sessionsWaiting - 1);
  }

  const int bh = H - BTN_Y - 6, bw = (W - 18) / 2;
  spr.setTextDatum(middle_center);
  spr.setTextSize(2);
  if (responseSent) {
    spr.fillRoundRect(6, BTN_Y + 2, W - 12, bh, 10, PANEL);
    spr.setTextColor(p.textDim, PANEL);
    spr.drawString("sent...", W / 2, BTN_Y + 2 + bh / 2);
  } else {
    bool armed = millis() - promptArrivedMs >= PROMPT_ARM_MS;
    uint16_t dBg = armed ? DENY_BG : PANEL, aBg = armed ? OK_BG : PANEL;
    spr.fillRoundRect(6, BTN_Y + 2, bw, bh, 10, dBg);
    spr.drawRoundRect(6, BTN_Y + 2, bw, bh, 10, HOT);
    spr.setTextColor(C_WHITE, dBg);
    spr.drawString("DENY", 6 + bw / 2, BTN_Y + 2 + bh / 2);
    spr.fillRoundRect(12 + bw, BTN_Y + 2, bw, bh, 10, aBg);
    spr.drawRoundRect(12 + bw, BTN_Y + 2, bw, bh, 10, C_GREEN);
    spr.setTextColor(C_WHITE, aBg);
    spr.drawString("APPROVE", 12 + bw + bw / 2, BTN_Y + 2 + bh / 2);
  }
  spr.setTextDatum(top_left);
}

static void sendDecision(bool approve) {
  char cmd[96];
  snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"%s\"}",
           tama.promptId, approve ? "once" : "deny");
  sendCmd(cmd);
  responseSent = true;
  if (approve) {
    uint32_t tookS = (millis() - promptArrivedMs) / 1000;
    statsOnApproval(tookS);
    beep(2400, 60);
    if (tookS < 5) triggerOneShot(P_HEART, 2000);
  } else {
    statsOnDenial();
    beep(600, 60);
  }
}

// ──────────────── pet screen ────────────────
static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  if (filled) {
    spr.fillCircle(x - 3, y, 3, col);
    spr.fillCircle(x + 3, y, 3, col);
    spr.fillTriangle(x - 6, y + 1, x + 6, y + 1, x, y + 7, col);
  } else {
    spr.drawCircle(x - 3, y, 3, col);
    spr.drawCircle(x + 3, y, 3, col);
    spr.drawLine(x - 6, y + 1, x, y + 7, col);
    spr.drawLine(x + 6, y + 1, x, y + 7, col);
  }
}

static void drawPetStats(const Palette& p, int y) {
  const int X = PX + 6;
  spr.setTextSize(1);
  y += 4;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(X, y); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? C_RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(X + 56 + i * 20, y + 2, i < mood, moodCol);

  y += 18;
  spr.setCursor(X, y); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = X + 50 + i * 11;
    if (i < fed) spr.fillCircle(px, y + 3, 3, p.body);
    else spr.drawCircle(px, y + 3, 3, p.textDim);
  }

  y += 18;
  spr.setCursor(X, y); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? C_CYAN : (en >= 2) ? C_YEL : HOT;
  for (int i = 0; i < 5; i++) {
    int px = X + 50 + i * 20;
    if (i < en) spr.fillRect(px, y, 16, 8, enCol);
    else spr.drawRect(px, y, 16, 8, p.textDim);
  }

  y += 20;
  spr.fillRoundRect(X, y, 52, 18, 4, p.body);
  spr.setTextSize(2);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(X + 4, y + 2); spr.printf("Lv%u", stats().level);
  spr.setTextSize(1);

  y += 26;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(X, y);          spr.printf("approved %u", stats().approvals);
  spr.setCursor(X, y + LH);     spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(X, y + LH * 2); spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(X, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + LH * 3);
  tokFmt("today    ", tama.tokensToday, y + LH * 4);
}

static void drawPetHowTo(const Palette& p, int y) {
  spr.setTextSize(1);
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(PX + 6, y); spr.print(s); y += 9;
  };
  auto gap = [&]() { y += 4; };

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens = level up");
  ln(p.textDim, " + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " lay face-down to nap");
  ln(p.textDim, " refills to full"); gap();

  ln(p.body,    "LOVE");
  ln(p.textDim, " tap the pet to pet it"); gap();

  ln(p.textDim, "idle 30s on battery = off");
  ln(p.textDim, "touch anywhere = wake");
}

static void drawPet(const Palette& p) {
  panelClear(p);
  int y = PY + 5;
  panelHeader(p, y, petPage == 0 ? "stats" : "how to care", nullptr, petPage, PET_PAGES);
  if (petPage == 0) drawPetStats(p, y);
  else drawPetHowTo(p, y + 2);
}

// ──────────────── info screen ────────────────
static void drawInfo(const Palette& p) {
  panelClear(p);
  spr.setTextSize(1);
  int y = PY + 5;
  const int X = PX + 6;
  auto ln = [&](const char* fmt, ...) {
    char b[40]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(X, y); spr.print(b); y += 9;
  };
  static const char* const SECTIONS[] = { "ABOUT", "CONTROLS", "CLAUDE", "DEVICE", "BLUETOOTH", "CREDITS" };
  panelHeader(p, y, "info", SECTIONS[infoPage], infoPage, INFO_PAGES);

  if (infoPage == 0) {
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += 5;
    ln("I sleep when nothing's");
    ln("happening, wake when you");
    ln("start working, and get");
    ln("impatient when approvals");
    ln("pile up.");
    y += 5;
    spr.setTextColor(p.text, p.bg);
    ln("Tap APPROVE on a prompt");
    ln("to approve from here.");
    y += 5;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. menu >");
    ln("settings > pet to cycle.");

  } else if (infoPage == 1) {
    auto row = [&](const char* k, const char* v) {
      spr.setTextColor(p.text, p.bg);    spr.setCursor(X, y);      spr.print(k);
      spr.setTextColor(p.textDim, p.bg); spr.setCursor(X + 60, y); spr.print(v);
      y += 11;
    };
    row("tabs",      "switch screen");
    row("drag chat", "scroll history");
    row("tap chat",  "jump to newest");
    row("tap/swipe", "next page");
    row("tap pet",   "pet it <3");
    row("prompt",    "deny / approve");
    row("power btn", "screen on/off");
    row("hold pwr",  "power off");
    row("shake",     "dizzy");
    row("face-down", "nap");

  } else if (infoPage == 2) {
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 6;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    bool full = _onUsb && !_charging && _batPct >= 95;
    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(2);
    spr.setCursor(X, y);
    spr.printf("%d%%", _batPct);
    spr.setTextSize(1);
    spr.setTextColor(full ? C_GREEN : (_charging ? HOT : p.textDim), p.bg);
    spr.setCursor(X + 56, y + 4);
    spr.print(full ? "full" : (_charging ? "charging" : (_onUsb ? "usb" : "battery")));
    y += 20;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", _batMv/1000, (_batMv%1000)/10);
    ln("  current  %+dmA", _batMa);
    if (_onUsb && _vbusMv > 0) ln("  usb in   %d.%02dV", _vbusMv/1000, (_vbusMv%1000)/10);
    y += 5;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %.15s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  psram    %uKB", ESP.getFreePsram() / 1024);
    ln("  bright   %u/4", brightLevel);
    ln("  temp     %dC", (int)temperatureRead());

  } else if (infoPage == 4) {
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? C_GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(2);
    spr.setCursor(X, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(1);
    y += 20;

    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += 6;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Open Hardware Buddy");
      y += 4;
      ln(" auto-connects via BLE");
    }

  } else {
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += 3;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += 3;
    spr.setTextColor(p.text, p.bg);
    ln("github.com/anthropics");
    ln("/claude-desktop-buddy");
    y += 8;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += 3;
    ln("M5Stack CoreS3");
    ln("ESP32-S3 + AXP2101");
  }
}

static void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextDatum(middle_center);
  spr.setTextSize(2);
  spr.setTextColor(p.textDim, p.bg);
  spr.drawString("BLUETOOTH PAIRING", W / 2, 50);
  spr.setTextSize(1);
  spr.drawString("enter this code on your desktop", W / 2, 180);
  spr.setTextSize(5);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  spr.drawString(b, W / 2, 115);
  spr.setTextDatum(top_left);
}

// Shown in the pet panel when GIF mode is selected but nothing is loaded
// (e.g. mid-transfer, after characterClose()).
static void drawNoCharacter() {
  const Palette& p = characterPalette();
  petSpr.fillSprite(p.bg);
  petSpr.setTextSize(1);
  petSpr.setTextColor(p.textDim, p.bg);
  petSpr.setTextDatum(middle_center);
  petSpr.drawString(xferActive() ? "installing..." : "no character", PET_W / 2, PET_H / 2);
  petSpr.setTextDatum(top_left);
}

// ──────────────── touch ────────────────
static void flipPage(int dir) {
  if (displayMode == DISP_INFO) infoPage = (infoPage + INFO_PAGES + dir) % INFO_PAGES;
  else if (displayMode == DISP_PET) petPage = (petPage + PET_PAGES + dir) % PET_PAGES;
}

static bool inPanel(int x, int y) { return x >= PX && y >= PY && y < TAB_Y; }

static void onTap(int bx, int by, int x, int y) {
  bool approvalUp = tama.promptId[0] && !menuOpen && !settingsOpen && !resetOpen;
  if (blePasskey()) return;

  if (approvalUp) {
    // Modal until answered. Press and release must land on the same
    // button, after the arm delay.
    if (responseSent || millis() - promptArrivedMs < PROMPT_ARM_MS) return;
    if (by < BTN_Y || y < BTN_Y) return;
    bool left = bx < W / 2;
    if (left != (x < W / 2)) return;
    sendDecision(!left);
    return;
  }

  if (resetOpen) {
    if (overlayHeaderHit(bx, by)) { beep(1800, 30); resetOpen = false; return; }
    int i = tileHit(bx, by, RESET_N, RESET_TH);
    if (i >= 0) { beep(2400, 30); applyReset(i); }
    return;
  }
  if (settingsOpen) {
    if (overlayHeaderHit(bx, by)) { beep(1800, 30); settingsOpen = false; return; }
    int i = tileHit(bx, by, SETTINGS_N, SETTINGS_TH);
    if (i >= 0) { beep(2400, 30); applySetting(i); }
    return;
  }
  if (menuOpen) {
    if (overlayHeaderHit(bx, by)) { beep(1800, 30); menuOpen = false; return; }
    int i = tileHit(bx, by, MENU_N, MENU_TH);
    if (i >= 0) { beep(2400, 30); menuConfirm(i); }
    return;
  }

  if (by >= TAB_Y) {
    uint8_t tab = bx / (W / TAB_N);
    if (tab >= TAB_N) tab = TAB_N - 1;
    beep(1800, 30);
    if (tab == 3) { menuOpen = true; return; }
    if (tab == displayMode) { flipPage(1); return; }   // re-tap = next page
    displayMode = tab;
    return;
  }

  if (bx < PET_W && by >= PY) {
    // Petting. Doesn't interrupt another one-shot (dizzy, celebrate).
    if ((int32_t)(millis() - oneShotUntil) >= 0) {
      triggerOneShot(P_HEART, 2000);
      beep(2600, 25);
    }
    return;
  }

  if (inPanel(bx, by)) {
    if (displayMode == DISP_NORMAL) { if (msgScroll) { msgScroll = 0; beep(1800, 30); } }
    else { beep(2400, 30); flipPage(1); }
  }
}

static void handleTouch() {
  static int16_t dragAccum = 0;
  if (!M5.Touch.getCount()) return;
  const auto& t = M5.Touch.getDetail(0);

  bool touching = t.isPressed();
  if (touching) {
    // First touch while dark or napping only wakes; swallow its whole cycle.
    if (screenOff || napping) swallowTouch = true;
    wake();
  }
  if (swallowTouch) { if (!touching) swallowTouch = false; return; }

  bool moving = t.state & m5::touch_state_t::mask_moving;
  if (touching && moving) {
    // Vertical drag on the chat panel scrolls the transcript. Finger down
    // pulls older lines into view.
    bool chat = displayMode == DISP_NORMAL && !menuOpen && !settingsOpen && !resetOpen
             && !tama.promptId[0] && inPanel(t.base_x, t.base_y);
    if (chat) {
      dragAccum += t.deltaY();
      while (dragAccum >= LH)  { if (msgScroll < maxScroll) msgScroll++; dragAccum -= LH; }
      while (dragAccum <= -LH) { if (msgScroll > 0) msgScroll--;         dragAccum += LH; }
    }
    return;
  }
  if (moving) {   // flick_end / drag_end
    dragAccum = 0;
    int dx = t.distanceX(), dy = t.distanceY();
    if (abs(dx) > 40 && abs(dx) > abs(dy) * 2 && inPanel(t.base_x, t.base_y)
        && !menuOpen && !settingsOpen && !resetOpen && !tama.promptId[0]) {
      beep(2400, 30);
      flipPage(dx < 0 ? 1 : -1);
    }
    return;
  }
  if (t.state == m5::touch_state_t::touch_end || t.state == m5::touch_state_t::hold_end) {
    onTap(t.base_x, t.base_y, t.x, t.y);
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  cfg.output_power = false;    // don't run the 5V boost for the bus/Grove
  M5.begin(cfg);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  // Don't stall the UI loop on Serial writes when no USB host is reading.
  Serial.setTxTimeoutMs(0);
#endif
  M5.Speaker.setVolume(96);
  startBt();
  ledInit();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
  petNameLoad();
  buddyInit();
  applyRotation();
  applyBrightness();

  // BLE stays always-on; s.bt is stored as a preference only.
  spr.setColorDepth(16);
  spr.setPsram(true);
  if (!spr.createSprite(W, H)) {
    // No PSRAM? Fall back to an 8-bit sprite that fits in internal RAM.
    Serial.println("[ui] PSRAM sprite failed, using 8-bit");
    spr.setPsram(false);
    spr.setColorDepth(8);
    spr.createSprite(W, H);
  }
  petSpr.setColorDepth(16);
  petSpr.setPsram(false);
  petSpr.createSprite(PET_W, PET_H);
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
  refreshSensors();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(middle_center);
    spr.setTextSize(3);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - 18);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + 18);
    } else {
      // First boot, no owner pushed yet — say hi.
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - 14);
      spr.setTextSize(2);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + 18);
    }
    spr.setTextDatum(top_left); spr.setTextSize(1);
    spr.pushSprite(0, 0);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5.update();
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // Attention LED: pulse the AXP2101 CHGLED; also flashes a screen border
  // (drawn below) since the LED is small and on the side.
  bool alert = activeState == P_ATTENTION && settings().led;
  bool alertOn = alert && (now / 400) % 2;
  ledSet(alert ? (alertOn ? 1 : 0) : -1);

  // shake → dizzy
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // Prompt arrival: beep, reset response flag, jump to the approval view.
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
    }
  }

  inPrompt = tama.promptId[0] && !responseSent;

  handleTouch();

  // Power button (AXP2101 PEK): click toggles screen off. A long hold
  // powers the device off in PMIC hardware.
  if (M5.BtnPWR.wasClicked()) {
    if (screenOff) wake();
    else screenSleep();
  }

  // Time-of-day moods when the device is docked on USB and nothing is
  // happening — the desk-clock mode from the stick, minus the rotation.
  refreshSensors();   // 1Hz internal throttle
  bool deskIdle = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !tama.promptId[0]
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb
               && (int32_t)(now - oneShotUntil) >= 0;
  if (deskIdle) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clk.time.hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;

  // Render at ~30fps. A full 320×240 push is ~20ms over SPI, and pets only
  // animate at 5–10fps, so there's nothing to gain from going faster.
  static uint32_t lastFrame = 0;
  if (!napping && !screenOff && now - lastFrame >= 33) {
    lastFrame = now;

    if (buddyMode) {
      buddyTick(activeState);
    } else if (characterLoaded()) {
      characterSetState(activeState);
      characterTick();
    } else {
      drawNoCharacter();
    }

    const Palette& p = characterPalette();
    if (blePasskey()) {
      drawPasskey();
    } else {
      drawStatusBar(p);
      if (resetOpen)          drawReset();
      else if (settingsOpen)  drawSettings();
      else if (menuOpen)      drawMenu();
      else {
        petSpr.pushSprite(0, PY);
        if (tama.promptId[0]) drawApproval(p);
        else {
          if (displayMode == DISP_INFO)     drawInfo(p);
          else if (displayMode == DISP_PET) drawPet(p);
          else                              drawChat(p);
          drawTabs(p);
        }
      }
      if (alertOn) {
        spr.drawRect(0, 0, W, H, HOT);
        spr.drawRect(1, 1, W - 2, H - 2, HOT);
      }
    }
    spr.pushSprite(0, 0);
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    M5.Display.setBrightness(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a finger is held → flicker.
  // No auto-off on USB power — the desk clock wants to stay visible.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    screenSleep();
  }

  delay(screenOff ? 50 : 8);
}
