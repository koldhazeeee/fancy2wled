// The relay: Sync Box broadcast (UDP 19875) -> per-LED colours -> one or more WLED targets (DDP, UDP 4048).
// Also drives a target while the LED finder / phone calibration is lighting it, and for the rainbow test pattern and the markers.
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <errno.h>
#include <fcntl.h>
#include <lwip/sockets.h>

#include "app.h"

using namespace fwb;

static const int kCalMaxLeds = 2000;
static const uint32_t kStaleMs = 4000;        // box silent this long = stop relaying
static const uint32_t kCalWatchdogMs = 30000; // phone vanished mid-calibration

// Plain UDP sockets: one receives the Sync Box broadcast (blocking wait, no polling), one sends DDP to every WLED. (The Arduino
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
static float gAdj[kZones][3];                  // the picture's 24 zones with brightness / gamma / saturation applied (shared by all targets)
static Motion gMotion;                         // the latest pictures with their place on a steady clock, for motion smoothing (under Lock)

// ---- what arrives on the Sync Box port, for telling "the box is silent" from "the box changed what it sends"
static struct {
  uint32_t ok = 0, newLayout = 0, unreadable = 0;   // datagrams by class since boot
  uint32_t lastOddMs = 0;
  bool haveOdd = false;
} gStat;
struct OddSample {                                  // a raw datagram we could not read (colours only: nothing private is in it)
  uint32_t ms = 0;
  uint8_t cls = 0;
  bool checksumOk = false;
  uint16_t len = 0;
  uint8_t data[260];
};
static OddSample gOdd[4];
static int gOddCount = 0, gOddHead = 0;             // a ring: the newest is at gOddHead - 1
static uint32_t gOddStoredMs = 0;
static uint8_t gGoodSample[93];                     // one recent frame that did decode, to compare with
static uint8_t gGoodLen = 0;
static uint32_t gGoodSampleMs = 0;

// ---- output timing (shared: all targets are produced in the same output cycle)
static uint8_t gPayload[kCalMaxLeds * 4];      // scratch for one frame
static uint8_t gDbuf[kDdpHeader + kDdpMaxData];
static uint8_t gSeq = 0;
static uint32_t gLastTick = 0;                 // when the last output cycle ran (a new Sync Box frame or the smoothing tick)
static bool gNewPending = false;               // a Sync Box frame arrived that has not been sent on yet
static uint32_t gWaitHintMs = 10;              // how long the main loop may sleep before the relay needs to act again
static const uint32_t kMinGapMs = 6;           // never run two output cycles closer than this
static const uint32_t kKeepaliveMs = 1000;     // resend an unchanged frame this often so WLED does not leave realtime mode (2.5 s)
static const uint32_t kGoveeGapMs = 80;         // a Govee light gets at most about 12 frames a second (UDP, no acknowledgement, no documented limit)
static const uint32_t kResolveEveryMs = 60000; // look a WLED's name up again this often (its IP may have changed)
static const uint32_t kRetryResolveMs = 15000; // ... and this often while the name has not been found yet
static uint32_t gHouseMs = 0;                  // when the once-a-second housekeeping last ran
static volatile bool gUpdating = false;        // a firmware update is being written: no streaming
void relayUpdateBegin() { gUpdating = true; }
void relayUpdateEnd() { gUpdating = false; }
static BlackTracker gBlack;                   // is the picture black, and since when (TV off detection, see core.h); under Lock

// ---- per-target state
struct Cal {                                   // the LED finder / phone calibration session on this target
  bool active = false;
  uint32_t lastCmd = 0, lastSend = 0;
  int total = 0, start = 0, count = 0;
  bool dirty = true;
  bool alt = false;                            // show every LED / segment alternately red and blue instead of one white block
  bool endPending = false;
  char screen[8] = "idle";
  bool snapOn = false;
  int snapBri = 128;
};
struct Mark {                                  // marker blocks (the setup page lights the start / end of the run and of the TV part)
  uint32_t until = 0, lastSend = 0;
  int n = 0;
  MarkBlock b[8];
  bool dirty = true;
};
struct Rt {
  float *tgt = nullptr, *cur = nullptr;        // wanted / shown colours of this target's LEDs (n * 3)
  int cap = 0;
  uint8_t* last = nullptr;                     // the last frame sent: identical frames are not sent again
  int lastCap = 0;
  size_t lastLen = 0;                          // 0 = nothing valid (the next frame is always sent)
  int lastStart = 0;
  bool lastRgbw = false;
  uint32_t lastSendMs = 0;
  bool curValid = false, converged = false, prepared = false, wasTesting = false;
  uint32_t smoothUs = 0, computedGen = 0xFFFFFFFF, smoothGen = 0xFFFFFFFF;
  SmoothInfo smooth = {0, -1, 0, 0, 0u, 0u};   // how the strip beyond the TV eases (recomputed when settings change)
  // The WLED's address. A name (wled-ceiling.local) is resolved by the background task, never by the relay loop (a lookup can take
  // seconds), and looked up again every kResolveEveryMs, so a WLED whose IP changed is found again. A name that stops answering keeps its
  // last known address.
  volatile uint32_t ipRaw = 0;                 // the address as a network-order word, 0 = not known yet (written by the worker, read by the relay loop)
  String resolved;                             // the host ipRaw belongs to (relay task only)
  bool hostIsName = false;                     // the host is a name, not an IP address (relay task only)
  bool resolving = false;                      // a lookup is queued or running (under Lock)
  uint32_t resolveMs = 0;                      // when ipRaw was last confirmed (under Lock)
  uint32_t tryMs = 0, addrGen = 0xFFFFFFFF;    // when a lookup was last requested; the settings generation the host was last compared at
  String hostCache;                            // the host this target was prepared for
  bool restoreCache = true;
  int kindCache = 0;                           // the target type (0 WLED, 1 Govee light) this target was prepared as
  uint32_t goveeEnableMs = 0;                  // when "streaming on" was last sent to a Govee light
  int snapR = 0, snapG = 0, snapB = 0;         // a Govee light's colour before the stream (it is restored afterwards)
  bool offByTv = false;                        // this WLED was switched off because the TV went off: the next stream switches it on again and keeps its original state
  String snapHost;                             // the host snapOn / snapBri were read from
  bool snapOn = false;
  int snapBri = 128;
  bool wledOk = false;
  int wledLeds = 0;
  String wledName;
  // Delay tracking (docs/LATENCY.md). The age of each Sync Box frame when this target's packet for it leaves, over 5 s windows; round trips to the target.
  uint32_t ageSumUs = 0, ageN = 0, ageMaxUs = 0, ageWinMs = 0, ageFrame = 0;   // the window being filled (relay task only)
  float pubAgeAvgMs = -1, pubAgeMaxMs = -1;    // the last finished window (under Lock)
  RttRing rtt;                                 // the last probe round trips (under Lock)
  uint32_t probeMs = 0, rttMs = 0;             // when a probe was last queued; when the latest one finished (under Lock for rttMs)
  bool probing = false;                        // a probe is queued or running (under Lock)
  int probeCount = 0;                          // probes so far: every sixth one of a WLED also reads its refresh rate
  int wledFps = 0;                             // the refresh rate WLED last reported (0 = unknown)
  Cal cal;
  uint32_t testUntil = 0;
  Mark mark;
};
static Rt gRt[kMaxTargets];
static int gCalTarget = -1;                    // the target with a finder / calibration session (one at a time)

static void freeBuffers(Rt& r) {
  free(r.tgt); free(r.cur); free(r.last);
  r.tgt = r.cur = nullptr;
  r.last = nullptr;
  r.cap = r.lastCap = 0;
  r.lastLen = 0;
  r.curValid = false;
  r.converged = false;
}
static bool ensureBuffers(Rt& r, int n) {
  if (r.cap >= n && r.tgt && r.cur && r.last) return true;
  freeBuffers(r);
  r.tgt = (float*)malloc(sizeof(float) * n * 3);
  r.cur = (float*)malloc(sizeof(float) * n * 3);
  r.last = (uint8_t*)malloc((size_t)n * 4);
  if (!r.tgt || !r.cur || !r.last) { freeBuffers(r); return false; }
  r.cap = n;
  r.lastCap = n * 4;
  return true;
}

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
  filter["leds"]["fps"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) { r.error = "That did not look like a WLED device."; return r; }
  if (doc["leds"]["count"].isNull()) { r.error = "That did not look like a WLED device."; return r; }
  r.leds = doc["leds"]["count"].as<int>();
  r.fps = doc["leds"]["fps"] | 0;
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
    o["host"] = h.length() ? h + ".local" : MDNS.IP(i).toString();      // what to type in the address box: the name keeps working if the IP changes
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

// Keeps a target's address in step with its settings: when the host was edited, the old address is dropped (an IP address is taken at
// once, a name is looked up by the background task). Costs nothing when the settings did not change. Relay task only.
static void syncAddress(int ti) {
  Rt& r = gRt[ti];
  if (r.addrGen == gSettingsGen) return;
  String host;
  { Lock l; host = gSet.t[ti].wHost; }
  r.addrGen = gSettingsGen;
  if (host == r.resolved) return;
  r.resolved = host;
  r.tryMs = 0;
  IPAddress ip;
  const bool literal = ip.fromString(host);
  r.hostIsName = host.length() > 0 && !literal;
  { Lock l; r.ipRaw = literal ? (uint32_t)ip : 0u; r.resolveMs = millis(); }
}

// The address of a target's WLED, or false while it is not known (a name that has not been found yet).
static bool wledAddress(int ti, IPAddress& ip) {
  syncAddress(ti);
  const uint32_t a = gRt[ti].ipRaw;
  if (a == 0) return false;
  ip = IPAddress(a);
  return true;
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

// A frame for a Govee light: one colour per segment (3 bytes each), sent as the "razer" streaming command. The light stretches the segments over
// its whole length. "Streaming on" is sent by the prepare job and repeated here every 10 s in case the light left the mode.
static char gGoveeJson[760];
static void goveeFrameSend(int ti, const uint8_t* rgb, size_t len) {
  IPAddress ip;
  if (gTx < 0 || !wledAddress(ti, ip)) return;
  const size_t jl = fwb::goveeFrameJson(rgb, (int)(len / 3), gGoveeJson, sizeof gGoveeJson);
  if (!jl) return;
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(fwb::kGoveeControlPort);
  to.sin_addr.s_addr = (uint32_t)ip;
  if (sendto(gTx, gGoveeJson, jl, 0, (sockaddr*)&to, sizeof to) < 0) { if (gPerfOn) perfSendFail(); return; }
  Rt& r = gRt[ti];
  const uint32_t now = millis();
  if (now - r.goveeEnableMs >= 10000) {
    r.goveeEnableMs = now;
    char en[100];
    const size_t el = fwb::goveeEnableJson(true, en, sizeof en);
    if (el) sendto(gTx, en, el, 0, (sockaddr*)&to, sizeof to);
  }
}

static void ddpSend(int ti, const uint8_t* payload, size_t len, int ledStart, bool rgbw) {
  if (gSet.t[ti].kind == 1) { goveeFrameSend(ti, payload, len); return; }                  // a Govee light, not a WLED
  IPAddress ip;
  if (gTx < 0 || !wledAddress(ti, ip)) return;
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
static void adjustInto(float out[kZones][3], const float raw[kZones][3]) {
  float br, gm, sat;
  { Lock l; br = gSet.kBri; gm = gSet.kGam; sat = gSet.kSat; }
  for (int z = 0; z < kZones; z++) {
    float c[3] = {raw[z][0] / 255.f, raw[z][1] / 255.f, raw[z][2] / 255.f};
    if (sat != 1.f) {
      float y = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
      for (int k = 0; k < 3; k++) c[k] = clampf(y + (c[k] - y) * sat, 0.f, 1.f);
    }
    if (gm != 1.f) for (int k = 0; k < 3; k++) c[k] = powf(c[k], gm);
    for (int k = 0; k < 3; k++) out[z][k] = clampf(c[k] * br * 255.f, 0.f, 255.f);
  }
}

static void rainbowZones(float z[kZones][3]) {
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

// ---------------------------------------------------------------- WLED on / off, in the background
// Turning a WLED on and handing it back are HTTP calls that can take a second or two (or time out when the WLED is off). They run
// in their own task, one after the other, so a slow or missing WLED never delays the lights on the other targets.
struct WledJob {
  bool prepare = true;
  bool resolveOnly = false;                        // just look the host's address up again (a name whose IP may have changed)
  int target = 0;
  String host;
  bool turnOn = true, restore = true;
  int bri = -1;
  bool explicitSnap = false, snapOn = false;       // restore to these values instead of the ones read when the stream began
  int snapBri = 128;
  bool forceOff = false;                           // (restore) switch the WLED off instead of putting it back how it was: the TV is off
  bool keepSnap = false;                           // (prepare) the WLED was switched off by us: keep the state remembered before, not "off"
  bool govee = false;                              // the target is a Govee light, not a WLED
  int segments = 0;                                // (Govee) its number of segments
  bool probe = false;                              // a delay probe: one round trip to the target (and, for a WLED, now and then its refresh rate)
  bool probeFps = false;
};
static QueueHandle_t gJobs = nullptr;

static bool postJob(WledJob* j) {
  if (!gJobs || xQueueSend(gJobs, &j, 0) != pdTRUE) { delete j; return false; }
  return true;
}

// The address to use for a WLED in a background job: the IP address when the host is (or just resolved to) one, else the host as typed.
// A name is looked up here, in the worker, and the result is published for the relay loop; a name that cannot be found keeps its last
// known address. Returns true when the address changed from a known one (a WLED that moved).
static bool workerAddress(int target, const String& host, bool fresh, String& addr) {
  addr = host;
  uint32_t known = 0;
  uint32_t age = 0;
  {
    Lock l;
    if (target >= gSet.nTargets || gSet.t[target].wHost != host) return false;     // the host was edited meanwhile: the new one is looked after elsewhere
    known = gRt[target].ipRaw;
    age = millis() - gRt[target].resolveMs;
  }
  IPAddress ip;
  if (ip.fromString(host)) { addr = host; return false; }
  if (!fresh && known != 0 && age < kResolveEveryMs) { addr = IPAddress(known).toString(); return false; }
  const bool found = resolveWled(host, ip);
  bool moved = false;
  {
    Lock l;
    Rt& r = gRt[target];
    if (target < gSet.nTargets && gSet.t[target].wHost == host) {
      if (found) { moved = r.ipRaw != 0 && r.ipRaw != (uint32_t)ip; r.ipRaw = (uint32_t)ip; r.resolveMs = millis(); }
      known = r.ipRaw;
    }
  }
  if (known != 0) addr = IPAddress(known).toString();
  return moved;
}

// Preparing / handing back a Govee light (blocking UDP exchanges, see govee.cpp).
static void goveeJob(WledJob* j) {
  IPAddress ip;
  if (!ip.fromString(j->host)) { Lock l; if (gRt[j->target].hostCache == j->host) gRt[j->target].wledOk = false; return; }
  if (j->prepare) {
    GoveeState before;
    const bool ok = goveePrepare(ip, j->turnOn, j->bri, before);
    bool remember = false;
    {
      Lock l;
      Rt& r = gRt[j->target];
      if (r.hostCache == j->host) {
        r.wledOk = ok; r.wledLeds = j->segments; r.wledName = "Govee light";
        if (ok && !(j->keepSnap && r.snapHost == j->host)) { r.snapHost = j->host; r.snapOn = before.on; r.snapBri = before.bri; r.snapR = before.r; r.snapG = before.g; r.snapB = before.b; remember = true; }
      }
    }
    if (remember && j->restore) goveeSessionSave(j->target, j->host, before);      // so a restart while streaming cannot lose what the light was like
  } else {
    GoveeState snap;
    bool haveSnap = false;
    { Lock l; const Rt& r = gRt[j->target]; if (r.snapHost == j->host) { snap.on = r.snapOn; snap.bri = r.snapBri; snap.r = r.snapR; snap.g = r.snapG; snap.b = r.snapB; haveSnap = true; } }
    goveeRestore(ip, j->restore, haveSnap, snap, j->forceOff);
    if (!j->forceOff) goveeSessionClear(j->target);          // handed back (when it was switched off for the TV, the original state is still the one to hand back later)
  }
}

// After a restart: a light that was being streamed to when the board stopped is still as the stream left it (on, at the stream's brightness).
// Hand it back from the record kept in flash, before anything new is prepared (so the next stream takes the right "before" state).
static bool gGoveeBootDone = false;
static void goveeBootHandBack(int nT) {
  gGoveeBootDone = true;
  for (int i = 0; i < kMaxTargets; i++) {
    String host;
    GoveeState s;
    if (!goveeSessionLoad(i, host, s)) continue;
    bool give = false;
    {
      Lock l;
      give = i < nT && gSet.t[i].kind == 1 && gSet.t[i].wHost == host && gSet.t[i].wRestore;
      if (give) { Rt& r = gRt[i]; r.snapHost = host; r.snapOn = s.on; r.snapBri = s.bri; r.snapR = s.r; r.snapG = s.g; r.snapB = s.b; }
    }
    if (!give) { goveeSessionClear(i); continue; }           // the target was changed or removed meanwhile: nothing to hand back
    WledJob* j = new WledJob();
    j->prepare = false; j->target = i; j->host = host; j->restore = true; j->govee = true;
    postJob(j);
  }
}

// ---- delay probes
// The time to open a TCP connection to a port (the connect call returns when the SYN-ACK is back: a round trip through the network and the
// other side's network stack, without waiting for any application). -1 = no answer within timeoutMs.
static float tcpRttMs(const String& addr, uint16_t port, int timeoutMs) {
  IPAddress ip;
  if (!ip.fromString(addr) && !WiFi.hostByName(addr.c_str(), ip)) return -1.f;
  const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s < 0) return -1.f;
  fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = (uint32_t)ip;
  float ms = -1.f;
  const uint32_t t0 = micros();
  const int rc = connect(s, (sockaddr*)&to, sizeof to);
  if (rc == 0) ms = (micros() - t0) / 1000.f;
  else if (errno == EINPROGRESS) {
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (select(s + 1, nullptr, &wf, nullptr, &tv) > 0) {
      int err = 0;
      socklen_t el = sizeof err;
      getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
      if (err == 0) ms = (micros() - t0) / 1000.f;
    }
  }
  close(s);
  return ms;
}

static void probeJob(WledJob* j) {
  float ms = -1.f;
  int fps = 0;
  if (j->govee) {
    IPAddress ip;
    if (ip.fromString(j->host)) ms = goveeRttMs(ip);
  } else {
    String addr;
    workerAddress(j->target, j->host, false, addr);
    ms = tcpRttMs(addr, 80, 800);
    if (j->probeFps && ms >= 0.f) {                                  // WLED's own refresh rate while it is streaming (it changes with the LEDs and the load)
      String body;
      if (httpGet(addr, "/json/info", body)) {
        JsonDocument filter;
        filter["leds"]["fps"] = true;
        JsonDocument doc;
        if (!deserializeJson(doc, body, DeserializationOption::Filter(filter))) fps = doc["leds"]["fps"] | 0;
      }
    }
  }
  Lock l;
  Rt& r = gRt[j->target];
  r.probing = false;
  if (r.hostCache != j->host) return;                                // the target was changed meanwhile: this answer belongs to the old one
  rttAdd(r.rtt, ms);
  r.rttMs = millis();
  if (r.rttMs == 0) r.rttMs = 1;
  if (fps > 0) r.wledFps = fps;
}

static void wledWorker(void*) {
  for (;;) {
    WledJob* j = nullptr;
    if (xQueueReceive(gJobs, &j, portMAX_DELAY) != pdTRUE || !j) continue;
    if (j->probe) { probeJob(j); delete j; continue; }
    if (j->govee) { goveeJob(j); delete j; continue; }
    if (j->resolveOnly) {
      String addr;
      const bool moved = workerAddress(j->target, j->host, true, addr);
      { Lock l; gRt[j->target].resolving = false; }
      if (moved) {                                    // the WLED has a new address: check it is the same device and refresh what the page shows
        WledInfo wi = wledQuery(addr);
        Lock l;
        Rt& r = gRt[j->target];
        if (j->target < gSet.nTargets && gSet.t[j->target].wHost == j->host) {
          r.wledOk = wi.ok;
          if (wi.ok) { r.wledLeds = wi.leds; r.wledName = wi.name; if (wi.fps > 0) r.wledFps = wi.fps; }
        }
      }
    } else if (j->prepare) {
      String addr;
      workerAddress(j->target, j->host, false, addr);
      WledInfo wi = wledQuery(addr);
      {
        Lock l;
        Rt& r = gRt[j->target];
        if (r.hostCache == j->host) {
          r.wledOk = wi.ok;
          if (wi.ok) {
            r.wledLeds = wi.leds; r.wledName = wi.name; if (wi.fps > 0) r.wledFps = wi.fps;
            if (!(j->keepSnap && r.snapHost == j->host)) { r.snapHost = j->host; r.snapOn = wi.on; r.snapBri = wi.bri; }   // after "TV off" the WLED reads as off: that is not what to hand back
          }
        }
      }
      if (wi.ok) {
        String body = "{";
        bool any = false;
        if (j->turnOn && !wi.on) { body += "\"on\":true"; any = true; }
        if (j->bri >= 0 && j->bri != wi.bri) { body += String(any ? "," : "") + "\"bri\":" + String(j->bri); any = true; }
        body += "}";
        if (any) httpPostJson(addr, "/json/state", body);
      }
    } else {
      String addr;
      workerAddress(j->target, j->host, false, addr);
      // WLED keeps showing the last streamed frame until its realtime timeout (about 2.5 s) runs out. {"live":false} makes it leave
      // realtime mode at once, so the lights return the moment we stop streaming.
      bool sOn = j->snapOn;
      int sBri = j->snapBri;
      bool haveSnap = j->explicitSnap;
      if (!haveSnap) {
        Lock l;
        const Rt& r = gRt[j->target];
        if (r.snapHost == j->host) { sOn = r.snapOn; sBri = r.snapBri; haveSnap = true; }
      }
      String body = "{\"live\":false";
      if (j->forceOff) body += ",\"on\":false";                                  // the TV is off: the lights go off too (brightness untouched)
      else if (j->restore && haveSnap) body += ",\"on\":" + String(sOn ? "true" : "false") + ",\"bri\":" + String(sBri);
      httpPostJson(addr, "/json/state", body + "}");
    }
    delete j;
  }
}

// Asks the background task to look a target's name up (again). Relay task only.
static void requestResolve(int i) {
  Rt& r = gRt[i];
  r.tryMs = millis();
  if (r.tryMs == 0) r.tryMs = 1;                  // 0 means "never asked"
  WledJob* j = new WledJob();
  j->prepare = false; j->resolveOnly = true; j->target = i; j->host = r.resolved;
  { Lock l; r.resolving = true; }
  if (!postJob(j)) { Lock l; r.resolving = false; }
}

// Once a second: keep every target's address current. A name is looked up when it has not been found yet (every kRetryResolveMs) and
// again every kResolveEveryMs while it is known, so a WLED that got a new IP address is found again without anyone touching the setup page.
static void housekeeping(int nT) {
  const uint32_t now = millis();
  for (int i = 0; i < nT; i++) {
    Rt& r = gRt[i];
    syncAddress(i);
    // delay tracking: publish the finished 5 s window of frame ages, and send a probe now and then while this target is being streamed to
    if (now - r.ageWinMs >= 5000) {
      Lock l;
      if (r.ageN) { r.pubAgeAvgMs = r.ageSumUs / 1000.f / r.ageN; r.pubAgeMaxMs = r.ageMaxUs / 1000.f; }
      r.ageSumUs = r.ageN = r.ageMaxUs = 0;
      r.ageWinMs = now;
    }
    if (r.prepared && r.hostCache.length() > 0) {
      const bool gv = r.kindCache == 1;
      bool busy2;
      { Lock l; busy2 = r.probing; }
      if (!busy2 && now - r.probeMs >= (gv ? 30000u : 10000u)) {
        r.probeMs = now;
        WledJob* pj = new WledJob();
        pj->probe = true; pj->target = i; pj->host = r.hostCache; pj->govee = gv; pj->probeFps = !gv && (r.probeCount++ % 6 == 0);
        { Lock l; r.probing = true; }
        if (!postJob(pj)) { Lock l; r.probing = false; }
      }
    }
    if (!r.hostIsName) continue;
    bool busy;
    uint32_t known, since;
    { Lock l; busy = r.resolving; known = r.ipRaw; since = now - r.resolveMs; }
    if (busy) continue;
    const bool due = known == 0 ? (r.tryMs == 0 || now - r.tryMs >= kRetryResolveMs) : since >= kResolveEveryMs;
    if (due) requestResolve(i);
  }
}

static void requestPrepare(int i, const String& host, bool turnOn, int bri, bool restore) {
  Rt& r = gRt[i];
  r.prepared = true;                              // even if it fails: do not retry on every frame
  r.hostCache = host;
  r.restoreCache = restore;
  const bool resume = r.offByTv;                  // coming back after "TV off": switch it on whatever the target's setting says
  r.offByTv = false;
  int kind, segs;
  { Lock l; kind = gSet.t[i].kind; segs = gSet.t[i].wCount; }
  r.kindCache = kind;
  if (host.length() == 0) return;
  WledJob* j = new WledJob();
  j->prepare = true; j->target = i; j->host = host; j->turnOn = turnOn || resume; j->bri = bri; j->restore = restore; j->keepSnap = resume;
  j->govee = kind == 1; j->segments = segs;
  postJob(j);
}

// Stop streaming to a target: hand its WLED back (or, when the TV is off, switch it off) and forget the stream state.
static void requestRestore(int i, bool switchOff = false) {
  Rt& r = gRt[i];
  if (r.prepared && r.hostCache.length() > 0) {
    WledJob* j = new WledJob();
    j->prepare = false; j->target = i; j->host = r.hostCache; j->restore = r.restoreCache; j->forceOff = switchOff; j->govee = r.kindCache == 1;
    postJob(j);
    if (switchOff) r.offByTv = true;
  }
  r.prepared = false;
  r.curValid = false;
  r.converged = false;
  r.lastLen = 0;
}

// The WLED was switched off because the TV went off, and "hand the WLEDs back" is what is wanted now (the setting was changed while the TV
// was off): put it back how it was before the stream began.
static void requestHandBackAfterOff(int i) {
  Rt& r = gRt[i];
  r.offByTv = false;
  if (r.hostCache.length() == 0) return;
  WledJob* j = new WledJob();
  j->prepare = false; j->target = i; j->host = r.hostCache; j->restore = r.restoreCache; j->govee = r.kindCache == 1;
  postJob(j);
}

// ---------------------------------------------------------------- receive
// A datagram on the Sync Box port that we cannot read: count it and keep a few raw samples (at most one every half second).
static void noteOddFrame(const uint8_t* d, int len, FrameClass cls, const FrameInfo& fi) {
  const uint32_t now = millis();
  Lock l;
  if (cls == kFrameNewLayout) gStat.newLayout++; else gStat.unreadable++;
  gStat.lastOddMs = now;
  gStat.haveOdd = true;
  if (gOddCount > 0 && now - gOddStoredMs < 500) return;
  OddSample& s = gOdd[gOddHead];
  s.ms = now;
  s.cls = (uint8_t)cls;
  s.checksumOk = fi.checksumOk;
  s.len = (uint16_t)len;
  memcpy(s.data, d, len < (int)sizeof s.data ? len : sizeof s.data);
  gOddHead = (gOddHead + 1) % 4;
  if (gOddCount < 4) gOddCount++;
  gOddStoredMs = now;
}

static String toHex(const uint8_t* d, size_t n) {
  static const char* kHex = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) { s += kHex[d[i] >> 4]; s += kHex[d[i] & 15]; }
  return s;
}

void relayDiagnostics(JsonObject d) {
  Lock l;
  const uint32_t now = millis();
  JsonObject f = d["frames"].to<JsonObject>();
  f["understood"] = gStat.ok;
  f["newLayout"] = gStat.newLayout;
  f["unreadable"] = gStat.unreadable;
  f["lastUnderstoodAgoMs"] = gHave ? (int64_t)(now - gLastFrame) : -1;
  f["lastOtherAgoMs"] = gStat.haveOdd ? (int64_t)(now - gStat.lastOddMs) : -1;
  JsonArray a = d["samples"].to<JsonArray>();
  for (int i = 0; i < gOddCount; i++) {                    // oldest first
    const OddSample& s = gOdd[(gOddHead - gOddCount + i + 8) % 4];
    JsonObject o = a.add<JsonObject>();
    o["ageMs"] = (int64_t)(now - s.ms);
    o["class"] = s.cls == kFrameNewLayout ? "sync box framing, payload not understood" : "unreadable";
    o["len"] = s.len;
    o["checksumOk"] = s.checksumOk;
    o["hex"] = toHex(s.data, s.len < sizeof s.data ? s.len : sizeof s.data);
  }
  if (gGoodLen) {
    JsonObject g = d["lastUnderstood"].to<JsonObject>();
    g["ageMs"] = (int64_t)(now - gGoodSampleMs);
    g["len"] = gGoodLen;
    g["hex"] = toHex(gGoodSample, gGoodLen);
  }
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
  int motionPct, boxSpeed;
  { Lock l; boost = gSet.kBoost; motionPct = gSet.motion; boxSpeed = gSet.boxSpeed; }
  uint8_t buf[300];                                       // longer than any frame seen (86-93 bytes) so a bigger one is not cut short
  for (int guard = 0; guard < 16; guard++) {
    sockaddr_in sa;
    socklen_t sl = sizeof sa;
    const int r = recvfrom(gRx, buf, sizeof buf, 0, (sockaddr*)&sa, &sl);
    if (r <= 0) break;                                   // nothing queued (the socket is non-blocking)
    const IPAddress from((uint32_t)sa.sin_addr.s_addr);
    if ((uint32_t)gBoxFilter != 0 && from != gBoxFilter) continue;
    uint8_t z[kZones][3];
    FrameInfo fi;
    const FrameClass cls = classifyFrame(buf, (size_t)r, z, &fi);
    if (cls != kFrameOk) { noteOddFrame(buf, r, cls, fi); continue; }
    {
      uint32_t now = millis();
      Lock l;
      gStat.ok++;
      if (now - gGoodSampleMs >= 2000 || !gGoodLen) { gGoodLen = (uint8_t)(r < (int)sizeof gGoodSample ? r : sizeof gGoodSample); memcpy(gGoodSample, buf, gGoodLen); gGoodSampleMs = now; }
      if (now - gLastFrame > 1500) { gPrevValid = false; blackReset(gBlack); }   // the box was silent: do not extrapolate across the gap, and forget "black since"
      blackFrame(gBlack, z, now);                                  // TV off detection: black for long enough = the TV is off
      memcpy(gRaw, z, sizeof gRaw);                                // the raw box output (calibration and /api/zones use this)
      gSender = from;
      const int steps = boxStepsForGap(now - gLastFrame);          // the box's smoothing steps since the previous packet (more than one if packets were lost)
      if (gHave && now - gLastFrame < 2000) gIntervalMs = gIntervalMs == 0 ? (now - gLastFrame) : gIntervalMs * 0.9f + (now - gLastFrame) * 0.1f;
      gLastFrame = now;
      gLastFrameUs = micros();
      if (gPerfOn) perfBoxFrame(gLastFrameUs);
      gHave = true;
      gFrames++;
      float f[kZones][3];                                          // what the lights should follow: the box's value, or (boost) the colour it is aiming at
      for (int q = 0; q < kZones; q++)
        for (int c = 0; c < 3; c++) {
          f[q][c] = (boost > 0.f && motionPct == 0 && gPrevValid) ? leadBoost(z[q][c], gPrevY[q][c], boost, boxSpeed, steps) : (float)z[q][c];   // (motion smoothing replaces the boost)
          gPrevY[q][c] = z[q][c];
        }
      gPrevValid = true;
      adjustInto(gAdj, f);
      if (motionPct > 0) motionPush(gMotion, now, gAdj);
      got = true;
    }
  }
  return got;
}

// ---------------------------------------------------------------- finder / calibration frames, markers
static void finishCal(int i);

static void markSendFrame(int i) {
  int st, cnt, n;
  bool whiteMin;
  MarkBlock blocks[8];
  {
    Lock l;
    Rt& r = gRt[i];
    st = gSet.t[i].wStart; cnt = gSet.t[i].wCount; whiteMin = gSet.t[i].whiteMin; n = r.mark.n;
    memcpy(blocks, r.mark.b, sizeof blocks);
    r.mark.dirty = false;
    r.mark.lastSend = millis();
    r.lastLen = 0;
  }
  if (cnt <= 0 || cnt > kMaxLeds) return;
  const int per = whiteMin ? 4 : 3;
  memset(gPayload, 0, (size_t)cnt * per);                      // the whole run dark, only the markers lit
  for (int k = 0; k < n; k++)
    for (int q = blocks[k].start; q < blocks[k].start + blocks[k].count; q++) {
      const int idx = q - st;
      if (idx < 0 || idx >= cnt) continue;
      uint8_t* p = gPayload + (size_t)idx * per;
      p[0] = blocks[k].r; p[1] = blocks[k].g; p[2] = blocks[k].b;
      if (per == 4) p[3] = 0;
    }
  ddpSend(i, gPayload, (size_t)cnt * per, st, whiteMin);
}

static void calSendFrame(int i) {
  int total, start, count;
  bool alt;
  {
    Lock l;
    Cal& c = gRt[i].cal;
    total = c.total; start = c.start; count = c.count; alt = c.alt; c.dirty = false; c.lastSend = millis();
    gRt[i].lastLen = 0;
  }
  if (total <= 0) return;
  memset(gPayload, 0, (size_t)total * 3);
  if (alt) {                                                                  // every LED / segment in turn red and blue: where each one starts and ends
    for (int q = 0; q < total; q++) gPayload[q * 3 + (q % 2 == 0 ? 0 : 2)] = 255;
  } else {
    for (int q = start; q < start + count && q < total; q++) { gPayload[q * 3] = gPayload[q * 3 + 1] = gPayload[q * 3 + 2] = 255; }
  }
  ddpSend(i, gPayload, (size_t)total * 3, 0, false);
}

void relayBegin() {
  openSockets();
  if (!gJobs) {
    gJobs = xQueueCreate(12, sizeof(WledJob*));
    xTaskCreate(wledWorker, "wledjobs", 8192, nullptr, 1, nullptr);
  }
}

bool relayZones(uint8_t zones[kZones][3], uint32_t* ageMs) {
  Lock l;
  if (!gHave) { *ageMs = UINT32_MAX; return false; }
  memcpy(zones, gRaw, sizeof gRaw);
  *ageMs = millis() - gLastFrame;
  return true;
}

void relayStatus(RelayStatus& s, bool detail) {
  Lock l;
  const uint32_t now = millis();
  bool fresh = gHave && now - gLastFrame < 2500;
  s.tvOff = fresh && tvIsOff(gBlack, now, (uint32_t)gSet.tvSecs * 1000u);
  s.tvBlackS = fresh ? (int)(blackForMs(gBlack, now) / 1000u) : 0;
  s.boxFps = (fresh && gIntervalMs > 0) ? 1000.f / gIntervalMs : 0.f;
  s.boxAgeMs = gHave ? now - gLastFrame : UINT32_MAX;
  s.boxSender = (detail && gHave) ? gSender.toString() : String("");
  s.framesOk = gStat.ok;
  s.framesNewLayout = gStat.newLayout;
  s.framesUnreadable = gStat.unreadable;
  s.boxProblem = !fresh && gStat.haveOdd && now - gStat.lastOddMs < kStaleMs;     // nothing readable lately, but something arrived
  s.nTargets = gSet.nTargets;
  bool anyUsable = false, allOk = true, calibrating = false, marking = false, testing = false;
  int leds = 0;
  for (int i = 0; i < gSet.nTargets; i++) {
    const Target& t = gSet.t[i];
    const Rt& r = gRt[i];
    TargetStatus& ts = s.targets[i];
    ts.enabled = t.enabled;
    ts.kind = t.kind;
    ts.ok = r.wledOk;
    ts.leds = r.wledLeds;
    ts.ageAvgMs = r.pubAgeAvgMs; ts.ageMaxMs = r.pubAgeMaxMs;
    { const RttSummary q = rttSummary(r.rtt);
      ts.rttSamples = q.samples; ts.rttLost = q.lost; ts.rttAvgMs = q.avgMs; ts.rttMinMs = q.minMs; ts.rttMaxMs = q.maxMs; }
    ts.rttAgeS = r.rttMs ? (int)((now - r.rttMs) / 1000) : -1;
    ts.showMs = t.kind == 0 ? wledShowMs(r.wledLeds, (float)r.wledFps) : -1.f;
    if (detail) {
      ts.name = t.name; ts.host = t.wHost; ts.wledName = r.wledName;
      const uint32_t a = r.ipRaw;
      if (a != 0) { ts.ip = IPAddress(a).toString(); ts.ipAgeS = (int)((now - r.resolveMs) / 1000); }
    }
    if (r.cal.active) calibrating = true;
    if (r.mark.until && (int32_t)(r.mark.until - now) > 0) marking = true;
    if (r.testUntil && (int32_t)(r.testUntil - now) > 0) testing = true;
    if (t.enabled && t.wHost.length() > 0 && t.wCount > 0) { anyUsable = true; allOk = allOk && r.wledOk; leds += r.wledLeds; }
  }
  s.wledOk = anyUsable && allOk;
  s.wledLeds = leds;
  s.wledName = detail ? gRt[0].wledName : String("");
  if (gUpdating) s.state = "updating firmware";
  else if (calibrating) s.state = "calibrating";
  else if (marking) s.state = "showing markers";
  else if (testing) s.state = "showing test pattern";
  else if (!anyUsable) s.state = "needs setup";
  else if (!gSet.enabled) s.state = "paused";
  else if (!fresh) s.state = s.boxProblem ? "Sync Box frames not understood" : "waiting for the Sync Box";
  else if (s.tvOff && gSet.tvMode != 0) s.state = "TV off";
  else s.state = "relaying";
}

// Blur along a strip's picture part (spanFirst..spanLast; no part set = the whole strip) with a scratch buffer that is allocated the first time it is needed.
static float* gBlurScratch = nullptr;
static int gBlurCap = 0;
static void blurPart(float* rgb, int n, int first, int last, int blur) {
  const int m = (first >= 0 && last >= first) ? ((last < n ? last : n - 1) - first + 1) : n;
  const int need = 2 * (m > 0 ? m : n) * 3;
  if (need > gBlurCap) {
    free(gBlurScratch);
    gBlurScratch = (float*)malloc((size_t)need * sizeof(float));
    gBlurCap = gBlurScratch ? need : 0;
  }
  if (gBlurScratch) blurColours(rgb, n, first, last, blur, gBlurScratch);
}

// ---------------------------------------------------------------- the main relay step
// How the output works: a new Sync Box frame is turned into LED colours and sent to every WLED the moment it arrives (not at the next
// tick), then a tick every 1000/fps ms keeps the temporal smoothing moving until the lights reach the target. The smoothing is
// based on elapsed time (smoothDecay), so the extra frame does not change how it feels. A frame identical to the last one sent
// to a target is not sent again (a keep-alive every second stops WLED leaving realtime mode).
void relayLoop() {
  const bool gotNew = pollBox();
  if (gotNew) { gNewPending = true; for (int i = 0; i < kMaxTargets; i++) gRt[i].converged = false; }
  const uint32_t now = millis();
  gWaitHintMs = 10;
  if (!gGoveeBootDone && WiFi.status() == WL_CONNECTED) { int n0; { Lock l; n0 = gSet.nTargets; } goveeBootHandBack(n0); }   // once, before any stream is prepared

  struct TSnap { bool enabled, whiteMin, turnOn, restore, hostChanged; int start, count, bri; float trim; int kind; float sat; bool vivid; bool darkOut; int spanFirst, spanLast; int blur; };
  TSnap sn[kMaxTargets];
  int nT, fps, tvMode, motionPct;
  bool globalOn, tvOff;
  {
    Lock l;
    nT = gSet.nTargets;
    fps = gSet.fps < 1 ? 30 : gSet.fps;
    globalOn = gSet.enabled;
    tvMode = gSet.tvMode;
    motionPct = gSet.motion;
    tvOff = gHave && now - gLastFrame < kStaleMs && tvIsOff(gBlack, now, (uint32_t)gSet.tvSecs * 1000u);   // the picture has been black long enough
    for (int i = 0; i < nT; i++) {
      const Target& t = gSet.t[i];
      sn[i] = TSnap{t.enabled && t.wHost.length() > 0 && t.wCount > 0, t.whiteMin, t.wTurnOn, t.wRestore, t.wHost != gRt[i].hostCache || t.kind != gRt[i].kindCache, t.wStart, t.wCount, t.wBri, t.trim, t.kind, t.sat, t.vivid, t.darkOutside, t.spanFirst, t.spanLast, t.blur};
    }
  }
  for (int i = nT; i < kMaxTargets; i++) {                  // targets that no longer exist: hand their WLED back and free their memory
    Rt& r = gRt[i];
    if (r.prepared) requestRestore(i);
    if (r.tgt) { Lock l; freeBuffers(r); }
    if (r.cal.active || r.cal.endPending) { Lock l; r.cal = Cal(); if (gCalTarget == i) gCalTarget = -1; }
    r.testUntil = 0; r.mark.until = 0;
  }
  if (now - gHouseMs >= 1000) { gHouseMs = now; housekeeping(nT); }

  // ---- per target: calibration sessions and markers take the target over; the others stream
  bool streaming[kMaxTargets] = {};
  int nStream = 0;
  for (int i = 0; i < nT; i++) {
    Rt& r = gRt[i];
    if (r.prepared && (sn[i].hostChanged || !sn[i].enabled)) requestRestore(i);     // its WLED address changed or it was switched off
    bool calActive, endPending;
    { Lock l; calActive = r.cal.active; endPending = r.cal.endPending; }
    if (endPending) { finishCal(i); continue; }
    if (calActive) {
      bool dirty, wd;
      { Lock l; dirty = r.cal.dirty; wd = now - r.cal.lastCmd > kCalWatchdogMs; }
      if (wd) { calEnd(); continue; }
      if (sn[i].kind == 1 && !r.prepared) {                  // a Govee light: switch it on and into streaming first (a WLED takes the frames as they are)
        String host; { Lock l; host = gSet.t[i].wHost; }
        requestPrepare(i, host, true, 60, sn[i].restore);
      }
      if (dirty || now - r.cal.lastSend > 150) calSendFrame(i);
      continue;
    }
    bool marking, send = false;
    { Lock l; marking = r.mark.until && (int32_t)(r.mark.until - now) > 0;
      if (r.mark.until && !marking) { r.mark.until = 0; r.curValid = false; }          // expired: the picture takes over again
      if (marking) send = r.mark.dirty || now - r.mark.lastSend > 200; }
    if (marking) {
      if (!r.prepared) { String host; { Lock l; host = gSet.t[i].wHost; } requestPrepare(i, host, sn[i].turnOn, sn[i].bri, sn[i].restore); }
      if (send) markSendFrame(i);
      continue;
    }
    if (!sn[i].enabled) continue;
    streaming[i] = true;
    nStream++;
  }
  if (nStream == 0) return;

  // ---- is an output cycle due?
  const uint32_t tickMs = 1000u / (uint32_t)fps;
  const uint32_t since = now - gLastTick;
  bool anyBehind = false;
  for (int i = 0; i < nT; i++)
    if (streaming[i] && (!gRt[i].converged || !gRt[i].curValid || gRt[i].computedGen != gSettingsGen)) anyBehind = true;
  const bool eventDue = gNewPending && since >= kMinGapMs;           // a new picture: send it right away
  const bool tickDue = since >= tickMs && anyBehind;                 // keep smoothing toward the target
  if (!eventDue && !tickDue) {
    for (int i = 0; i < nT; i++) {
      Rt& r = gRt[i];
      if (streaming[i] && r.lastLen && now - r.lastSendMs >= kKeepaliveMs) { ddpSend(i, r.last, r.lastLen, r.lastStart, r.lastRgbw); r.lastSendMs = now; }
    }
    if (gNewPending) gWaitHintMs = since >= kMinGapMs ? 1 : kMinGapMs - since;
    else if (anyBehind) gWaitHintMs = since >= tickMs ? 1 : tickMs - since;
    return;
  }
  gLastTick = now;
  gNewPending = false;

  const uint32_t pf0 = gPerfOn ? micros() : 0;              // performance statistics: time the frame work
  uint32_t sendUs = 0;
  bool sentAny = false, allConverged = true;
  // paused (Home Assistant) = as if the box were silent; so is "the TV is off" unless that was set to do nothing
  const bool tvHold = tvOff && tvMode != 0;
  const bool fresh = globalOn && gHave && now - gLastFrame < kStaleMs && !tvHold && !gUpdating;
  // motion smoothing: one picture for every target, slid between the Sync Box's pictures (a share of one interval behind the newest)
  float zi[kZones][3];
  bool interp = false, motionSettled = true;
  if (motionPct > 0) {
    Lock l;
    if (gMotion.n > 0) { interp = true; motionSettled = motionAt(gMotion, now - (uint32_t)motionDelayMs(gMotion, motionPct), zi); }
  }
  for (int i = 0; i < nT; i++) {
    if (!streaming[i]) continue;
    Rt& r = gRt[i];
    const bool testing = r.testUntil && (int32_t)(r.testUntil - now) > 0;
    if (r.testUntil && !testing) r.testUntil = 0;
    if (!fresh && !testing) {                                // box gone quiet, paused or TV off: give this WLED back (TV off, mode 2: switch it off)
      if (r.prepared) requestRestore(i, globalOn && tvHold && tvMode == 2);
      else if (r.offByTv && globalOn && tvHold && tvMode == 1) requestHandBackAfterOff(i);   // switched off earlier; the setting now says hand back
      continue;
    }
    if (!r.prepared) {
      String host; { Lock l; host = gSet.t[i].wHost; }
      requestPrepare(i, host, sn[i].turnOn, sn[i].bri, sn[i].restore);
    }
    if (testing != r.wasTesting) { r.wasTesting = testing; r.converged = false; }
    // A Govee light takes at most about 12 frames a second (kGoveeGapMs): until its turn comes, do not mix colours it would not get. (The vivid blend
    // alone costs about 6 microseconds per segment.) Its next frame is computed from the picture of that moment.
    if (sn[i].kind == 1 && r.lastLen && !testing && now - r.lastSendMs < kGoveeGapMs) { allConverged = false; continue; }

    // colours of this target's LEDs, smoothed in time, packed
    size_t len = 0;
    bool packed = false;
    const int n = sn[i].count;
    {
      Lock l;
      if (gTableN[i] == n && n > 0 && ensureBuffers(r, n)) {
        float adjR[kZones][3];
        const float(*zones)[3] = interp ? zi : gAdj;
        if (testing) { float raw[kZones][3]; rainbowZones(raw); adjustInto(adjR, raw); zones = adjR; }
        if (r.smoothGen != gSettingsGen) { r.smooth = smoothInfo(gTable[i], n, gSet.smoothOut); r.smoothGen = gSettingsGen; }
        applyTable(gTable[i], n, zones, r.tgt, r.smooth, sn[i].vivid);
        if (sn[i].sat != 1.f) saturateColours(r.tgt, n, sn[i].sat);                  // this target's own saturation, on top of the shared one
        if (sn[i].blur > 0) blurPart(r.tgt, n, sn[i].spanFirst, sn[i].spanLast, sn[i].blur);   // soften the steps between zones along the picture part
        if (sn[i].darkOut) darkOutside(r.tgt, n, sn[i].spanFirst, sn[i].spanLast);   // the part of the strip that is not along the picture stays dark
        r.computedGen = gSettingsGen;
        const float smo = gSet.kSmo;
        const float trim = sn[i].trim;
        const uint32_t nowUs = micros();
        if (!r.curValid) {
          for (int k = 0; k < n * 3; k++) r.cur[k] = r.tgt[k] * trim;
          r.curValid = true;
          r.converged = interp ? motionSettled : true;
        } else if (interp && !testing) {                     // motion smoothing: the picture is already slid in time, no easing on top of it
          for (int k = 0; k < n * 3; k++) r.cur[k] = r.tgt[k] * trim;
          r.converged = motionSettled;
        } else {
          const float decay = smoothDecay(smo, (nowUs - r.smoothUs) / 1000.f, (float)tickMs);
          float worst = 0.f;
          for (int k = 0; k < n * 3; k++) {
            const float d = r.tgt[k] * trim - r.cur[k];
            r.cur[k] += d * (1.f - decay);
            const float left = d * decay;
            if (left > worst) worst = left; else if (-left > worst) worst = -left;
          }
          r.converged = worst < 0.5f;                        // less than half a colour step away: nothing visible left to smooth
        }
        r.smoothUs = nowUs;
        len = packColours(r.cur, n, sn[i].whiteMin);
        packed = true;
      }
    }
    if (!packed) continue;
    if (!r.converged) allConverged = false;
    const bool same = len == r.lastLen && sn[i].start == r.lastStart && sn[i].whiteMin == r.lastRgbw && memcmp(gPayload, r.last, len) == 0;
    if (!same && sn[i].kind == 1 && r.lastLen && now - r.lastSendMs < kGoveeGapMs) { allConverged = false; continue; }   // a Govee light is not flooded: the newest colours go out on the next tick
    if (same && now - r.lastSendMs < kKeepaliveMs) {
      if (gPerfOn) perfSkip();
    } else {
      const uint32_t ps0 = gPerfOn ? micros() : 0;
      ddpSend(i, gPayload, len, sn[i].start, sn[i].whiteMin);
      if (gPerfOn) sendUs += micros() - ps0;
      if (r.ageFrame != gFrames) {                                       // the first packet that carries the newest Sync Box frame to this target
        r.ageFrame = gFrames;
        const uint32_t age = micros() - gLastFrameUs;
        r.ageSumUs += age; r.ageN++;
        if (age > r.ageMaxUs) r.ageMaxUs = age;
      }
      sentAny = true;
      if (r.lastCap >= (int)len && r.last) {
        memcpy(r.last, gPayload, len);
        r.lastLen = len; r.lastStart = sn[i].start; r.lastRgbw = sn[i].whiteMin; r.lastSendMs = now;
      }
    }
  }
  if (gPerfOn && pf0 && sentAny) {
    const uint32_t t1 = micros();
    perfFrame(t1 - pf0, sendUs, t1 - gLastFrameUs, gFrames != gAgeFrames);
  }
  gAgeFrames = gFrames;
  if (!allConverged) gWaitHintMs = tickMs;
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

// ---------------------------------------------------------------- finder / calibration API
// A Govee light: the finder lights its segments. The light is switched on and put into streaming by the relay step below (the same preparation
// the picture uses) and handed back by finishCal(); here the session is only opened.
static String calBeginGovee(int target, const String& host, int segs, int* total) {
  IPAddress ip;
  if (host.length() == 0) return "Set the light's IP address first (on the setup page).";
  if (!ip.fromString(host)) return "A Govee light is addressed by its IP address, for example 192.168.1.60.";
  if (segs < 1) return "Set the number of segments first.";
  if (segs > fwb::kGoveeMaxSegments) segs = fwb::kGoveeMaxSegments;
  Lock l;
  if (gCalTarget >= 0 && gCalTarget != target && gRt[gCalTarget].cal.active) { gRt[gCalTarget].cal.active = false; gRt[gCalTarget].cal.endPending = true; }
  Rt& r = gRt[target];
  r.cal = Cal();
  r.cal.active = true;
  r.cal.total = segs;
  r.cal.dirty = true;
  r.cal.lastCmd = millis();
  strcpy(r.cal.screen, "black");
  r.testUntil = 0;
  gCalTarget = target;
  *total = segs;
  return "";
}

String calBegin(int target, int* total) {
  String host;
  {
    Lock l;
    if (target < 0 || target >= gSet.nTargets) return "There is no such target.";
    if (gSet.t[target].kind == 1) { const String h = gSet.t[target].wHost; const int segs = gSet.t[target].wCount; return calBeginGovee(target, h, segs, total); }
    host = gSet.t[target].wHost;
  }
  if (host.length() == 0) return "Set the WLED address first (on the setup page).";
  WledInfo wi = wledQuery(host);
  if (!wi.ok) return wi.error;
  int tot = wi.leds > kCalMaxLeds ? kCalMaxLeds : wi.leds;
  httpPostJson(host, "/json/state", "{\"on\":true,\"bri\":255}");
  {
    Lock l;
    if (gCalTarget >= 0 && gCalTarget != target && gRt[gCalTarget].cal.active) { gRt[gCalTarget].cal.active = false; gRt[gCalTarget].cal.endPending = true; }
    Rt& r = gRt[target];
    r.cal = Cal();
    r.cal.active = true;
    r.cal.total = tot;
    r.cal.dirty = true;
    r.cal.lastCmd = millis();
    r.cal.snapOn = wi.on;
    r.cal.snapBri = wi.bri;
    strcpy(r.cal.screen, "black");
    r.testUntil = 0;
    r.wledOk = true;
    r.wledLeds = wi.leds;
    r.wledName = wi.name;
    gCalTarget = target;
  }
  *total = tot;
  return "";
}

void calLed(int start, int count, bool alt) {
  Lock l;
  if (gCalTarget < 0 || !gRt[gCalTarget].cal.active) return;
  Cal& c = gRt[gCalTarget].cal;
  c.start = start < 0 ? 0 : start;
  c.count = count < 0 ? 0 : count;
  c.alt = alt;
  c.dirty = true;
  c.lastCmd = millis();
}

void calScreen(const char* mode) {
  Lock l;
  if (gCalTarget < 0 || !gRt[gCalTarget].cal.active) return;
  Cal& c = gRt[gCalTarget].cal;
  if (strcmp(mode, "white") == 0 || strcmp(mode, "black") == 0 || strcmp(mode, "idle") == 0 || strcmp(mode, "squares") == 0) strlcpy(c.screen, mode, sizeof c.screen);
  c.lastCmd = millis();
}

const char* calScreenMode() {
  static char copy[8];
  Lock l;
  strlcpy(copy, (gCalTarget >= 0 && gRt[gCalTarget].cal.active) ? gRt[gCalTarget].cal.screen : "idle", sizeof copy);
  return copy;
}

// Called from the web task (or the watchdog): only flags the end. The relay loop does the work (finishCal).
void calEnd() {
  Lock l;
  if (gCalTarget < 0) return;
  Cal& c = gRt[gCalTarget].cal;
  if (!c.active) return;
  c.active = false;
  c.endPending = true;
  strcpy(c.screen, "idle");
}

static void finishCal(int i) {
  bool snapOn, govee;
  int snapBri, total;
  String host;
  {
    Lock l;
    Cal& c = gRt[i].cal;
    snapOn = c.snapOn; snapBri = c.snapBri; total = c.total; host = gSet.t[i].wHost;
    govee = gSet.t[i].kind == 1;
    c.endPending = false;
    if (gCalTarget == i) gCalTarget = -1;
  }
  if (total > 0) { memset(gPayload, 0, (size_t)total * 3); ddpSend(i, gPayload, (size_t)total * 3, 0, false); }   // switch off what we lit
  if (govee) {                                                  // hand the light back like at the end of a stream (the picture prepares it again if it is still on)
    if (gRt[i].prepared) requestRestore(i);
    Rt& rg = gRt[i];
    rg.curValid = false;
    rg.converged = false;
    rg.lastLen = 0;
    return;
  }
  if (host.length() > 0) {
    WledJob* j = new WledJob();
    j->prepare = false; j->target = i; j->host = host; j->restore = true; j->explicitSnap = true; j->snapOn = snapOn; j->snapBri = snapBri;
    postJob(j);
  }
  Rt& r = gRt[i];
  r.curValid = false;
  r.converged = false;
  r.prepared = false;
  r.lastLen = 0;
}

void testPattern(int target, int seconds) {
  Lock l;
  for (int i = 0; i < gSet.nTargets; i++) {
    if (target >= 0 && i != target) continue;
    Rt& r = gRt[i];
    r.testUntil = seconds > 0 ? millis() + (uint32_t)seconds * 1000UL : 0;
    if (!r.testUntil) r.curValid = false;
  }
}

int zoneMark(int zone, int seconds, int perTarget[kMaxTargets]) {
  for (int i = 0; i < kMaxTargets; i++) perTarget[i] = 0;
  if (zone < 0 || zone >= kZones) return 0;
  Lock l;
  int total = 0;
  for (int i = 0; i < gSet.nTargets && i < kMaxTargets; i++) {
    const Target& t = gSet.t[i];
    if (t.kind != 0 || !t.enabled || !gTable[i] || gTableN[i] != t.wCount) continue;
    int runs[8][2];
    const int n = seconds > 0 ? zoneRuns(gTable[i], gTableN[i], zone, runs, 8) : 0;
    MarkBlock bl[8];
    for (int k = 0; k < n; k++) {
      bl[k] = MarkBlock{t.wStart + runs[k][0], runs[k][1], 255, 255, 255};
      perTarget[i] += runs[k][1];
    }
    total += perTarget[i];
    markShow(i, bl, n, seconds);                      // no run (or 0 seconds) clears what this target was showing
  }
  return total;
}

void markShow(int target, const MarkBlock* blocks, int n, int seconds) {
  Lock l;
  if (target < 0 || target >= gSet.nTargets) return;
  Rt& r = gRt[target];
  if (r.cal.active) return;                      // calibration owns the LEDs
  if (seconds <= 0 || n <= 0) { if (r.mark.until) r.curValid = false; r.mark.until = 0; return; }
  if (n > 8) n = 8;
  for (int i = 0; i < n; i++) r.mark.b[i] = blocks[i];
  r.mark.n = n;
  r.mark.until = millis() + (uint32_t)seconds * 1000UL;
  if (!r.mark.until) r.mark.until = 1;
  r.mark.dirty = true;                           // a running rainbow test resumes by itself when the markers expire
}
