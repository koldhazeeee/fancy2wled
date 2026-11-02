// Settings: stored in flash (NVS), edited through the web UI.
//
// Each target (a run of LEDs on a WLED) has its own settings. Target 0 keeps the key names the single-target firmware used, so an
// existing board upgrades without losing anything; targets 1.. use the same names with the target number appended.
#include <Preferences.h>

#include "app.h"

Settings gSet;
fwb::Entry* gTable[kMaxTargets] = {};
int gTableN[kMaxTargets] = {};
static int gTableCap[kMaxTargets] = {};
volatile uint32_t gSettingsGen = 0;
volatile uint32_t gMqttGen = 0;
SemaphoreHandle_t gLock = nullptr;

static uint8_t* gStored[kMaxTargets] = {};   // calibrated table of each target as saved by the phone (a, b, f, d per LED)
static int gStoredN[kMaxTargets] = {};
static Preferences prefs;

static const char* kNs = "fwb";

bool tableHasStored(int t) { return t >= 0 && t < kMaxTargets && gStoredN[t] > 0; }

static bool gPreview = false;     // settings in RAM differ from flash (live preview in the web UI)

static String K(const char* base, int i) { return i == 0 ? String(base) : String(base) + String(i); }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------------------------------------------- flash
static void loadTarget(Target& t, int i) {
  auto gS = [&](const char* k, const char* d) { const String key = K(k, i); return prefs.isKey(key.c_str()) ? prefs.getString(key.c_str(), d) : String(d); };
  auto gI = [&](const char* k, int d) { const String key = K(k, i); return prefs.isKey(key.c_str()) ? (int)prefs.getInt(key.c_str(), d) : d; };
  auto gB = [&](const char* k, bool d) { const String key = K(k, i); return prefs.isKey(key.c_str()) ? prefs.getBool(key.c_str(), d) : d; };
  auto gF = [&](const char* k, float d) { const String key = K(k, i); return prefs.isKey(key.c_str()) ? prefs.getFloat(key.c_str(), d) : d; };
  t = Target();
  t.name = gS("tName", ("Target " + String(i + 1)).c_str());
  t.enabled = gB("tOn", true);
  t.trim = gF("tTrim", 1.f);
  t.wHost = gS("wHost", "");
  t.wStart = gI("wStart", 0);
  t.wCount = gI("wCount", 0);
  t.wTurnOn = gB("wOn", true);
  t.wRestore = gB("wRestore", true);
  t.wBri = gI("wBri", -1);
  t.whiteMin = gB("whiteMin", false);
  t.lType = gI("lType", LAYOUT_LINEAR);
  t.led0Right = gB("lRight", false);
  t.linEdge = gI("lEdge", 0);
  if (t.linEdge < 0 || t.linEdge > 3) t.linEdge = 0;
  t.spanFirst = gI("lSf", -1);
  t.spanLast = gI("lSl", -1);
  t.corner = gI("lCorner", 0);
  t.ccw = gB("lCcw", false);
  for (int s = 0; s < 4; s++) t.sides[s] = gI(("lS" + String(s)).c_str(), 0);
  t.shift = gI("lShift", 0);
  // the calibrated table, if the phone stored one
  const String tk = K("tbl4", i);
  free(gStored[i]);
  gStored[i] = nullptr;
  gStoredN[i] = 0;
  const size_t n = prefs.isKey(tk.c_str()) ? prefs.getBytesLength(tk.c_str()) : 0;
  if (n > 0 && n % 4 == 0 && n <= (size_t)fwb::kMaxLeds * 4) {
    gStored[i] = (uint8_t*)malloc(n);
    if (gStored[i]) {
      prefs.getBytes(tk.c_str(), gStored[i], n);
      gStoredN[i] = (int)(n / 4);
    }
  }
  if (t.lType == LAYOUT_TABLE && gStoredN[i] == 0) t.lType = LAYOUT_LINEAR;
}

static void saveTarget(const Target& t, int i) {
  prefs.putString(K("tName", i).c_str(), t.name);
  prefs.putBool(K("tOn", i).c_str(), t.enabled);
  prefs.putFloat(K("tTrim", i).c_str(), t.trim);
  prefs.putString(K("wHost", i).c_str(), t.wHost);
  prefs.putInt(K("wStart", i).c_str(), t.wStart);
  prefs.putInt(K("wCount", i).c_str(), t.wCount);
  prefs.putBool(K("wOn", i).c_str(), t.wTurnOn);
  prefs.putBool(K("wRestore", i).c_str(), t.wRestore);
  prefs.putInt(K("wBri", i).c_str(), t.wBri);
  prefs.putBool(K("whiteMin", i).c_str(), t.whiteMin);
  prefs.putInt(K("lType", i).c_str(), t.lType);
  prefs.putBool(K("lRight", i).c_str(), t.led0Right);
  prefs.putInt(K("lEdge", i).c_str(), t.linEdge);
  prefs.putInt(K("lSf", i).c_str(), t.spanFirst);
  prefs.putInt(K("lSl", i).c_str(), t.spanLast);
  prefs.putInt(K("lCorner", i).c_str(), t.corner);
  prefs.putBool(K("lCcw", i).c_str(), t.ccw);
  for (int s = 0; s < 4; s++) prefs.putInt(K(("lS" + String(s)).c_str(), i).c_str(), t.sides[s]);
  prefs.putInt(K("lShift", i).c_str(), t.shift);
}

static void eraseTargetKeys(int i) {
  static const char* keys[] = {"tName", "tOn", "tTrim", "wHost", "wStart", "wCount", "wOn", "wRestore", "wBri", "whiteMin", "lType", "lRight",
                               "lEdge", "lSf", "lSl", "lCorner", "lCcw", "lS0", "lS1", "lS2", "lS3", "lShift", "tbl4"};
  for (const char* k : keys) prefs.remove(K(k, i).c_str());
}

static void saveStored(int i) {
  const String key = K("tbl4", i);
  if (gStoredN[i] > 0) prefs.putBytes(key.c_str(), gStored[i], (size_t)gStoredN[i] * 4);
  else prefs.remove(key.c_str());
}

static void loadFromFlash() {
  Settings& s = gSet;
  auto gS = [&](const char* k, const char* d) { return prefs.isKey(k) ? prefs.getString(k, d) : String(d); };
  auto gI = [&](const char* k, int d) { return prefs.isKey(k) ? (int)prefs.getInt(k, d) : d; };
  auto gB = [&](const char* k, bool d) { return prefs.isKey(k) ? prefs.getBool(k, d) : d; };
  auto gF = [&](const char* k, float d) { return prefs.isKey(k) ? prefs.getFloat(k, d) : d; };
  s.nTargets = gI("nT", 1);
  if (s.nTargets < 1) s.nTargets = 1;
  if (s.nTargets > kMaxTargets) s.nTargets = kMaxTargets;
  for (int i = 0; i < kMaxTargets; i++) {
    if (i < s.nTargets) loadTarget(s.t[i], i);
    else { s.t[i] = Target(); free(gStored[i]); gStored[i] = nullptr; gStoredN[i] = 0; }
  }
  s.kBri = gF("kBri", 1.f);
  s.kGam = gF("kGam", 1.f);
  s.kSat = gF("kSat", 1.f);
  s.kSmo = gF("kSmo", 0.5f);
  s.kBoost = gF("kBoost", 0.f);
  s.enabled = gB("enabled", true);
  s.tvMode = gI("tvMode", 1);
  if (s.tvMode < 0 || s.tvMode > 2) s.tvMode = 1;
  s.tvSecs = gI("tvSecs", 15);
  if (s.tvSecs < 3 || s.tvSecs > 600) s.tvSecs = 15;
  s.mqttOn = gB("mqOn", false);
  s.mqttHost = gS("mqHost", "");
  s.mqttPort = gI("mqPort", 1883);
  s.mqttUser = gS("mqUser", "");
  s.mqttPass = gS("mqPass", "");
  s.mqttBase = gS("mqBase", "");
  s.mqttDisc = gS("mqDisc", "homeassistant");
  s.perfDebug = gB("perf", false);
  s.smoothOut = gI("kOut", 100);
  s.fps = gI("fps", 30);
  s.boxHost = gS("boxHost", "");
  s.ssid = gS("ssid", "");
  s.pass = gS("pass", "");
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
  prefs.putInt("nT", s.nTargets);
  for (int i = 0; i < s.nTargets; i++) saveTarget(s.t[i], i);
  prefs.putFloat("kBri", s.kBri);
  prefs.putFloat("kGam", s.kGam);
  prefs.putFloat("kSat", s.kSat);
  prefs.putFloat("kSmo", s.kSmo);
  prefs.putFloat("kBoost", s.kBoost);
  prefs.putBool("enabled", s.enabled);
  prefs.putInt("tvMode", s.tvMode);
  prefs.putInt("tvSecs", s.tvSecs);
  prefs.putBool("mqOn", s.mqttOn);
  prefs.putString("mqHost", s.mqttHost);
  prefs.putInt("mqPort", s.mqttPort);
  prefs.putString("mqUser", s.mqttUser);
  prefs.putString("mqPass", s.mqttPass);
  prefs.putString("mqBase", s.mqttBase);
  prefs.putString("mqDisc", s.mqttDisc);
  prefs.putBool("perf", s.perfDebug);
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

// ---------------------------------------------------------------- per-LED tables
// The per-LED table of a target from its layout settings (n = its LED count, out has room for n entries).
static void buildTable(const Target& t, int idx, fwb::Entry* out, int n) {
  if (t.lType == LAYOUT_TABLE && gStoredN[idx] == n) {
    for (int k = 0; k < n; k++) out[k] = fwb::Entry{gStored[idx][k * 4], gStored[idx][k * 4 + 1], gStored[idx][k * 4 + 2], gStored[idx][k * 4 + 3]};
  } else if (t.lType == LAYOUT_LOOP) {
    const bool custom = t.sides[0] > 0;
    if (!fwb::tableLoop(out, n, t.corner, t.ccw, custom ? t.sides : nullptr, t.shift, t.aw, t.ah))
      fwb::tableLinear(out, n, t.led0Right, t.spanFirst, t.spanLast, t.linEdge);
  } else {   // linear (also the fallback when a calibrated table does not match the LED count)
    fwb::tableLinear(out, n, t.led0Right, t.spanFirst, t.spanLast, t.linEdge);
  }
}

void tableRebuild() {
  Lock l;
  const Settings& s = gSet;
  for (int i = 0; i < kMaxTargets; i++) {
    const Target& t = s.t[i];
    const int n = (i < s.nTargets) ? t.wCount : 0;
    gTableN[i] = 0;
    if (n <= 0 || n > fwb::kMaxLeds) {
      free(gTable[i]);
      gTable[i] = nullptr;
      gTableCap[i] = 0;
      continue;
    }
    if (gTableCap[i] != n) {                       // sized to this target's LEDs, nothing more
      free(gTable[i]);
      gTable[i] = (fwb::Entry*)malloc(sizeof(fwb::Entry) * n);
      gTableCap[i] = gTable[i] ? n : 0;
      if (!gTable[i]) continue;
    }
    buildTable(t, i, gTable[i], n);
    gTableN[i] = n;
  }
  gSettingsGen++;
}

// ---------------------------------------------------------------- JSON
static const char* kEdgeNames[4] = {"top", "right", "bottom", "left"};

static void targetToJson(const Target& t, int i, JsonObject o) {
  o["name"] = t.name;
  o["enabled"] = t.enabled;
  o["trim"] = t.trim;
  JsonObject w = o["wled"].to<JsonObject>();
  w["host"] = t.wHost;
  w["start"] = t.wStart;
  w["count"] = t.wCount;
  w["turnOn"] = t.wTurnOn;
  w["restore"] = t.wRestore;
  w["brightness"] = t.wBri;
  JsonObject l2 = o["layout"].to<JsonObject>();
  l2["type"] = t.lType == LAYOUT_TABLE ? "table" : (t.lType == LAYOUT_LOOP ? "loop" : "linear");
  l2["led0Right"] = t.led0Right;
  l2["edge"] = kEdgeNames[t.linEdge < 0 || t.linEdge > 3 ? 0 : t.linEdge];
  l2["spanFirst"] = t.spanFirst;
  l2["spanLast"] = t.spanLast;
  l2["corner"] = t.corner;
  l2["ccw"] = t.ccw;
  JsonArray sides = l2["sides"].to<JsonArray>();
  for (int s = 0; s < 4; s++) sides.add(t.sides[s]);
  l2["shift"] = t.shift;
  l2["hasTable"] = gStoredN[i] > 0;
  JsonArray ua = o["usage"].to<JsonArray>();                 // how many of its LEDs follow each picture zone (the setup page's picture map)
  float u[fwb::kZones] = {};
  if (gTable[i] && gTableN[i] > 0) fwb::zoneUsage(gTable[i], gTableN[i], u);
  for (int z = 0; z < fwb::kZones; z++) ua.add(((int)(u[z] * 10.f + 0.5f)) / 10.f);
}

void settingsToJson(JsonDocument& d) {
  Lock l;
  const Settings& s = gSet;
  JsonArray ts = d["targets"].to<JsonArray>();
  for (int i = 0; i < s.nTargets; i++) targetToJson(s.t[i], i, ts.add<JsonObject>());
  d["wled"] = ts[0]["wled"];                       // the first target also under its old names, for tools written for one target
  d["layout"] = ts[0]["layout"];
  JsonObject k = d["look"].to<JsonObject>();
  k["brightness"] = s.kBri;
  k["gamma"] = s.kGam;
  k["saturation"] = s.kSat;
  k["smoothing"] = s.kSmo;
  k["boost"] = s.kBoost;
  k["smoothOut"] = s.smoothOut;
  d["box"]["host"] = s.boxHost;
  d["relay"]["enabled"] = s.enabled;
  d["relay"]["tvOff"]["mode"] = s.tvMode == 0 ? "nothing" : (s.tvMode == 1 ? "handback" : "off");
  d["relay"]["tvOff"]["seconds"] = s.tvSecs;
  JsonObject mq = d["mqtt"].to<JsonObject>();
  mq["enabled"] = s.mqttOn;
  mq["host"] = s.mqttHost;
  mq["port"] = s.mqttPort;
  mq["user"] = s.mqttUser;
  mq["passSet"] = s.mqttPass.length() > 0;        // the password itself is never sent back
  mq["base"] = s.mqttBase;
  mq["discovery"] = s.mqttDisc;
  mq["perf"] = s.perfDebug;
}

// Applies the "name / enabled / trim", "wled" and "layout" parts of a request to one target. x is the object holding name / enabled /
// trim (the request itself, or one element of "targets"); w and y are its "wled" and "layout" objects. Returns "" or an error.
static String applyTargetJson(Target& n, int idx, JsonVariantConst x, JsonVariantConst w, JsonVariantConst y) {
  if (!x["name"].isNull()) {
    String nm = x["name"].as<String>();
    nm.trim();
    if (nm.length() < 1 || nm.length() > 24) return "The target name must be 1 to 24 characters";
    for (size_t i = 0; i < nm.length(); i++) {
      const char c = nm[i];
      if (!(isalnum((unsigned char)c) || c == ' ' || c == '_' || c == '-' || c == '.')) return "The target name may only contain letters, digits, spaces, - _ and .";
    }
    n.name = nm;
  }
  if (!x["enabled"].isNull()) n.enabled = x["enabled"].as<bool>();
  if (!x["trim"].isNull()) n.trim = clampf(x["trim"].as<float>(), 0.f, 2.f);

  if (!w["host"].isNull()) {
    // an IP address or a name (wled-ceiling.local); pasted URLs ("http://wled-ceiling.local/") are cleaned up first
    String raw = w["host"].as<String>();
    char buf[64];
    if (raw.length() > 200 || fwb::normalizeHost(raw.c_str(), buf, sizeof buf) < 0) return "WLED address is too long";
    String h = buf;
    for (size_t i = 0; i < h.length(); i++) {
      char c = h[i];
      if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) return "WLED address may only contain letters, digits, dots and dashes";
    }
    n.wHost = h;
  }
  if (!w["start"].isNull()) n.wStart = w["start"].as<int>();
  if (!w["count"].isNull()) n.wCount = w["count"].as<int>();
  if (!w["turnOn"].isNull()) n.wTurnOn = w["turnOn"].as<bool>();
  if (!w["restore"].isNull()) n.wRestore = w["restore"].as<bool>();
  if (!w["brightness"].isNull()) n.wBri = w["brightness"].as<int>();
  if (n.wStart < 0 || n.wStart > 20000) return "First LED must be between 0 and 20000";
  if (n.wCount < 0 || n.wCount > fwb::kMaxLeds) return "A target can have at most " + String(fwb::kMaxLeds) + " LEDs";
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
  if (n.lType == LAYOUT_TABLE && (gStoredN[idx] == 0)) return "There is no calibrated layout yet. Run the phone calibration first.";
  if (n.lType == LAYOUT_TABLE && gStoredN[idx] != n.wCount) return "The calibrated layout is for " + String(gStoredN[idx]) + " LEDs. Change the count back or calibrate again.";
  return "";
}

String settingsFromJson(JsonVariantConst d, bool persist) {
  Lock l;
  Settings n = gSet;          // work on a copy; commit only if everything is valid
  JsonVariantConst k = d["look"], b = d["box"];

  if (d["targets"].is<JsonArrayConst>()) {            // the whole list (an import): replaces the targets
    JsonArrayConst a = d["targets"];
    if (a.size() < 1 || a.size() > (size_t)kMaxTargets) return "There can be 1 to " + String(kMaxTargets) + " targets";
    const int old = n.nTargets;
    n.nTargets = (int)a.size();
    for (int i = old; i < n.nTargets; i++) n.t[i] = Target();
    for (int i = n.nTargets; i < kMaxTargets; i++) n.t[i] = Target();
    for (int i = 0; i < n.nTargets; i++) {
      if (i >= old) n.t[i].name = "Target " + String(i + 1);      // a new target gets a default name unless the request names it
      String e = applyTargetJson(n.t[i], i, a[i], a[i]["wled"], a[i]["layout"]);
      if (e.length()) return e;
    }
  } else if (!d["wled"].isNull() || !d["layout"].isNull() || !d["name"].isNull() || !d["trim"].isNull() || !d["enabled"].isNull()) {
    const int ti = d["target"] | 0;                   // which target the top-level wled / layout / name / trim apply to
    if (ti < 0 || ti >= n.nTargets) return "There is no target " + String(ti + 1);
    String e = applyTargetJson(n.t[ti], ti, d, d["wled"], d["layout"]);
    if (e.length()) return e;
  }
  int totalLeds = 0;
  for (int i = 0; i < n.nTargets; i++) totalLeds += n.t[i].wCount;
  if (totalLeds > kMaxTotalLeds) return "The targets have " + String(totalLeds) + " LEDs in total; the limit is " + String(kMaxTotalLeds);

  if (!k["brightness"].isNull()) n.kBri = clampf(k["brightness"].as<float>(), 0.f, 2.f);
  if (!k["gamma"].isNull()) n.kGam = clampf(k["gamma"].as<float>(), 0.3f, 4.f);
  if (!k["saturation"].isNull()) n.kSat = clampf(k["saturation"].as<float>(), 0.f, 3.f);
  if (!k["smoothing"].isNull()) n.kSmo = clampf(k["smoothing"].as<float>(), 0.f, 0.95f);
  if (!k["boost"].isNull()) n.kBoost = clampf(k["boost"].as<float>(), 0.f, 1.f);
  if (!k["smoothOut"].isNull()) n.smoothOut = (int)clampf(k["smoothOut"].as<float>(), 0.f, 100.f);
  if (!b["host"].isNull()) n.boxHost = b["host"].as<String>();
  if (!d["relay"]["enabled"].isNull()) n.enabled = d["relay"]["enabled"].as<bool>();
  JsonVariantConst tv = d["relay"]["tvOff"];
  if (!tv["mode"].isNull()) {
    const String m = tv["mode"].as<String>();
    if (m == "nothing") n.tvMode = 0;
    else if (m == "handback") n.tvMode = 1;
    else if (m == "off") n.tvMode = 2;
    else return "When the TV is off: choose off, handback or nothing";
  }
  if (!tv["seconds"].isNull()) {
    const int sec = tv["seconds"].as<int>();
    if (sec < 3 || sec > 600) return "The TV-off delay must be between 3 and 600 seconds";
    n.tvSecs = sec;
  }
  bool mqttTouched = false;
  JsonVariantConst mq = d["mqtt"];
  if (persist && !mq.isNull()) {                       // MQTT settings are never a live preview
    mqttTouched = true;
    auto okTopic = [](const String& s) { for (size_t i = 0; i < s.length(); i++) { char c = s[i]; if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '/')) return false; } return true; };
    if (!mq["enabled"].isNull()) n.mqttOn = mq["enabled"].as<bool>();
    if (!mq["host"].isNull()) {
      String h = mq["host"].as<String>(); h.trim();
      if (h.length() > 63) return "The MQTT broker address is too long";
      for (size_t i = 0; i < h.length(); i++) { char c = h[i]; if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) return "The MQTT broker address may only contain letters, digits, dots and dashes"; }
      n.mqttHost = h;
    }
    if (!mq["perf"].isNull()) n.perfDebug = mq["perf"].as<bool>();
    if (!mq["port"].isNull()) n.mqttPort = mq["port"].as<int>();
    if (!mq["user"].isNull()) { n.mqttUser = mq["user"].as<String>(); if (n.mqttUser.length() > 64) return "The MQTT user name is too long"; }
    if (!mq["pass"].isNull()) { n.mqttPass = mq["pass"].as<String>(); if (n.mqttPass.length() > 96) return "The MQTT password is too long"; }
    if (!mq["base"].isNull()) { n.mqttBase = mq["base"].as<String>(); n.mqttBase.trim(); if (n.mqttBase.length() > 48 || !okTopic(n.mqttBase)) return "The topic prefix may only contain letters, digits, - _ and /"; }
    if (!mq["discovery"].isNull()) { n.mqttDisc = mq["discovery"].as<String>(); n.mqttDisc.trim(); if (n.mqttDisc.length() == 0) n.mqttDisc = "homeassistant"; if (n.mqttDisc.length() > 32 || !okTopic(n.mqttDisc)) return "The discovery prefix may only contain letters, digits, - _ and /"; }
    if (n.mqttPort < 1 || n.mqttPort > 65535) return "The MQTT port must be between 1 and 65535";
    if (n.mqttOn && n.mqttHost.length() == 0) return "Enter the MQTT broker address (your Home Assistant or Mosquitto host).";
  }

  const int oldCount = gSet.nTargets;
  gSet = n;
  if (n.nTargets < oldCount) {                          // targets dropped (an import with fewer): forget their saved tables
    for (int i = n.nTargets; i < oldCount; i++) { free(gStored[i]); gStored[i] = nullptr; gStoredN[i] = 0; if (persist) eraseTargetKeys(i); }
  }
  if (mqttTouched) gMqttGen++;
  tableRebuild();
  if (persist) { settingsSave(); gPreview = false; }
  else gPreview = true;                      // live preview: used right away, not written to flash until Save
  return "";
}

// The picture map for a target: how many of its LEDs follow each of the 24 zones. `req` holds an unsaved edit (the "wled" / "layout"
// parts of a settings request); it is validated like a real one but nothing is stored. A null `req` means the saved settings.
String targetUsage(int idx, JsonVariantConst req, float out[fwb::kZones], int* leds) {
  Lock l;
  for (int z = 0; z < fwb::kZones; z++) out[z] = 0.f;
  *leds = 0;
  if (idx < 0 || idx >= gSet.nTargets) return "There is no target " + String(idx + 1);
  Target t = gSet.t[idx];
  if (!req.isNull()) {
    String e = applyTargetJson(t, idx, req, req["wled"], req["layout"]);
    if (e.length()) return e;
  }
  const int n = t.wCount;
  *leds = n;
  if (n <= 0 || n > fwb::kMaxLeds) return "";
  fwb::Entry* tb = (fwb::Entry*)malloc(sizeof(fwb::Entry) * n);
  if (!tb) return "Out of memory";
  buildTable(t, idx, tb, n);
  fwb::zoneUsage(tb, n, out);
  free(tb);
  return "";
}

// ---------------------------------------------------------------- calibrated tables
static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

String tableStoreHex(int target, int start, int count, const char* hex, size_t hexLen) {
  if (target < 0 || target >= kMaxTargets) return "No such target";
  if (count < 1 || count > fwb::kMaxLeds) return "LED count out of range";
  if (start < 0 || start > 20000) return "First LED out of range";
  // 8 hex characters per LED (zone a, zone b, blend, distance beyond the TV); older pages sent 6 (no distance)
  const int per = hexLen == (size_t)count * 8 ? 4 : (hexLen == (size_t)count * 6 ? 3 : 0);
  if (!per) return "Table size does not match the LED count";
  uint8_t* tmp = (uint8_t*)malloc((size_t)count * 4);
  if (!tmp) return "Out of memory";
  for (int i = 0; i < count; i++) {
    for (int k = 0; k < 4; k++) {
      uint8_t v = 0;
      if (k < per) {
        int hi = hexv(hex[(i * per + k) * 2]), lo = hexv(hex[(i * per + k) * 2 + 1]);
        if (hi < 0 || lo < 0) { free(tmp); return "Table contains invalid characters"; }
        v = (uint8_t)(hi * 16 + lo);
      }
      tmp[i * 4 + k] = v;
    }
    if (tmp[i * 4] >= fwb::kZones || tmp[i * 4 + 1] >= fwb::kZones) { free(tmp); return "Table refers to a zone that does not exist"; }
  }
  Lock l;
  free(gStored[target]);
  gStored[target] = tmp;
  gStoredN[target] = count;
  saveStored(target);
  Target& t = gSet.t[target];
  t.wStart = start;
  t.wCount = count;
  t.lType = LAYOUT_TABLE;
  tableRebuild();
  settingsSave();
  gPreview = false;
  return "";
}

// ---------------------------------------------------------------- adding and removing targets
String targetAdd(int* index) {
  Lock l;
  if (gSet.nTargets >= kMaxTargets) return "There can be at most " + String(kMaxTargets) + " targets";
  const int i = gSet.nTargets++;
  gSet.t[i] = Target();
  gSet.t[i].name = "Target " + String(i + 1);
  free(gStored[i]);
  gStored[i] = nullptr;
  gStoredN[i] = 0;
  saveStored(i);
  tableRebuild();
  settingsSave();
  gPreview = false;
  if (index) *index = i;
  return "";
}

String targetRemove(int index) {
  Lock l;
  if (gSet.nTargets <= 1) return "At least one target is needed";
  if (index < 0 || index >= gSet.nTargets) return "There is no target " + String(index + 1);
  const int last = gSet.nTargets - 1;
  uint8_t* gone = gStored[index];
  for (int j = index; j < last; j++) {              // the later targets move up
    gSet.t[j] = gSet.t[j + 1];
    gStored[j] = gStored[j + 1];
    gStoredN[j] = gStoredN[j + 1];
    saveStored(j);
  }
  free(gone);
  gStored[last] = nullptr;
  gStoredN[last] = 0;
  gSet.t[last] = Target();
  eraseTargetKeys(last);
  gSet.nTargets = last;
  tableRebuild();
  settingsSave();
  gPreview = false;
  return "";
}

// ---------------------------------------------------------------- moving to another board
static const char* kExportFormat = "fancy2wled-config";
static const int kExportVersion = 2;                 // 1 = a single target (wled / layout / table); 2 = a list of targets and tables

void settingsExport(JsonDocument& d, bool secrets) {
  JsonDocument c;
  settingsToJson(c);
  d["format"] = kExportFormat;
  d["version"] = kExportVersion;
  d["firmware"] = FWB_VERSION;
  d["config"] = c;
  d["config"].remove("wled");                         // only the list of targets is exported (the old names are just aliases of target 0)
  d["config"].remove("layout");
  for (JsonObject tg : d["config"]["targets"].as<JsonArray>()) { tg["layout"].remove("hasTable"); tg.remove("usage"); }   // derived, not settings
  d["config"]["mqtt"].remove("passSet");
  Lock l;
  if (secrets && gSet.mqttPass.length()) d["config"]["mqtt"]["pass"] = gSet.mqttPass;
  JsonArray tb = d["tables"].to<JsonArray>();
  for (int i = 0; i < gSet.nTargets; i++) {
    if (gStoredN[i] <= 0) continue;
    static const char* kHex = "0123456789abcdef";
    String hex;
    hex.reserve((size_t)gStoredN[i] * 8);
    for (int k = 0; k < gStoredN[i] * 4; k++) { hex += kHex[gStored[i][k] >> 4]; hex += kHex[gStored[i][k] & 15]; }
    JsonObject o = tb.add<JsonObject>();
    o["target"] = i;
    o["start"] = gSet.t[i].wStart;
    o["count"] = gStoredN[i];
    o["hex"] = hex;
  }
}

struct TableSnap { uint8_t* data[kMaxTargets]; int n[kMaxTargets]; };
static void snapTables(TableSnap& s) {
  for (int i = 0; i < kMaxTargets; i++) {
    s.n[i] = gStoredN[i];
    s.data[i] = nullptr;
    if (gStoredN[i] > 0) {
      s.data[i] = (uint8_t*)malloc((size_t)gStoredN[i] * 4);
      if (s.data[i]) memcpy(s.data[i], gStored[i], (size_t)gStoredN[i] * 4);
      else s.n[i] = 0;
    }
  }
}
static void restoreTables(TableSnap& s) {              // takes over the snapshot's memory
  for (int i = 0; i < kMaxTargets; i++) {
    free(gStored[i]);
    gStored[i] = s.data[i];
    gStoredN[i] = s.n[i];
    s.data[i] = nullptr;
    saveStored(i);
  }
}
static void freeSnap(TableSnap& s) { for (int i = 0; i < kMaxTargets; i++) { free(s.data[i]); s.data[i] = nullptr; } }

String settingsImport(JsonVariantConst doc) {
  if (String(doc["format"] | "") != kExportFormat) return "This is not a Fancy2WLED config file.";
  const int ver = doc["version"] | 0;
  if (ver < 1) return "This config file has no version number.";
  if (ver > kExportVersion) return "This config file was made by a newer version of Fancy2WLED. Update this board first.";
  JsonVariantConst cfg = doc["config"];
  if (!cfg.is<JsonObjectConst>()) return "The config file has no settings in it.";

  // Everything or nothing: the tables go in first (so a "table" layout is valid when the settings arrive), and if anything is
  // refused afterwards the previous tables, targets and settings are put back.
  Lock l;
  const Settings oldSet = gSet;
  TableSnap snap;
  snapTables(snap);
  String e;
  auto storeTable = [&](int target, JsonVariantConst t) {
    const char* hex = t["hex"] | "";
    String r = tableStoreHex(target, t["start"] | -1, t["count"] | 0, hex, strlen(hex));
    return r.length() ? "The calibration in the file is not valid: " + r : String("");
  };
  if (ver >= 2 && doc["tables"].is<JsonArrayConst>()) {
    for (JsonVariantConst t : doc["tables"].as<JsonArrayConst>()) {
      e = storeTable(t["target"] | 0, t);
      if (e.length()) break;
    }
  } else if (ver == 1 && !doc["table"].isNull()) {
    e = storeTable(0, doc["table"]);
  }
  if (e.length() == 0) e = settingsFromJson(cfg, true);
  if (e.length()) {                                    // refused: put everything back
    restoreTables(snap);
    gSet = oldSet;
    tableRebuild();
    settingsSave();
    gPreview = false;
  } else {
    freeSnap(snap);
  }
  return e;
}
