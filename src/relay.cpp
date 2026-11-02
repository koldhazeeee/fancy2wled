// The relay: Sync Box broadcast (UDP 19875) -> per-LED colours -> WLED (DDP, UDP 4048).
// Also drives WLED while the phone calibrates and for the rainbow test pattern.
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>

#include "app.h"

using namespace fwb;

static const int kCalMaxLeds = 2000;
static const uint32_t kStaleMs = 4000;        // box silent this long = stop relaying
static const uint32_t kCalWatchdogMs = 30000; // phone vanished mid-calibration

static WiFiUDP udp;
static bool udpUp = false;

// ---- latest Sync Box frame
static uint8_t gRaw[kZones][3];
static bool gHave = false;
static uint32_t gLastFrame = 0, gFrames = 0;
static float gIntervalMs = 0;                  // smoothed time between frames
static IPAddress gSender;
static IPAddress gBoxFilter;                   // 0.0.0.0 = accept anyone
static uint32_t gFilterGen = 0xFFFFFFFF;
static SmoothInfo gSmooth = {0, -1, 0, 0};     // how the strip beyond the TV eases to the average (recomputed when settings change)
static uint32_t gSmoothGen = 0xFFFFFFFF;

// ---- relay state
static float gAdj[kZones][3];                  // adjusted zone colours (brightness / gamma / saturation applied)
static float gTgt[kMaxLeds * 3], gCur[kMaxLeds * 3];
static bool gCurValid = false;
static uint8_t gPayload[kCalMaxLeds * 4];
static uint8_t gDbuf[kDdpHeader + kDdpMaxData];
static uint8_t gSeq = 0;
static uint32_t gLastTick = 0;
static bool gPrepared = false;                 // WLED switched on / brightness set for streaming
static bool gSnapOn = false;
static int gSnapBri = 128;
static IPAddress gWledIp;
static String gWledResolved;
static uint32_t gWledTry = 0;
static bool gWledOk = false;
static int gWledLeds = 0;
static String gWledName;

// ---- calibration / test
static struct {
  bool active = false;
  uint32_t lastCmd = 0, lastSend = 0;
  int total = 0, start = 0, count = 0;
  bool dirty = true;
  bool endPending = false;
  char screen[8] = "idle";
  bool snapOn = false;
  int snapBri = 128;
} gCal;
static uint32_t gTestUntil = 0;

// ---- marker blocks (the setup page lights the start / end of the run and of the TV part)
static struct {
  uint32_t until = 0, lastSend = 0;
  int n = 0;
  MarkBlock b[8];
  bool dirty = true;
} gMark;

// ---------------------------------------------------------------- WLED HTTP helpers
static bool httpGet(const String& host, const char* path, String& body, int timeoutMs = 1500) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, host, 80, path)) return false;
  int code = http.GET();
  bool ok = code == 200;
  if (ok) body = http.getString();
  http.end();
  return ok;
}

static bool httpPostJson(const String& host, const char* path, const String& json, int timeoutMs = 1500) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(timeoutMs);
  http.setTimeout(timeoutMs);
  if (!http.begin(client, host, 80, path)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(json);
  http.end();
  return code == 200;
}

WledInfo wledQuery(const String& host) {
  WledInfo r;
  if (host.length() == 0) { r.error = "Enter the WLED address first."; return r; }
  String body;
  if (!httpGet(host, "/json/info", body)) { r.error = "Cannot reach WLED at " + host + ". Check the address and that both are on the same network."; return r; }
  JsonDocument filter;
  filter["ver"] = true;
  filter["name"] = true;
  filter["leds"]["count"] = true;
  filter["leds"]["rgbw"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) { r.error = "That did not look like a WLED device."; return r; }
  if (doc["leds"]["count"].isNull()) { r.error = "That did not look like a WLED device."; return r; }
  r.leds = doc["leds"]["count"].as<int>();
  r.rgbw = doc["leds"]["rgbw"].as<bool>();
  r.name = doc["name"].as<String>();
  r.ver = doc["ver"].as<String>();
  String st;
  if (httpGet(host, "/json/state", st)) {
    JsonDocument f2;
    f2["on"] = true;
    f2["bri"] = true;
    JsonDocument d2;
    if (!deserializeJson(d2, st, DeserializationOption::Filter(f2))) {
      r.on = d2["on"].as<bool>();
      r.bri = d2["bri"] | 128;
    }
  }
  r.ok = true;
  return r;
}

int wledDiscover(String& out) {
  JsonDocument d;
  JsonArray a = d["devices"].to<JsonArray>();
  int n = MDNS.queryService("wled", "tcp");
  for (int i = 0; i < n && i < 12; i++) {
    JsonObject o = a.add<JsonObject>();
    String h = MDNS.hostname(i);
    o["name"] = h.length() ? h : String("WLED");
    o["ip"] = MDNS.IP(i).toString();
  }
  serializeJson(d, out);
  return n;
}

static bool resolveWled(const String& host, IPAddress& out) {
  if (host.length() == 0) return false;
  if (out.fromString(host)) return true;
  if (host.endsWith(".local")) {
    IPAddress ip = MDNS.queryHost(host.substring(0, host.length() - 6), 1500);
    if ((uint32_t)ip != 0) { out = ip; return true; }
    return false;
  }
  return WiFi.hostByName(host.c_str(), out) == 1;
}

static bool wledAddress(IPAddress& ip) {
  String host;
  { Lock l; host = gSet.wHost; }
  if (host != gWledResolved) { gWledResolved = host; gWledTry = 0; gWledIp = IPAddress((uint32_t)0); }
  if ((uint32_t)gWledIp != 0) { ip = gWledIp; return true; }
  if (millis() - gWledTry < 15000 && gWledTry != 0) return false;   // do not hammer name resolution
  gWledTry = millis();
  if (resolveWled(host, gWledIp)) { ip = gWledIp; return true; }
  return false;
}

// ---------------------------------------------------------------- DDP
static void ddpSend(const uint8_t* payload, size_t len, int ledStart, bool rgbw) {
  IPAddress ip;
  if (!wledAddress(ip)) return;
  const int per = rgbw ? 4 : 3;
  const uint32_t base = (uint32_t)ledStart * per;
  const size_t step = kDdpMaxData - (kDdpMaxData % per);
  gSeq = gSeq % 15 + 1;
  for (size_t off = 0;; off += step) {
    size_t chunk = len - off < step ? len - off : step;
    bool last = off + step >= len;
    ddpHeader(gDbuf, gSeq, rgbw ? 0x1B : 0x0B, base + (uint32_t)off, (uint16_t)chunk, last);
    memcpy(gDbuf + kDdpHeader, payload + off, chunk);
    udp.beginPacket(ip, 4048);
    udp.write(gDbuf, kDdpHeader + chunk);
    udp.endPacket();
    if (last) break;
  }
}

// ---------------------------------------------------------------- relay
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void adjustZones(const uint8_t raw[kZones][3]) {
  float br, gm, sat;
  { Lock l; br = gSet.kBri; gm = gSet.kGam; sat = gSet.kSat; }
  for (int z = 0; z < kZones; z++) {
    float c[3] = {raw[z][0] / 255.f, raw[z][1] / 255.f, raw[z][2] / 255.f};
    if (sat != 1.f) {
      float y = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
      for (int k = 0; k < 3; k++) c[k] = clampf(y + (c[k] - y) * sat, 0.f, 1.f);
    }
    if (gm != 1.f) for (int k = 0; k < 3; k++) c[k] = powf(c[k], gm);
    for (int k = 0; k < 3; k++) gAdj[z][k] = clampf(c[k] * br * 255.f, 0.f, 255.f);
  }
}

static void rainbowZones(uint8_t z[kZones][3]) {
  static const uint8_t col[6][3] = {{255, 0, 0}, {255, 130, 0}, {255, 255, 0}, {0, 255, 0}, {0, 80, 255}, {200, 0, 255}};
  for (int i = 0; i < kZones; i++)
    for (int k = 0; k < 3; k++) z[i][k] = col[i % 6][k];
}

static size_t packColours(const float* cur, int n, bool whiteMin) {
  size_t o = 0;
  for (int i = 0; i < n; i++) {
    int r = (int)(cur[i * 3] + 0.5f), g = (int)(cur[i * 3 + 1] + 0.5f), b = (int)(cur[i * 3 + 2] + 0.5f);
    r = r < 0 ? 0 : (r > 255 ? 255 : r); g = g < 0 ? 0 : (g > 255 ? 255 : g); b = b < 0 ? 0 : (b > 255 ? 255 : b);
    if (whiteMin) {
      int w = r < g ? (r < b ? r : b) : (g < b ? g : b);
      gPayload[o++] = r - w; gPayload[o++] = g - w; gPayload[o++] = b - w; gPayload[o++] = w;
    } else {
      gPayload[o++] = r; gPayload[o++] = g; gPayload[o++] = b;
    }
  }
  return o;
}

static void wledPrepare() {
  String host; bool on; int bri; bool restore;
  { Lock l; host = gSet.wHost; on = gSet.wTurnOn; bri = gSet.wBri; restore = gSet.wRestore; }
  (void)restore;
  gPrepared = true;                              // even on failure: do not retry on every frame
  if (!on && bri < 0) return;
  WledInfo wi = wledQuery(host);
  gWledOk = wi.ok;
  if (!wi.ok) return;
  gWledLeds = wi.leds;
  gWledName = wi.name;
  gSnapOn = wi.on;
  gSnapBri = wi.bri;
  String body = "{";
  bool any = false;
  if (on && !wi.on) { body += "\"on\":true"; any = true; }
  if (bri >= 0 && bri != wi.bri) { body += String(any ? "," : "") + "\"bri\":" + String(bri); any = true; }
  body += "}";
  if (any) httpPostJson(host, "/json/state", body);
}

// WLED keeps showing the last streamed frame until its realtime timeout (about 2.5 s) runs out. {"live":false} makes it leave
// realtime mode at once, so the lights return the moment we stop streaming.
static void wledRestore() {
  bool restore; String host;
  { Lock l; restore = gSet.wRestore; host = gSet.wHost; }
  gPrepared = false;
  if (host.length() == 0) return;
  String body = "{\"live\":false";
  if (restore) body += ",\"on\":" + String(gSnapOn ? "true" : "false") + ",\"bri\":" + String(gSnapBri);
  httpPostJson(host, "/json/state", body + "}");
}

static void pollBox() {
  if (!udpUp) return;
  if (gFilterGen != gSettingsGen) {
    gFilterGen = gSettingsGen;
    String h; { Lock l; h = gSet.boxHost; }
    if (!gBoxFilter.fromString(h)) gBoxFilter = IPAddress((uint32_t)0);
  }
  uint8_t buf[160];
  int n;
  while ((n = udp.parsePacket()) > 0) {
    IPAddress from = udp.remoteIP();
    if (n > (int)sizeof buf) { udp.flush(); continue; }
    int r = udp.read(buf, sizeof buf);
    if ((uint32_t)gBoxFilter != 0 && from != gBoxFilter) continue;
    uint8_t z[kZones][3];
    if (r > 0 && decodeFrame(buf, (size_t)r, z)) {
      uint32_t now = millis();
      Lock l;
      memcpy(gRaw, z, sizeof gRaw);
      gSender = from;
      if (gHave && now - gLastFrame < 2000) gIntervalMs = gIntervalMs == 0 ? (now - gLastFrame) : gIntervalMs * 0.9f + (now - gLastFrame) * 0.1f;
      gLastFrame = now;
      gHave = true;
      gFrames++;
      adjustZones(z);
    }
  }
}

static void finishCal();

static void markSendFrame() {
  int st, cnt, n;
  bool whiteMin;
  MarkBlock blocks[8];
  {
    Lock l;
    st = gSet.wStart; cnt = gSet.wCount; whiteMin = gSet.whiteMin; n = gMark.n;
    memcpy(blocks, gMark.b, sizeof blocks);
    gMark.dirty = false;
    gMark.lastSend = millis();
  }
  if (cnt <= 0 || cnt > kMaxLeds) return;
  const int per = whiteMin ? 4 : 3;
  memset(gPayload, 0, (size_t)cnt * per);                      // the whole run dark, only the markers lit
  for (int k = 0; k < n; k++)
    for (int i = blocks[k].start; i < blocks[k].start + blocks[k].count; i++) {
      const int idx = i - st;
      if (idx < 0 || idx >= cnt) continue;
      uint8_t* p = gPayload + (size_t)idx * per;
      p[0] = blocks[k].r; p[1] = blocks[k].g; p[2] = blocks[k].b;
      if (per == 4) p[3] = 0;
    }
  ddpSend(gPayload, (size_t)cnt * per, st, whiteMin);
}

static void calSendFrame() {
  int total, start, count;
  { Lock l; total = gCal.total; start = gCal.start; count = gCal.count; gCal.dirty = false; gCal.lastSend = millis(); }
  if (total <= 0) return;
  memset(gPayload, 0, (size_t)total * 3);
  for (int i = start; i < start + count && i < total; i++) { gPayload[i * 3] = gPayload[i * 3 + 1] = gPayload[i * 3 + 2] = 255; }
  ddpSend(gPayload, (size_t)total * 3, 0, false);
}

void relayBegin() {
  if (!udpUp) udpUp = udp.begin(kBoxPort);
}

bool relayZones(uint8_t zones[kZones][3], uint32_t* ageMs) {
  Lock l;
  if (!gHave) { *ageMs = UINT32_MAX; return false; }
  memcpy(zones, gRaw, sizeof gRaw);
  *ageMs = millis() - gLastFrame;
  return true;
}

void relayStatus(RelayStatus& s) {
  Lock l;
  bool fresh = gHave && millis() - gLastFrame < 2500;
  s.boxFps = (fresh && gIntervalMs > 0) ? 1000.f / gIntervalMs : 0.f;
  s.boxAgeMs = gHave ? millis() - gLastFrame : UINT32_MAX;
  s.boxSender = gHave ? gSender.toString() : String("");
  s.wledOk = gWledOk;
  s.wledLeds = gWledLeds;
  s.wledName = gWledName;
  if (gCal.active) s.state = "calibrating";
  else if (gMark.until && (int32_t)(gMark.until - millis()) > 0) s.state = "showing markers";
  else if (gTestUntil && (int32_t)(gTestUntil - millis()) > 0) s.state = "showing test pattern";
  else if (gSet.wHost.length() == 0 || gSet.wCount <= 0) s.state = "needs setup";
  else if (!gSet.enabled) s.state = "paused";
  else if (!fresh) s.state = "waiting for the Sync Box";
  else s.state = "relaying";
}

void relayLoop() {
  pollBox();
  const uint32_t now = millis();

  // ---- calibration has priority (all DDP sending happens in this task only)
  bool calActive, endPending;
  { Lock l; calActive = gCal.active; endPending = gCal.endPending; }
  if (endPending) { finishCal(); return; }
  if (calActive) {
    bool dirty, wd;
    { Lock l; dirty = gCal.dirty; wd = now - gCal.lastCmd > kCalWatchdogMs; }
    if (wd) { calEnd(); return; }
    if (dirty || now - gCal.lastSend > 150) calSendFrame();
    return;
  }

  // ---- marker blocks: dark run with a few lit LEDs at the places being set
  {
    bool marking, send = false;
    { Lock l; marking = gMark.until && (int32_t)(gMark.until - now) > 0;
      if (gMark.until && !marking) { gMark.until = 0; gCurValid = false; }          // expired: the picture takes over again
      if (marking) send = gMark.dirty || now - gMark.lastSend > 200; }
    if (marking) {
      if (!gPrepared) wledPrepare();
      if (send) markSendFrame();
      return;
    }
  }

  int fps, n, start; float smo; bool whiteMin, testing;
  {
    Lock l;
    fps = gSet.fps < 1 ? 30 : gSet.fps; n = gTableN; start = gSet.wStart; smo = gSet.kSmo; whiteMin = gSet.whiteMin;
    testing = gTestUntil && (int32_t)(gTestUntil - now) > 0;
    if (gTestUntil && !testing) gTestUntil = 0;
  }
  if (now - gLastTick < (uint32_t)(1000 / fps)) return;
  gLastTick = now;
  if (n <= 0) return;

  bool fresh;
  { Lock l; fresh = gSet.enabled && gHave && now - gLastFrame < kStaleMs; }     // paused (Home Assistant) = as if the box were silent
  if (!fresh && !testing) {
    if (gPrepared) { wledRestore(); gCurValid = false; }      // box gone quiet: give WLED back
    return;
  }
  if (!gPrepared) wledPrepare();

  { Lock l; if (testing) { uint8_t z[kZones][3]; rainbowZones(z); adjustZones(z); }
    if (gSmoothGen != gSettingsGen) { gSmooth = smoothInfo(gTable, n, gSet.smoothOut); gSmoothGen = gSettingsGen; }
    applyTable(gTable, n, gAdj, gTgt, gSmooth); }
  const float alpha = 1.f - clampf(smo, 0.f, 0.95f);
  if (!gCurValid) { memcpy(gCur, gTgt, sizeof(float) * n * 3); gCurValid = true; }
  else for (int i = 0; i < n * 3; i++) gCur[i] += (gTgt[i] - gCur[i]) * alpha;
  size_t len = packColours(gCur, n, whiteMin);
  ddpSend(gPayload, len, start, whiteMin);
}

// ---------------------------------------------------------------- calibration API
String calBegin(int* total) {
  String host; { Lock l; host = gSet.wHost; }
  if (host.length() == 0) return "Set the WLED address first (on the setup page).";
  WledInfo wi = wledQuery(host);
  if (!wi.ok) return wi.error;
  int tot = wi.leds > kCalMaxLeds ? kCalMaxLeds : wi.leds;
  httpPostJson(host, "/json/state", "{\"on\":true,\"bri\":255}");
  {
    Lock l;
    gCal.active = true;
    gCal.endPending = false;
    gCal.total = tot;
    gCal.start = 0;
    gCal.count = 0;
    gCal.dirty = true;
    gCal.lastCmd = millis();
    gCal.snapOn = wi.on;
    gCal.snapBri = wi.bri;
    strcpy(gCal.screen, "black");
    gTestUntil = 0;
  }
  gWledOk = true;
  gWledLeds = wi.leds;
  gWledName = wi.name;
  *total = tot;
  return "";
}

void calLed(int start, int count) {
  Lock l;
  if (!gCal.active) return;
  gCal.start = start < 0 ? 0 : start;
  gCal.count = count < 0 ? 0 : count;
  gCal.dirty = true;
  gCal.lastCmd = millis();
}

void calScreen(const char* mode) {
  Lock l;
  if (!gCal.active) return;
  if (strcmp(mode, "white") == 0 || strcmp(mode, "black") == 0 || strcmp(mode, "idle") == 0 || strcmp(mode, "squares") == 0) strlcpy(gCal.screen, mode, sizeof gCal.screen);
  gCal.lastCmd = millis();
}

const char* calScreenMode() {
  static char copy[8];
  Lock l;
  strlcpy(copy, gCal.active ? gCal.screen : "idle", sizeof copy);
  return copy;
}

// Called from the web task (or the watchdog): only flags the end. The relay task does the work (finishCal).
void calEnd() {
  Lock l;
  if (!gCal.active) return;
  gCal.active = false;
  gCal.endPending = true;
  strcpy(gCal.screen, "idle");
}

static void finishCal() {
  bool snapOn; int snapBri, total; String host;
  {
    Lock l;
    snapOn = gCal.snapOn; snapBri = gCal.snapBri; total = gCal.total; host = gSet.wHost;
    gCal.endPending = false;
  }
  if (total > 0) { memset(gPayload, 0, (size_t)total * 3); ddpSend(gPayload, (size_t)total * 3, 0, false); }   // switch off what we lit
  httpPostJson(host, "/json/state", "{\"live\":false,\"on\":" + String(snapOn ? "true" : "false") + ",\"bri\":" + String(snapBri) + "}");
  gCurValid = false;
  gPrepared = false;
}
void testPattern(int seconds) {
  Lock l;
  gTestUntil = seconds > 0 ? millis() + (uint32_t)seconds * 1000UL : 0;
  if (!gTestUntil) gCurValid = false;
  gPrepared = gPrepared && seconds > 0;
}

void markShow(const MarkBlock* blocks, int n, int seconds) {
  Lock l;
  if (gCal.active) return;                       // calibration owns the LEDs
  if (seconds <= 0 || n <= 0) { if (gMark.until) gCurValid = false; gMark.until = 0; return; }
  if (n > 8) n = 8;
  for (int i = 0; i < n; i++) gMark.b[i] = blocks[i];
  gMark.n = n;
  gMark.until = millis() + (uint32_t)seconds * 1000UL;
  if (!gMark.until) gMark.until = 1;
  gMark.dirty = true;                            // a running rainbow test resumes by itself when the markers expire
}
