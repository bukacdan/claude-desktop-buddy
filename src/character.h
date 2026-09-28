#pragma once
#include <stdint.h>

struct Palette {
  uint16_t body, bg, text, textDim, ink;
};

// Call after M5.begin() and petSpr.createSprite(). Mounts LittleFS, reads
// /characters/<name>/manifest.json, parses colors, caches GIF paths.
bool characterInit(const char* name);
bool characterLoaded();

// 0..6: sleep, idle, busy, attention, celebrate, dizzy, heart.
// Closes current GIF, opens the one for this state. No-op if same state.
void characterSetState(uint8_t state);

// Advances timing; if it's time for the next frame, decodes it into the
// pet canvas. Call every loop iteration. Does nothing if not loaded.
void characterTick();
void characterInvalidate();   // full clear + reopen current state
void characterClose();        // close GIF + clear loaded flag; FS stays mounted

const Palette& characterPalette();
