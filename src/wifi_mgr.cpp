// Wi-Fi: join the saved network, or open a setup access point ("Fancy2WLED-Setup") with a captive portal.
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "app.h"

static bool gAp = false;
static DNSServer gDns;
static uint32_t gRestartAt = 0;
static uint32_t gDownSince = 0;
static const char* kHostname = FWB_HOSTNAME;

bool wifiIsAp() { return gAp; }
String wifiSsid() { return WiFi.SSID(); }

void requestRestart(uint32_t inMs) { gRestartAt = millis() + inMs; if (!gRestartAt) gRestartAt = 1; }
bool restartDue() { return gRestartAt && (int32_t)(millis() - gRestartAt) >= 0; }

String deviceId() {
  char buf[13];
  snprintf(buf, sizeof buf, "%012llX", (unsigned long long)(ESP.getEfuseMac() & 0xFFFFFFFFFFFFULL));
  return String(buf);
}

static void startMdns() {
  if (MDNS.begin(kHostname)) {
    MDNS.addService("http", "tcp", 80);
    MDNS.addService("https", "tcp", 443);
    MDNS.addService("fancy2wled", "tcp", 80);                       // lets Home Assistant find the board (when mDNS reaches it)
    MDNS.addServiceTxt("fancy2wled", "tcp", "id", deviceId());
    MDNS.addServiceTxt("fancy2wled", "tcp", "ver", FWB_VERSION);
  }
}

static void startAp() {
  WiFi.mode(WIFI_AP_STA);                     // STA stays on so the setup page can scan for networks
  IPAddress apIp(FWB_AP_IP);
  WiFi.softAPConfig(apIp, apIp, IPAddress(255, 255, 255, 0));   // must come before softAP()
  WiFi.softAP(FWB_AP_SSID);             // open network: it only exists during setup
  gDns.start(53, "*", WiFi.softAPIP());       // captive portal: every name resolves to us
  gAp = true;
  Serial.printf("Setup access point '%s' on %s\n", FWB_AP_SSID, WiFi.softAPIP().toString().c_str());
}

void wifiBegin() {
  WiFi.persistent(false);
  WiFi.setHostname(kHostname);
  String ssid, pass;
  { Lock l; ssid = gSet.ssid; pass = gSet.pass; }
  if (ssid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);                     // needed to catch every broadcast frame
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
      delay(100);
      ledStatusLoop();
    }
    if (WiFi.status() == WL_CONNECTED) {
      gAp = false;
      startMdns();
      Serial.printf("Wi-Fi connected: %s  (http://%s.local)\n", WiFi.localIP().toString().c_str(), kHostname);
      return;
    }
    Serial.println("Wi-Fi: could not join the saved network; starting the setup access point");
  }
  startAp();
}

void wifiLoop() {
  if (gAp) {
    gDns.processNextRequest();
    // If the saved network comes back (e.g. the router was still booting), restart into normal mode.
    bool haveSaved; { Lock l; haveSaved = gSet.ssid.length() > 0; }
    if (haveSaved && WiFi.status() == WL_CONNECTED && !gRestartAt) requestRestart(1000);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (!gDownSince) gDownSince = millis();
    if (millis() - gDownSince > 30000) { WiFi.reconnect(); gDownSince = millis(); }
  } else {
    gDownSince = 0;
  }
}

String wifiScanJson() {
  JsonDocument d;
  JsonArray a = d["networks"].to<JsonArray>();
  int n = WiFi.scanNetworks(false, false);
  for (int i = 0; i < n && a.size() < 20; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;
    bool dup = false;
    for (JsonVariant v : a) if (s == v.as<String>()) dup = true;
    if (!dup) a.add(s);
  }
  WiFi.scanDelete();
  String out;
  serializeJson(d, out);
  return out;
}

bool wifiSave(const String& ssid, const String& pass) {
  if (ssid.length() == 0 || ssid.length() > 32 || pass.length() > 63) return false;
  { Lock l; gSet.ssid = ssid; gSet.pass = pass; settingsSave(); }
  requestRestart(1200);
  return true;
}
