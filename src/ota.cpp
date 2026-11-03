// Firmware updates over Wi-Fi.
//   * the setup page uploads firmware.bin (POST /api/ota, see webserver.cpp), and
//   * PlatformIO / espota can push it (`pio run -t upload --upload-protocol espota ...`) through ArduinoOTA.
// Both are off until an update password has been set and updates are switched on (settings.cpp enforces that), and both need that password.
// The flash has two firmware slots (see the partition table): the new image is written into the one that is not running and is only
// chosen for the next boot when the whole file has arrived and passed its checks, so a failed or interrupted update changes nothing.
// A new firmware is "pending" until it has run for kStableMs; if it crashes before that, the bootloader goes back to the old one.
#include <ArduinoOTA.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "app.h"
#include "core.h"

static const uint32_t kStableMs = 45000;       // a new firmware that has run this long (loop alive, settings loaded) is kept
static const uint8_t kMaxFails = 5;            // wrong passwords in a row ...
static const uint32_t kLockMs = 60000;         // ... and how long everything is refused afterwards

static volatile bool gBusy = false;            // an update is being written
static uint8_t gFails = 0;
static uint32_t gLockUntil = 0;

bool otaBusy() { return gBusy; }

bool otaPasswordOk(const String& given, const String& stored, bool* locked) {
  Lock l;
  const uint32_t now = millis();
  if (locked) *locked = false;
  if (gLockUntil && (int32_t)(gLockUntil - now) > 0) { if (locked) *locked = true; return false; }
  gLockUntil = 0;
  if (stored.length() > 0 && fwb::secureEquals(given.c_str(), stored.c_str())) { gFails = 0; return true; }
  if (++gFails >= kMaxFails) {
    gFails = 0;
    gLockUntil = now + kLockMs;
    if (!gLockUntil) gLockUntil = 1;
    if (locked) *locked = true;
  }
  return false;
}

// ---------------------------------------------------------------- rollback
// Arduino's startup normally marks the running image as good at once. Overriding this keeps a new image "pending" until otaLoop() has seen it
// run for a while; resetting before that makes the bootloader return to the previous firmware.
extern "C" bool verifyRollbackLater() { return true; }

// While the running firmware is still "pending" a new update is refused. The bootloader can only fall back to a firmware that was confirmed
// good: updating again within the first minute leaves it nothing to go back to, and a new firmware that crashes would then restart forever
// (found by testing exactly that on the board; the only way out was USB).
static bool gMarked = false, gPending = false;
static void markGoodWhenStable() {
  if (gMarked || millis() < kStableMs) return;
  gMarked = true;
  if (gPending) {
    esp_ota_mark_app_valid_cancel_rollback();
    gPending = false;
    Serial.println("New firmware confirmed.");
  }
}

static String notConfirmedYet() {
  const uint32_t up = millis();
  return "The firmware that is running has not been confirmed yet (a new firmware is checked for about a minute after it starts, so that a bad one can be undone). Try again in " +
         String(up < kStableMs ? (unsigned)((kStableMs - up) / 1000 + 1) : 1u) + " seconds.";
}

static const char* imageStateName(const esp_partition_t* p) {
  esp_ota_img_states_t st;
  if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK) return "unknown";
  switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
  }
}

void otaInfo(OtaInfo& o) {
  { Lock l; o.enabled = gSet.otaOn && gSet.otaPass.length() > 0; o.passSet = gSet.otaPass.length() > 0; }
  o.version = FWB_VERSION;
  const esp_partition_t* run = esp_ota_get_running_partition();
  o.slot = run ? String(run->label) : String("?");
  o.imageState = imageStateName(run);
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  o.maxBytes = next ? next->size : 0;
  o.rolledBack = esp_ota_get_last_invalid_partition() != nullptr;        // a firmware was refused by the bootloader after it failed to start
}

// ---------------------------------------------------------------- web upload
static fwb::MarkScanner gScan;
static bool gUpOpen = false;

String otaUploadBegin(size_t total, const uint8_t* head, size_t headLen) {
  if (gBusy) return "An update is already running.";
  if (gPending) return notConfirmedYet();
  const fwb::ImageCheck c =fwb::checkAppImage(head, headLen, fwb::kChipEsp32S3);
  if (c != fwb::kImageOk) return fwb::imageCheckMessage(c);
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!next) return "This board has no second firmware slot to write to.";
  if (total > next->size) return "That file is larger than the firmware slot (" + String((unsigned)(next->size / 1024)) + " KB).";
  if (!Update.begin(total, U_FLASH)) return String("The update could not start: ") + Update.errorString();
  gScan = fwb::MarkScanner();
  gUpOpen = true;
  gBusy = true;
  relayUpdateBegin();                                                    // hand the WLEDs back and stop streaming while flash is written
  return "";
}

String otaUploadWrite(const uint8_t* data, size_t n) {
  if (!gUpOpen) return "No update is open.";
  fwb::markScan(gScan, data, n);
  if (Update.write(const_cast<uint8_t*>(data), n) != n) return String("Writing to flash failed: ") + Update.errorString();
  return "";
}

String otaUploadEnd() {
  if (!gUpOpen) return "No update is open.";
  if (!gScan.found) return "That file is not a Fancy2WLED firmware (it is for something else). Nothing was changed.";
  if (!Update.end()) return String("The update could not be finished: ") + Update.errorString();
  gUpOpen = false;                                                       // the next boot runs the new firmware (gBusy stays set until then)
  return "";
}

void otaUploadAbort() {
  if (gUpOpen) Update.abort();
  gUpOpen = false;
  gBusy = false;
  relayUpdateEnd();
}

// ---------------------------------------------------------------- PlatformIO / espota
static bool gAoWanted = false, gAoRunning = false;
static String gAoPass;
static uint32_t gOtaGen = 0xFFFFFFFF;

static void startArduinoOta() {
  ArduinoOTA.setHostname(FWB_HOSTNAME);
  ArduinoOTA.setMdnsEnabled(false);                                      // wifi_mgr.cpp already runs mDNS
  ArduinoOTA.setPassword(gAoPass.c_str());
  ArduinoOTA.onStart([]() { gBusy = true; relayUpdateBegin(); relayLoop(); });   // one relay step: the WLEDs are handed back before handle() blocks
  ArduinoOTA.onEnd([]() {});                                             // restarts by itself
  ArduinoOTA.onError([](ota_error_t) { gBusy = false; relayUpdateEnd(); });
  ArduinoOTA.begin();
  gAoRunning = true;
}

void otaBegin() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  gPending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
  Serial.printf("Firmware slot %s, image state %s\n", run ? run->label : "?", imageStateName(run));
}

void otaLoop() {
  markGoodWhenStable();
  if (gOtaGen != gSettingsGen) {                                         // settings changed: should the espota listener run, with which password?
    gOtaGen = gSettingsGen;
    String pass;
    bool want;
    { Lock l; want = gSet.otaOn && gSet.otaPass.length() > 0; pass = gSet.otaPass; }
    if (gAoRunning && (!want || pass != gAoPass)) { ArduinoOTA.end(); gAoRunning = false; }
    gAoWanted = want;
    gAoPass = pass;
  }
  const bool up = !wifiIsAp() && WiFi.status() == WL_CONNECTED;
  if (gAoRunning && !up) { ArduinoOTA.end(); gAoRunning = false; }
  if (gAoWanted && up && !gAoRunning && !gPending) startArduinoOta();         // not while the running firmware is unconfirmed (see above)
  if (gAoRunning) ArduinoOTA.handle();
}
