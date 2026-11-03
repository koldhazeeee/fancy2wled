// Fancy2WLED: Sync Box -> WLED relay for the M5Stack AtomS3 Lite (ESP32-S3).
// Receives the Sync Box's UDP broadcast, maps its 24 picture zones onto your LEDs and sends them to WLED (DDP).
#include <Arduino.h>
#include <WiFi.h>

#include "app.h"

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n" FWB_NAME " %s\n", FWB_VERSION);
  settingsBegin();
  ledStatusBegin();
  wifiBegin();
  if (!wifiIsAp()) relayBegin();
  otaBegin();
  goveeBegin();
  webBegin();
  mqttBegin();
}

void loop() {
  if (gPerfOn) perfLoopBegin();
  wifiLoop();
  otaLoop();
  if (!wifiIsAp() && WiFi.status() == WL_CONNECTED) { relayLoop(); mqttLoop(); }
  ledStatusLoop();
  if (restartDue()) {
    delay(100);
    ESP.restart();
  }
  if (gPerfOn) perfLoopEnd();
  relayWait(10);                     // sleeps until a Sync Box packet arrives or the relay has something to do (at most 10 ms)
}
