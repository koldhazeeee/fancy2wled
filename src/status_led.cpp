// The AtomS3 Lite's RGB LED (GPIO35) shows what the relay is doing; the button (GPIO41) controls it.
//   purple blink  setup access point      blue blink   joining Wi-Fi
//   blue          waiting for the Sync Box   green      relaying
//   yellow        needs WLED set up          white      calibrating / test pattern
//   violet        paused (sync is off)
// Button: a short tap turns the sync on / off (same as the Pause/Resume button on the setup page and the Home Assistant
// switch); hold for 8 s to factory-reset (the LED turns red after 3 s; letting go before 8 s cancels).
#include <WiFi.h>

#include "app.h"

static const int kLedPin = 35;
static const int kButtonPin = 41;
static const uint32_t kTapMinMs = 40;        // shorter than this is contact bounce
static const uint32_t kTapMaxMs = 1000;      // longer than this is not a tap
static uint32_t gPressStart = 0;
static uint32_t gLastUpdate = 0;
static uint32_t gLastStatus = 0;
static uint32_t gFlashUntil = 0;
static uint8_t gFlash[3] = {0, 0, 0};
static RelayStatus gSt;

void ledStatusBegin() { pinMode(kButtonPin, INPUT_PULLUP); }

static uint8_t gShown[3] = {255, 255, 255};
static void show(uint8_t r, uint8_t g, uint8_t b) {        // the LED only needs telling when its colour changes
  if (r == gShown[0] && g == gShown[1] && b == gShown[2]) return;
  gShown[0] = r; gShown[1] = g; gShown[2] = b;
  neopixelWrite(kLedPin, r, g, b);
}

// Tap: flip the sync on / off (saved, so it survives a restart; Home Assistant sees the change too).
static void toggleSync() {
  bool nowOn;
  { Lock l; nowOn = !gSet.enabled; }
  JsonDocument d;
  d["relay"]["enabled"] = nowOn;
  settingsFromJson(d.as<JsonVariantConst>(), true);
  // brief confirmation: green = sync on, violet = sync off
  gFlash[0] = nowOn ? 0 : 30; gFlash[1] = nowOn ? 40 : 0; gFlash[2] = nowOn ? 0 : 40;
  gFlashUntil = millis() + 350;
}

void ledStatusLoop() {
  const uint32_t now = millis();

  // ---- button
  if (digitalRead(kButtonPin) == LOW) {
    if (!gPressStart) gPressStart = now ? now : 1;
    const uint32_t held = now - gPressStart;
    if (held > 8000) {                       // long hold: factory reset
      show(40, 0, 0);
      settingsReset();
      delay(200);
      ESP.restart();
    }
    if (held > 3000) { show((now / 150) % 2 ? 40 : 0, 0, 0); return; }   // warning: about to reset
  } else if (gPressStart) {                  // released
    const uint32_t held = now - gPressStart;
    gPressStart = 0;
    if (held >= kTapMinMs && held <= kTapMaxMs) toggleSync();
  }

  if ((int32_t)(gFlashUntil - now) > 0) { show(gFlash[0], gFlash[1], gFlash[2]); return; }

  if (now - gLastUpdate < 120) return;
  gLastUpdate = now;
  const bool blink = (now / 500) % 2;
  if (wifiIsAp()) { show(blink ? 20 : 0, 0, blink ? 24 : 0); return; }
  if (WiFi.status() != WL_CONNECTED) { show(0, 0, blink ? 20 : 2); return; }
  if (now - gLastStatus > 400) { relayStatus(gSt); gLastStatus = now; }
  const String s = gSt.state;
  if (s == "relaying") show(0, 14, 0);
  else if (s == "needs setup") show(18, 12, 0);
  else if (s == "paused") show(10, 0, 14);
  else if (s == "calibrating" || s == "showing test pattern" || s == "showing markers") show(14, 14, 14);
  else show(0, 0, 14);
}
