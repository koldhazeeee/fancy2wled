// The relay: Sync Box broadcast (UDP 19875) -> per-LED colours -> WLED (DDP, UDP 4048).
// Also drives WLED while the phone calibrates and for the rainbow test pattern.
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <errno.h>
#include <lwip/sockets.h>

#include "app.h"

using namespace fwb;

static const int kCalMaxLeds = 2000;
static const uint32_t kStaleMs = 4000;        // box silent this long = stop relaying
static const uint32_t kCalWatchdogMs = 30000; // phone vanished mid-calibration

// Plain UDP sockets: one receives the Sync Box broadcast (blocking wait, no polling), one sends DDP to WLED. (The Arduino
// WiFiUDP class allocates and frees a 1.4 KB buffer for every packet and can only be polled.)
static int gRx = -1, gTx = -1;

// ---- latest Sync Box frame
static uint8_t gRaw[kZones][3];
static bool gHave = false;
static uint32_t gLastFrame = 0, gFrames = 0;
static float gPrevY[kZones][3];                 // the previous Sync Box frame (reaction boost undoes the box's smoothing with it)
static bool gPrevValid = false;
static uint32_t gLastFrameUs = 0;              // micros() when the newest Sync Box frame was read (latency statistics)
static uint32_t gAgeFrames = 0;                // the gFrames value the last sent output frame was built from
static float gIntervalMs = 0;                  // smoothed time between frames
static IPAddress gSender;
static IPAddress gBoxFilter;                   // 0.0.0.0 = accept anyone
static uint32_t gFilterGen = 0xFFFFFFFF;
static SmoothInfo gSmooth = {0, -1, 0, 0, 0u, 0u};     // how the strip beyond the TV eases toward its nearest zones (recomputed when settings change)
static uint32_t gSmoothGen = 0xFFFFFFFF;

// ---- relay state
static float gAdj[kZones][3];                  // adjusted zone colours (brightness / gamma / saturation applied)
static float gTgt[kMaxLeds * 3], gCur[kMaxLeds * 3];
static bool gCurValid = false;
static uint8_t gPayload[kCalMaxLeds * 4];
static uint8_t gDbuf[kDdpHeader + kDdpMaxData];
static uint8_t gSeq = 0;
static uint32_t gLastTick = 0;                 // when the last output frame was produced (a new Sync Box frame or the smoothing tick)
static uint32_t gSmoothUs = 0;                 // micros() of the last smoothing step (smoothing is time based)
static bool gNewPending = false;               // a Sync Box frame arrived that has not been sent on yet
static bool gConverged = false;                // the lights have reached the target: nothing to smooth until the next frame
static bool gWasTesting = false;
static uint32_t gComputedGen = 0xFFFFFFFF;     // gSettingsGen the target was computed with
static uint32_t gWaitHintMs = 10;              // how long the main loop may sleep before the relay needs to act again
static uint8_t gLastSent[kMaxLeds * 4];        // the last frame sent to WLED: identical frames are not sent again
static size_t gLastLen = 0;                    // 0 = nothing valid (the next frame is always sent)
static int gLastStart = 0;
static bool gLastRgbw = false;
static uint32_t gLastSendMs = 0;
static const uint32_t kMinGapMs = 6;           // never produce two output frames closer than this
static const uint32_t kKeepaliveMs = 1000;     // resend an unchanged frame this often so WLED does not leave realtime mode (2.5 s)
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

static uint32_t gWledGen = 0xFFFFFFFF;           // gSettingsGen the cached WLED address was checked against
static bool wledAddress(IPAddress& ip) {
  if (gWledGen == gSettingsGen && (uint32_t)gWledIp != 0) { ip = gWledIp; return true; }    // fast path: once per frame, no allocation
  String host;
  { Lock l; host = gSet.wHost; }
  gWledGen = gSettingsGen;
  if (host != gWledResolved) { gWledResolved = host; gWledTry = 0; gWledIp = IPAddress((uint32_t)0); }
  if ((uint32_t)gWledIp != 0) { ip = gWledIp; return true; }
  if (millis() - gWledTry < 15000 && gWledTry != 0) return false;   // do not hammer name resolution
  gWledTry = millis();
  if (resolveWled(host, gWledIp)) { ip = gWledIp; return true; }
  return false;
}

// ---------------------------------------------------------------- sockets + DDP
static bool openSockets() {
  if (gRx < 0) {
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s >= 0) {
      int one = 1;
      setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
      sockaddr_in a = {};
      a.sin_family = AF_INET;
      a.sin_port = htons(kBoxPort);
      a.sin_addr.s_addr = htonl(INADDR_ANY);
      if (bind(s, (sockaddr*)&a, sizeof a) == 0) {
        fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
        gRx = s;
      } else {
        close(s);
      }
    }
  }
  if (gTx < 0) gTx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  return gRx >= 0 && gTx >= 0;
}

static void ddpSend(const uint8_t* payload, size_t len, int ledStart, bool rgbw) {
  IPAddress ip;
  if (gTx < 0 || !wledAddress(ip)) return;
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(4048);
  to.sin_addr.s_addr = (uint32_t)ip;
  const int per = rgbw ? 4 : 3;
  const uint32_t base = (uint32_t)ledStart * per;
  const size_t step = kDdpMaxData - (kDdpMaxData % per);
  gSeq = gSeq % 15 + 1;
  for (size_t off = 0;; off += step) {
    size_t chunk = len - off < step ? len - off : step;
    bool last = off + step >= len;
    ddpHeader(gDbuf, gSeq, rgbw ? 0x1B : 0x0B, base + (uint32_t)off, (uint16_t)chunk, last);
    memcpy(gDbuf + kDdpHeader, payload + off, chunk);
    if (sendto(gTx, gDbuf, kDdpHeader + chunk, 0, (sockaddr*)&to, sizeof to) < 0) { if (gPerfOn) perfSendFail(); break; }   // out of buffers: drop this frame
    if (last) break;
  }
}
// ---------------------------------------------------------------- relay
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Brightness / gamma / saturation applied to 24 zone colours given as 0..255 floats (the box's value or the boosted one).
static void adjustZonesF(const float raw[kZones][3]) {
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

static void adjustZones(const uint8_t raw[kZones][3]) {
  float f[kZones][3];
  for (int z = 0; z < kZones; z++)
    for (int c = 0; c < 3; c++) f[z][c] = raw[z][c];
  adjustZonesF(f);
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
  gLastLen = 0;
  if (host.length() == 0) return;
  String body = "{\"live\":false";
  if (restore) body += ",\"on\":" + String(gSnapOn ? "true" : "false") + ",\"bri\":" + String(gSnapBri);
  httpPostJson(host, "/json/state", body + "}");
}

// Reads every queued Sync Box packet; true if at least one valid frame arrived.
static bool pollBox() {
  if (gRx < 0) return false;
  if (gFilterGen != gSettingsGen) {
    gFilterGen = gSettingsGen;
    String h; { Lock l; h = gSet.boxHost; }
    if (!gBoxFilter.fromString(h)) gBoxFilter = IPAddress((uint32_t)0);
  }
  bool got = false;
  float boost;
  { Lock l; boost = gSet.kBoost; }
  uint8_t buf[160];
  for (int guard = 0; guard < 16; guard++) {
    sockaddr_in sa;
    socklen_t sl = sizeof sa;
    const int r = recvfrom(gRx, buf, sizeof buf, 0, (sockaddr*)&sa, &sl);
    if (r <= 0) break;                                   // nothing queued (the socket is non-blocking)
    const IPAddress from((uint32_t)sa.sin_addr.s_addr);
    if ((uint32_t)gBoxFilter != 0 && from != gBoxFilter) continue;
    uint8_t z[kZones][3];
    if (decodeFrame(buf, (size_t)r, z)) {                // frames are 86-93 bytes; anything longer was cut at 160 and fails here
      uint32_t now = millis();
      Lock l;
      if (now - gLastFrame > 1500) gPrevValid = false;             // the box was silent: do not extrapolate across the gap
      memcpy(gRaw, z, sizeof gRaw);                                // the raw box output (calibration and /api/zones use this)
      gSender = from;
      if (gHave && now - gLastFrame < 2000) gIntervalMs = gIntervalMs == 0 ? (now - gLastFrame) : gIntervalMs * 0.9f + (now - gLastFrame) * 0.1f;
      gLastFrame = now;
      gLastFrameUs = micros();
      if (gPerfOn) perfBoxFrame(gLastFrameUs);
      gHave = true;
      gFrames++;
      float f[kZones][3];                                          // what the lights should follow: the box's value, or (boost) the colour it is aiming at
      for (int q = 0; q < kZones; q++)
        for (int c = 0; c < 3; c++) {
          f[q][c] = (boost > 0.f && gPrevValid) ? leadBoost(z[q][c], gPrevY[q][c], boost) : (float)z[q][c];
          gPrevY[q][c] = z[q][c];
        }
      gPrevValid = true;
      adjustZonesF(f);
      got = true;
    }
  }
  return got;
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
  gLastLen = 0;
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
  gLastLen = 0;
  if (total <= 0) return;
  memset(gPayload, 0, (size_t)total * 3);
  for (int i = start; i < start + count && i < total; i++) { gPayload[i * 3] = gPayload[i * 3 + 1] = gPayload[i * 3 + 2] = 255; }
  ddpSend(gPayload, (size_t)total * 3, 0, false);
}

void relayBegin() { openSockets(); }

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

// How the output works: a new Sync Box frame is turned into LED colours and sent to WLED the moment it arrives (not at the next
// tick), then a tick every 1000/fps ms keeps the temporal smoothing moving until the lights reach the target. The smoothing is
// based on elapsed time (smoothDecay), so the extra frame does not change how it feels. A frame identical to the last one sent
// is not sent again (a keep-alive every second stops WLED leaving realtime mode).
void relayLoop() {
  const bool gotNew = pollBox();
  if (gotNew) { gNewPending = true; gConverged = false; }
  const uint32_t now = millis();
  gWaitHintMs = 10;

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

  int fps, n, start; float smo; bool whiteMin, testing, fresh;
  {
    Lock l;
    fps = gSet.fps < 1 ? 30 : gSet.fps; n = gTableN; start = gSet.wStart; smo = gSet.kSmo; whiteMin = gSet.whiteMin;
    testing = gTestUntil && (int32_t)(gTestUntil - now) > 0;
    if (gTestUntil && !testing) gTestUntil = 0;
    fresh = gSet.enabled && gHave && now - gLastFrame < kStaleMs;        // paused (Home Assistant) = as if the box were silent
  }
  if (n <= 0) return;
  if (!fresh && !testing) {
    if (gPrepared) { wledRestore(); gCurValid = false; }      // box gone quiet: give WLED back
    gNewPending = false;
    return;
  }
  if (testing != gWasTesting) { gWasTesting = testing; gConverged = false; }
  const uint32_t tickMs = 1000u / (uint32_t)fps;
  const uint32_t since = now - gLastTick;
  const bool settingsChanged = gComputedGen != gSettingsGen;
  const bool eventDue = gNewPending && since >= kMinGapMs;           // a new picture: send it right away
  const bool tickDue = since >= tickMs && (!gConverged || !gCurValid || settingsChanged);   // keep smoothing toward the target
  if (!eventDue && !tickDue) {
    if (gLastLen && now - gLastSendMs >= kKeepaliveMs) { ddpSend(gLastSent, gLastLen, gLastStart, gLastRgbw); gLastSendMs = now; }
    if (gNewPending) gWaitHintMs = since >= kMinGapMs ? 1 : kMinGapMs - since;
    else if (!gConverged || !gCurValid || settingsChanged) gWaitHintMs = since >= tickMs ? 1 : tickMs - since;
    return;
  }
  gLastTick = now;
  gNewPending = false;
  if (!gPrepared) wledPrepare();

  const uint32_t pf0 = gPerfOn ? micros() : 0;              // performance statistics: time the frame work
  { Lock l; if (testing) { uint8_t z[kZones][3]; rainbowZones(z); adjustZones(z); }
    if (gSmoothGen != gSettingsGen) { gSmooth = smoothInfo(gTable, n, gSet.smoothOut); gSmoothGen = gSettingsGen; }
    applyTable(gTable, n, gAdj, gTgt, gSmooth);
    gComputedGen = gSettingsGen; }
  const uint32_t nowUs = micros();
  if (!gCurValid) {
    memcpy(gCur, gTgt, sizeof(float) * n * 3);
    gCurValid = true;
    gConverged = true;
  } else {
    const float decay = smoothDecay(smo, (nowUs - gSmoothUs) / 1000.f, (float)tickMs);
    float worst = 0.f;
    for (int i = 0; i < n * 3; i++) {
      const float d = gTgt[i] - gCur[i];
      gCur[i] += d * (1.f - decay);
      const float left = d * decay;
      if (left > worst) worst = left; else if (-left > worst) worst = -left;
    }
    gConverged = worst < 0.5f;                              // less than half a colour step away: nothing visible left to smooth
  }
  gSmoothUs = nowUs;
  const size_t len = packColours(gCur, n, whiteMin);
  const bool same = len == gLastLen && start == gLastStart && whiteMin == gLastRgbw && memcmp(gPayload, gLastSent, len) == 0;
  if (same && now - gLastSendMs < kKeepaliveMs) {
    if (gPerfOn) perfSkip();
  } else {
    const uint32_t ps0 = gPerfOn ? micros() : 0;
    ddpSend(gPayload, len, start, whiteMin);
    if (gPerfOn && pf0) {
      const uint32_t t1 = micros();
      const bool first = gFrames != gAgeFrames;
      perfFrame(t1 - pf0, t1 - ps0, t1 - gLastFrameUs, first);
    }
    memcpy(gLastSent, gPayload, len);
    gLastLen = len; gLastStart = start; gLastRgbw = whiteMin; gLastSendMs = now;
  }
  gAgeFrames = gFrames;
  if (!gConverged) gWaitHintMs = tickMs;
}

// Called by the main loop instead of a fixed delay: sleeps until a Sync Box packet arrives (wakes at once) or the relay has
// something to do again, at most maxMs.
void relayWait(uint32_t maxMs) {
  if (gRx < 0 || WiFi.status() != WL_CONNECTED) { delay(1); return; }
  uint32_t ms = gWaitHintMs < maxMs ? gWaitHintMs : maxMs;
  if (ms < 1) ms = 1;
  fd_set rf;
  FD_ZERO(&rf);
  FD_SET(gRx, &rf);
  timeval tv;
  tv.tv_sec = ms / 1000;
  tv.tv_usec = (ms % 1000) * 1000;
  select(gRx + 1, &rf, nullptr, nullptr, &tv);
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
  gLastLen = 0;
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
