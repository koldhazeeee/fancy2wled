// Settings: stored in flash (NVS), edited through the web UI.
#include <Preferences.h>

#include "app.h"

Settings gSet;
fwb::Entry gTable[fwb::kMaxLeds];
int gTableN = 0;
volatile uint32_t gSettingsGen = 0;
volatile uint32_t gMqttGen = 0;
SemaphoreHandle_t gLock = nullptr;

static uint8_t gStored[fwb::kMaxLeds * 4];   // calibrated table as saved by the phone (a, b, f, d per LED)
static int gStoredN = 0;
static Preferences prefs;

static const char* kNs = "fwb";

bool tableHasStored() { return gStoredN > 0; }

static bool gPreview = false;     // settings in RAM differ from flash (live preview in the web UI)

static void loadFromFlash() {
  Settings& s = gSet;
  auto gS = [&](const char* k, const char* d) { return prefs.isKey(k) ? prefs.getString(k, d) : String(d); };
  auto gI = [&](const char* k, int d) { return prefs.isKey(k) ? (int)prefs.getInt(k, d) : d; };
  auto gB = [&](const char* k, bool d) { return prefs.isKey(k) ? prefs.getBool(k, d) : d; };
  auto gF = [&](const char* k, float d) { return prefs.isKey(k) ? prefs.getFloat(k, d) : d; };
  s.wHost = gS("wHost", "");
  s.wStart = gI("wStart", 0);
  s.wCount = gI("wCount", 0);
  s.wTurnOn = gB("wOn", true);
  s.wRestore = gB("wRestore", true);
  s.wBri = gI("wBri", -1);
  s.whiteMin = gB("whiteMin", false);
  s.lType = gI("lType", LAYOUT_LINEAR);
  s.led0Right = gB("lRight", false);
  s.linEdge = gI("lEdge", 0);
  if (s.linEdge < 0 || s.linEdge > 3) s.linEdge = 0;
  s.spanFirst = gI("lSf", -1);
  s.spanLast = gI("lSl", -1);
  s.corner = gI("lCorner", 0);
  s.ccw = gB("lCcw", false);
  for (int i = 0; i < 4; i++) s.sides[i] = gI(("lS" + String(i)).c_str(), 0);
  s.shift = gI("lShift", 0);
  s.kBri = gF("kBri", 1.f);
  s.kGam = gF("kGam", 1.f);
  s.kSat = gF("kSat", 1.f);
  s.kSmo = gF("kSmo", 0.5f);
  s.enabled = gB("enabled", true);
  s.mqttOn = gB("mqOn", false);
  s.mqttHost = gS("mqHost", "");
  s.mqttPort = gI("mqPort", 1883);
  s.mqttUser = gS("mqUser", "");
  s.mqttPass = gS("mqPass", "");
  s.mqttBase = gS("mqBase", "");
  s.mqttDisc = gS("mqDisc", "homeassistant");
  s.smoothOut = gI("kOut", 100);
  s.fps = gI("fps", 30);
  s.boxHost = gS("boxHost", "");
  s.ssid = gS("ssid", "");
  s.pass = gS("pass", "");
  size_t n = prefs.isKey("tbl4") ? prefs.getBytesLength("tbl4") : 0;
  if (n > 0 && n % 4 == 0 && n <= sizeof gStored) {
    prefs.getBytes("tbl4", gStored, n);
    gStoredN = (int)(n / 4);
  }
  if (s.lType == LAYOUT_TABLE && gStoredN == 0) s.lType = LAYOUT_LINEAR;
}

void settingsBegin() {
  gLock = xSemaphoreCreateRecursiveMutex();
  prefs.begin(kNs, false);
  loadFromFlash();
  tableRebuild();
}

// Throw away unsaved live-preview changes (keeps the Wi-Fi settings, which are never previewed).
void settingsRevert() {
  Lock l;
  if (!gPreview) return;
  gPreview = false;
  loadFromFlash();
  tableRebuild();
}

void settingsSave() {
  Lock l;
  const Settings& s = gSet;
  prefs.putString("wHost", s.wHost);
  prefs.putInt("wStart", s.wStart);
  prefs.putInt("wCount", s.wCount);
  prefs.putBool("wOn", s.wTurnOn);
  prefs.putBool("wRestore", s.wRestore);
  prefs.putInt("wBri", s.wBri);
  prefs.putBool("whiteMin", s.whiteMin);
  prefs.putInt("lType", s.lType);
  prefs.putBool("lRight", s.led0Right);
  prefs.putInt("lEdge", s.linEdge);
  prefs.putInt("lSf", s.spanFirst);
  prefs.putInt("lSl", s.spanLast);
  prefs.putInt("lCorner", s.corner);
  prefs.putBool("lCcw", s.ccw);
  for (int i = 0; i < 4; i++) prefs.putInt(("lS" + String(i)).c_str(), s.sides[i]);
  prefs.putInt("lShift", s.shift);
  prefs.putFloat("kBri", s.kBri);
  prefs.putFloat("kGam", s.kGam);
  prefs.putFloat("kSat", s.kSat);
  prefs.putFloat("kSmo", s.kSmo);
  prefs.putBool("enabled", s.enabled);
  prefs.putBool("mqOn", s.mqttOn);
  prefs.putString("mqHost", s.mqttHost);
  prefs.putInt("mqPort", s.mqttPort);
  prefs.putString("mqUser", s.mqttUser);
  prefs.putString("mqPass", s.mqttPass);
  prefs.putString("mqBase", s.mqttBase);
  prefs.putString("mqDisc", s.mqttDisc);
  prefs.putInt("kOut", s.smoothOut);
  prefs.putInt("fps", s.fps);
  prefs.putString("boxHost", s.boxHost);
  prefs.putString("ssid", s.ssid);
  prefs.putString("pass", s.pass);
}

void settingsReset() {
  prefs.clear();
  prefs.end();
  Preferences tls;
  if (tls.begin("fwb_tls", false)) { tls.clear(); tls.end(); }
}

void tableRebuild() {
  Lock l;
  const Settings& s = gSet;
  int n = s.wCount;
  gTableN = 0;
  if (n <= 0 || n > fwb::kMaxLeds) return;
  if (s.lType == LAYOUT_TABLE && gStoredN == n) {
    for (int i = 0; i < n; i++) gTable[i] = fwb::Entry{gStored[i * 4], gStored[i * 4 + 1], gStored[i * 4 + 2], gStored[i * 4 + 3]};
    gTableN = n;
  } else if (s.lType == LAYOUT_LOOP) {
    const bool custom = s.sides[0] > 0;
    if (fwb::tableLoop(gTable, n, s.corner, s.ccw, custom ? s.sides : nullptr, s.shift, s.aw, s.ah)) {
      gTableN = n;
    } else {
      fwb::tableLinear(gTable, n, s.led0Right, s.spanFirst, s.spanLast, s.linEdge);
      gTableN = n;
    }
  } else {   // linear (also the fallback when a calibrated table does not match the LED count)
    fwb::tableLinear(gTable, n, s.led0Right, s.spanFirst, s.spanLast, s.linEdge);
    gTableN = n;
  }
  gSettingsGen++;
}

void settingsToJson(JsonDocument& d) {
  Lock l;
  const Settings& s = gSet;
  JsonObject w = d["wled"].to<JsonObject>();
  w["host"] = s.wHost;
  w["start"] = s.wStart;
  w["count"] = s.wCount;
  w["turnOn"] = s.wTurnOn;
  w["restore"] = s.wRestore;
  w["brightness"] = s.wBri;
  JsonObject l2 = d["layout"].to<JsonObject>();
  l2["type"] = s.lType == LAYOUT_TABLE ? "table" : (s.lType == LAYOUT_LOOP ? "loop" : "linear");
  l2["led0Right"] = s.led0Right;
  static const char* kEdgeNames[4] = {"top", "right", "bottom", "left"};
  l2["edge"] = kEdgeNames[s.linEdge < 0 || s.linEdge > 3 ? 0 : s.linEdge];
  l2["spanFirst"] = s.spanFirst;
  l2["spanLast"] = s.spanLast;
  l2["corner"] = s.corner;
  l2["ccw"] = s.ccw;
  JsonArray sides = l2["sides"].to<JsonArray>();
  for (int i = 0; i < 4; i++) sides.add(s.sides[i]);
  l2["shift"] = s.shift;
  l2["hasTable"] = gStoredN > 0;
  JsonObject k = d["look"].to<JsonObject>();
  k["brightness"] = s.kBri;
  k["gamma"] = s.kGam;
  k["saturation"] = s.kSat;
  k["smoothing"] = s.kSmo;
  k["smoothOut"] = s.smoothOut;
  d["box"]["host"] = s.boxHost;
  d["relay"]["enabled"] = s.enabled;
  JsonObject mq = d["mqtt"].to<JsonObject>();
  mq["enabled"] = s.mqttOn;
  mq["host"] = s.mqttHost;
  mq["port"] = s.mqttPort;
  mq["user"] = s.mqttUser;
  mq["passSet"] = s.mqttPass.length() > 0;        // the password itself is never sent back
  mq["base"] = s.mqttBase;
  mq["discovery"] = s.mqttDisc;
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static const char* kExportFormat = "fancy2wled-config";
static const int kExportVersion = 1;

void settingsExport(JsonDocument& d, bool secrets) {
  JsonDocument c;
  settingsToJson(c);
  d["format"] = kExportFormat;
  d["version"] = kExportVersion;
  d["firmware"] = FWB_VERSION;
  d["config"] = c;
  d["config"]["layout"].remove("hasTable");           // derived, not a setting
  d["config"]["mqtt"].remove("passSet");
  Lock l;
  if (secrets && gSet.mqttPass.length()) d["config"]["mqtt"]["pass"] = gSet.mqttPass;
  if (gStoredN > 0) {
    static const char* kHex = "0123456789abcdef";
    String hex;
    hex.reserve((size_t)gStoredN * 8);
    for (int i = 0; i < gStoredN * 4; i++) { hex += kHex[gStored[i] >> 4]; hex += kHex[gStored[i] & 15]; }
    d["table"]["start"] = gSet.wStart;
    d["table"]["count"] = gStoredN;
    d["table"]["hex"] = hex;
  }
}

String settingsImport(JsonVariantConst doc) {
  if (String(doc["format"] | "") != kExportFormat) return "This is not a Fancy2WLED config file.";
  const int ver = doc["version"] | 0;
  if (ver < 1) return "This config file has no version number.";
  if (ver > kExportVersion) return "This config file was made by a newer version of Fancy2WLED. Update this board first.";
  JsonVariantConst cfg = doc["config"];
  if (!cfg.is<JsonObjectConst>()) return "The config file has no settings in it.";
  JsonVariantConst t = doc["table"];
  if (t.isNull()) return settingsFromJson(cfg, true);

  // The calibrated table goes in first, so a "table" layout is valid when the settings arrive. Storing it also switches the
  // layout, so if anything is refused afterwards everything is put back: an import is all or nothing.
  Lock l;
  static uint8_t oldStored[fwb::kMaxLeds * 4];
  const Settings oldSet = gSet;
  const int oldN = gStoredN;
  memcpy(oldStored, gStored, (size_t)oldN * 4);
  const char* hex = t["hex"] | "";
  String e = tableStoreHex(t["start"] | -1, t["count"] | 0, hex, strlen(hex));
  if (e.length() == 0) e = settingsFromJson(cfg, true);
  else e = "The calibration in the file is not valid: " + e;
  if (e.length()) {                                    // refused: restore the table, the layout and the saved settings
    memcpy(gStored, oldStored, (size_t)oldN * 4);
    gStoredN = oldN;
    if (oldN > 0) prefs.putBytes("tbl4", gStored, (size_t)oldN * 4);
    else prefs.remove("tbl4");
    gSet = oldSet;
    tableRebuild();
    settingsSave();
    gPreview = false;
  }
  return e;
}

String settingsFromJson(JsonVariantConst d, bool persist) {
  Lock l;
  Settings n = gSet;          // work on a copy; commit only if everything is valid
  JsonVariantConst w = d["wled"], y = d["layout"], k = d["look"], b = d["box"];
  if (!w["host"].isNull()) {
    String h = w["host"].as<String>();
    h.trim();
    if (h.length() > 63) return "WLED address is too long";
    for (size_t i = 0; i < h.length(); i++) {
      char c = h[i];
      if (!(isalnum(c) || c == '.' || c == '-' || c == '_')) return "WLED address may only contain letters, digits, dots and dashes";
    }
    n.wHost = h;
  }
  if (!w["start"].isNull()) n.wStart = w["start"].as<int>();
  if (!w["count"].isNull()) n.wCount = w["count"].as<int>();
  if (!w["turnOn"].isNull()) n.wTurnOn = w["turnOn"].as<bool>();
  if (!w["restore"].isNull()) n.wRestore = w["restore"].as<bool>();
  if (!w["brightness"].isNull()) n.wBri = w["brightness"].as<int>();
  if (n.wStart < 0 || n.wStart > 20000) return "First LED must be between 0 and 20000";
  if (n.wCount < 0 || n.wCount > fwb::kMaxLeds) return "The TV run can have at most " + String(fwb::kMaxLeds) + " LEDs";
  if (n.wBri < -1 || n.wBri > 255) return "WLED brightness must be -1 (leave) or 0-255";

  if (!y["type"].isNull()) {
    String t = y["type"].as<String>();
    if (t == "linear") n.lType = LAYOUT_LINEAR;
    else if (t == "loop") n.lType = LAYOUT_LOOP;
    else if (t == "table") n.lType = LAYOUT_TABLE;
    else return "Unknown layout type";
  }
  if (!y["led0Right"].isNull()) n.led0Right = y["led0Right"].as<bool>();
  if (!y["edge"].isNull()) {
    String e = y["edge"].as<String>();
    if (e == "top") n.linEdge = 0;
    else if (e == "right") n.linEdge = 1;
    else if (e == "bottom") n.linEdge = 2;
    else if (e == "left") n.linEdge = 3;
    else return "edge must be top, right, bottom or left";
  }
  if (!y["spanFirst"].isNull()) n.spanFirst = y["spanFirst"].as<int>();
  if (!y["spanLast"].isNull()) n.spanLast = y["spanLast"].as<int>();
  if (!y["corner"].isNull()) n.corner = y["corner"].as<int>();
  if (!y["ccw"].isNull()) n.ccw = y["ccw"].as<bool>();
  if (!y["shift"].isNull()) n.shift = y["shift"].as<int>();
  if (y["sides"].is<JsonArrayConst>()) {
    JsonArrayConst a = y["sides"];
    for (int i = 0; i < 4 && i < (int)a.size(); i++) n.sides[i] = a[i].as<int>();
  }
  if (n.corner < 0 || n.corner > 3) return "Corner must be 0-3";
  if (n.spanFirst >= 0 && (n.spanLast < n.spanFirst)) return "The right-edge LED must come after the left-edge LED";
  for (int i = 0; i < 4; i++) if (n.sides[i] < 0) return "LEDs per side cannot be negative";
  if (n.sides[0] > 0 || n.sides[1] > 0 || n.sides[2] > 0 || n.sides[3] > 0) {
    if (n.sides[0] + n.sides[1] + n.sides[2] + n.sides[3] != n.wCount || n.sides[0] <= 0 || n.sides[1] <= 0 || n.sides[2] <= 0 || n.sides[3] <= 0)
      return "LEDs per side must be four numbers that add up to the number of LEDs (or leave all four blank)";
  }
  if (n.lType == LAYOUT_TABLE && (gStoredN == 0)) return "There is no calibrated layout yet. Run the phone calibration first.";
  if (n.lType == LAYOUT_TABLE && gStoredN != n.wCount) return "The calibrated layout is for " + String(gStoredN) + " LEDs. Change the count back or calibrate again.";

  if (!k["brightness"].isNull()) n.kBri = clampf(k["brightness"].as<float>(), 0.f, 2.f);
  if (!k["gamma"].isNull()) n.kGam = clampf(k["gamma"].as<float>(), 0.3f, 4.f);
  if (!k["saturation"].isNull()) n.kSat = clampf(k["saturation"].as<float>(), 0.f, 3.f);
  if (!k["smoothing"].isNull()) n.kSmo = clampf(k["smoothing"].as<float>(), 0.f, 0.95f);
  if (!k["smoothOut"].isNull()) n.smoothOut = (int)clampf(k["smoothOut"].as<float>(), 0.f, 100.f);
  if (!b["host"].isNull()) n.boxHost = b["host"].as<String>();
  if (!d["relay"]["enabled"].isNull()) n.enabled = d["relay"]["enabled"].as<bool>();
  bool mqttTouched = false;
  JsonVariantConst mq = d["mqtt"];
  if (persist && !mq.isNull()) {                       // MQTT settings are never a live preview
    mqttTouched = true;
    auto okTopic = [](const String& s) { for (size_t i = 0; i < s.length(); i++) { char c = s[i]; if (!(isalnum(c) || c == '_' || c == '-' || c == '/')) return false; } return true; };
    if (!mq["enabled"].isNull()) n.mqttOn = mq["enabled"].as<bool>();
    if (!mq["host"].isNull()) {
      String h = mq["host"].as<String>(); h.trim();
      if (h.length() > 63) return "The MQTT broker address is too long";
      for (size_t i = 0; i < h.length(); i++) { char c = h[i]; if (!(isalnum(c) || c == '.' || c == '-' || c == '_')) return "The MQTT broker address may only contain letters, digits, dots and dashes"; }
      n.mqttHost = h;
    }
    if (!mq["port"].isNull()) n.mqttPort = mq["port"].as<int>();
    if (!mq["user"].isNull()) { n.mqttUser = mq["user"].as<String>(); if (n.mqttUser.length() > 64) return "The MQTT user name is too long"; }
    if (!mq["pass"].isNull()) { n.mqttPass = mq["pass"].as<String>(); if (n.mqttPass.length() > 96) return "The MQTT password is too long"; }
    if (!mq["base"].isNull()) { n.mqttBase = mq["base"].as<String>(); n.mqttBase.trim(); if (n.mqttBase.length() > 48 || !okTopic(n.mqttBase)) return "The topic prefix may only contain letters, digits, - _ and /"; }
    if (!mq["discovery"].isNull()) { n.mqttDisc = mq["discovery"].as<String>(); n.mqttDisc.trim(); if (n.mqttDisc.length() == 0) n.mqttDisc = "homeassistant"; if (n.mqttDisc.length() > 32 || !okTopic(n.mqttDisc)) return "The discovery prefix may only contain letters, digits, - _ and /"; }
    if (n.mqttPort < 1 || n.mqttPort > 65535) return "The MQTT port must be between 1 and 65535";
    if (n.mqttOn && n.mqttHost.length() == 0) return "Enter the MQTT broker address (your Home Assistant or Mosquitto host).";
  }

  gSet = n;
  if (mqttTouched) gMqttGen++;
  tableRebuild();
  if (persist) { settingsSave(); gPreview = false; }
  else gPreview = true;                      // live preview: used right away, not written to flash until Save
  return "";
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

String tableStoreHex(int start, int count, const char* hex, size_t hexLen) {
  if (count < 1 || count > fwb::kMaxLeds) return "LED count out of range";
  if (start < 0 || start > 20000) return "First LED out of range";
  // 8 hex characters per LED (zone a, zone b, blend, distance beyond the TV); older pages sent 6 (no distance)
  const int per = hexLen == (size_t)count * 8 ? 4 : (hexLen == (size_t)count * 6 ? 3 : 0);
  if (!per) return "Table size does not match the LED count";
  static uint8_t tmp[fwb::kMaxLeds * 4];
  for (int i = 0; i < count; i++) {
    for (int k = 0; k < 4; k++) {
      uint8_t v = 0;
      if (k < per) {
        int hi = hexv(hex[(i * per + k) * 2]), lo = hexv(hex[(i * per + k) * 2 + 1]);
        if (hi < 0 || lo < 0) return "Table contains invalid characters";
        v = (uint8_t)(hi * 16 + lo);
      }
      tmp[i * 4 + k] = v;
    }
    if (tmp[i * 4] >= fwb::kZones || tmp[i * 4 + 1] >= fwb::kZones) return "Table refers to a zone that does not exist";
  }
  Lock l;
  memcpy(gStored, tmp, (size_t)count * 4);
  gStoredN = count;
  prefs.putBytes("tbl4", gStored, (size_t)count * 4);
  gSet.wStart = start;
  gSet.wCount = count;
  gSet.lType = LAYOUT_TABLE;
  tableRebuild();
  settingsSave();
  gPreview = false;
  return "";
}