// Govee lights over the LAN API (UDP). The relay streams colours to them through the "razer" mode (see core.h, goveeFrameJson); this file holds
// the blocking parts that run in the background task or for the setup page: asking a light for its state, switching it on and putting it
// back, a check of one light and a scan for lights.
//   scan / discovery: multicast 239.255.255.250:4001, the lights answer to port 4002 on this board
//   control:          unicast to the light, port 4003 (turn, brightness, colorwc, devStatus, razer)
// The light must have "LAN control" switched on in the Govee Home app. The protocol is plain, unauthenticated UDP and UDP gets lost: control
// commands are sent twice and a state is checked before it is believed (found on a real H61C3: a lone "turn off" was sometimes ignored).
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <lwip/sockets.h>

#include "app.h"
#include "core.h"

static SemaphoreHandle_t gMux = nullptr;             // port 4002 can only be listened on by one operation at a time
static int gCtl = -1;                                // an unbound socket for control commands

void goveeBegin() { if (!gMux) gMux = xSemaphoreCreateMutex(); }

struct GLock {
  GLock() { if (gMux) xSemaphoreTake(gMux, portMAX_DELAY); }
  ~GLock() { if (gMux) xSemaphoreGive(gMux); }
};

static int openReplySocket() {
  int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s < 0) return -1;
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(fwb::kGoveeReplyPort);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(s, (sockaddr*)&a, sizeof a) != 0) { close(s); return -1; }
  return s;
}

static void sendTo(int s, uint32_t addr, uint16_t port, const char* text) {
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(port);
  to.sin_addr.s_addr = addr;
  sendto(s, text, strlen(text), 0, (sockaddr*)&to, sizeof to);
}

// One datagram within the time left; false when nothing (valid) came.
static bool recvJson(int s, JsonDocument& doc, uint32_t* from, uint32_t waitMs) {
  fd_set rf;
  FD_ZERO(&rf);
  FD_SET(s, &rf);
  timeval tv;
  tv.tv_sec = waitMs / 1000;
  tv.tv_usec = (waitMs % 1000) * 1000;
  if (select(s + 1, &rf, nullptr, nullptr, &tv) <= 0) return false;
  char buf[600];
  sockaddr_in sa;
  socklen_t sl = sizeof sa;
  const int n = recvfrom(s, buf, sizeof buf - 1, 0, (sockaddr*)&sa, &sl);
  if (n <= 0) return false;
  buf[n] = 0;
  doc.clear();
  if (deserializeJson(doc, buf)) return false;
  *from = sa.sin_addr.s_addr;
  return true;
}

// A control command to a light, sent twice (UDP gets lost).
static void control(const IPAddress& ip, const String& json) {
  GLock l;
  if (gCtl < 0) gCtl = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (gCtl < 0) return;
  sendTo(gCtl, (uint32_t)ip, fwb::kGoveeControlPort, json.c_str());
  delay(70);
  sendTo(gCtl, (uint32_t)ip, fwb::kGoveeControlPort, json.c_str());
}
static String cmdJson(const char* cmd, const String& data) { return String("{\"msg\":{\"cmd\":\"") + cmd + "\",\"data\":" + data + "}}"; }

void goveeStream(const IPAddress& ip, bool on) {
  char b[100];
  if (!fwb::goveeEnableJson(on, b, sizeof b)) return;
  GLock l;
  if (gCtl < 0) gCtl = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (gCtl < 0) return;
  for (int i = 0; i < 3; i++) { sendTo(gCtl, (uint32_t)ip, fwb::kGoveeControlPort, b); delay(150); }
}

bool goveeStatus(const IPAddress& ip, GoveeState& st) {
  GLock l;
  const int s = openReplySocket();
  if (s < 0) return false;
  JsonDocument doc;
  bool got = false;
  for (int attempt = 0; attempt < 2 && !got; attempt++) {
    sendTo(s, (uint32_t)ip, fwb::kGoveeControlPort, "{\"msg\":{\"cmd\":\"devStatus\",\"data\":{}}}");
    const uint32_t end = millis() + 900;
    while (!got && (int32_t)(end - millis()) > 0) {
      uint32_t from = 0;
      if (!recvJson(s, doc, &from, end - millis()) || from != (uint32_t)ip) continue;
      if (String(doc["msg"]["cmd"] | "") != "devStatus") continue;
      JsonVariantConst d = doc["msg"]["data"];
      st.on = (d["onOff"] | 0) != 0;
      st.bri = d["brightness"] | 0;
      st.r = d["color"]["r"] | 0; st.g = d["color"]["g"] | 0; st.b = d["color"]["b"] | 0;
      st.ok = got = true;
    }
  }
  close(s);
  return got;
}

// Put the light on or off and look until it says so (the first command is sometimes lost). Returns whether it was confirmed.
static bool setOnOff(const IPAddress& ip, bool on) {
  for (int attempt = 0; attempt < 5; attempt++) {
    control(ip, cmdJson("turn", String("{\"value\":") + (on ? 1 : 0) + "}"));
    delay(1500);
    GoveeState st;
    if (goveeStatus(ip, st) && st.on == on) return true;
  }
  return false;
}

bool goveePrepare(const IPAddress& ip, bool turnOn, int briPct, GoveeState& before) {
  bool ok = goveeStatus(ip, before);
  for (int k = 0; k < 2 && !ok; k++) { delay(400); ok = goveeStatus(ip, before); }     // UDP gets lost: without this state the light could not be put back later
  if (turnOn && !(ok && before.on)) { control(ip, cmdJson("turn", "{\"value\":1}")); delay(300); }
  if (briPct >= 1 && briPct <= 100) { control(ip, cmdJson("brightness", String("{\"value\":") + briPct + "}")); delay(300); }
  goveeStream(ip, true);
  return ok;
}

void goveeRestore(const IPAddress& ip, bool restore, bool haveSnap, const GoveeState& snap, bool forceOff) {
  goveeStream(ip, false);
  delay(800);
  if (forceOff) { setOnOff(ip, false); return; }
  if (!restore || !haveSnap) return;
  // a scene (rainbow, music...) cannot be restored over the LAN: the light returns to the last plain colour it had
  control(ip, cmdJson("turn", "{\"value\":1}")); delay(300);
  control(ip, cmdJson("colorwc", String("{\"color\":{\"r\":") + snap.r + ",\"g\":" + snap.g + ",\"b\":" + snap.b + "},\"colorTemInKelvin\":0}")); delay(300);
  if (snap.bri >= 1) { control(ip, cmdJson("brightness", String("{\"value\":") + snap.bri + "}")); delay(300); }
  if (!snap.on) setOnOff(ip, false);
}

// ---------------------------------------------------------------- the state before the stream, kept across restarts
// Its own flash namespace (not the settings one): it is written from the background task. A record is written when the stream starts and
// removed when the light has been handed back, so it is written a few times a day at most.
static const char* kSessNs = "fwbgv";
static String sk(char c, int t) { return String(c) + String(t); }

bool goveeSessionSave(int t, const String& host, const GoveeState& s) {
  Preferences p;
  if (!p.begin(kSessNs, false)) return false;
  const int rgb = ((s.r & 255) << 16) | ((s.g & 255) << 8) | (s.b & 255);
  const int on = s.on ? 1 : 0;
  if (p.getString(sk('h', t).c_str(), "") != host) p.putString(sk('h', t).c_str(), host);
  if (p.getInt(sk('o', t).c_str(), -1) != on) p.putInt(sk('o', t).c_str(), on);
  if (p.getInt(sk('b', t).c_str(), -1) != s.bri) p.putInt(sk('b', t).c_str(), s.bri);
  if (p.getInt(sk('c', t).c_str(), -1) != rgb) p.putInt(sk('c', t).c_str(), rgb);
  p.end();
  return true;
}

bool goveeSessionLoad(int t, String& host, GoveeState& s) {
  Preferences p;
  if (!p.begin(kSessNs, false)) return false;                         // (read-write: opening a namespace that does not exist yet read-only logs an error)
  host = p.getString(sk('h', t).c_str(), "");
  const bool have = host.length() > 0 && p.isKey(sk('o', t).c_str());
  if (have) {
    s.on = p.getInt(sk('o', t).c_str(), 0) != 0;
    s.bri = p.getInt(sk('b', t).c_str(), 0);
    const int rgb = p.getInt(sk('c', t).c_str(), 0);
    s.r = (rgb >> 16) & 255; s.g = (rgb >> 8) & 255; s.b = rgb & 255;
    s.ok = true;
  }
  p.end();
  return have;
}

void goveeSessionClear(int t) {
  Preferences p;
  if (!p.begin(kSessNs, false)) return;
  if (p.isKey(sk('h', t).c_str())) {
    p.remove(sk('h', t).c_str()); p.remove(sk('o', t).c_str()); p.remove(sk('b', t).c_str()); p.remove(sk('c', t).c_str());
  }
  p.end();
}

// ---------------------------------------------------------------- the setup page
static void sendScan(int s, uint32_t unicastTo) {
  const char* scan = "{\"msg\":{\"cmd\":\"scan\",\"data\":{\"account_topic\":\"reserve\"}}}";
  sendTo(s, (uint32_t)IPAddress(239, 255, 255, 250), fwb::kGoveeScanPort, scan);
  if (unicastTo) sendTo(s, unicastTo, fwb::kGoveeScanPort, scan);
}

int goveeDiscover(String& out) {
  JsonDocument d;
  JsonArray a = d["devices"].to<JsonArray>();
  uint32_t seen[12];
  int n = 0;
  {
    GLock l;
    const int s = openReplySocket();
    if (s >= 0) {
      sendScan(s, 0);
      const uint32_t end = millis() + 3000;
      JsonDocument doc;
      while ((int32_t)(end - millis()) > 0 && n < 12) {
        uint32_t from = 0;
        if (!recvJson(s, doc, &from, end - millis())) continue;
        if (String(doc["msg"]["cmd"] | "") != "scan") continue;
        JsonVariantConst x = doc["msg"]["data"];
        const String ipStr = x["ip"] | IPAddress(from).toString().c_str();
        bool dup = false;
        for (int i = 0; i < n; i++) if (seen[i] == from) dup = true;
        if (dup) continue;
        seen[n++] = from;
        JsonObject o = a.add<JsonObject>();
        o["name"] = String("Govee ") + (const char*)(x["sku"] | "light");
        o["ip"] = ipStr;
        o["host"] = ipStr;                                               // what to type in the address box
        o["device"] = x["device"] | "";
      }
      close(s);
    }
  }
  serializeJson(d, out);
  return n;
}

GoveeInfo goveeQuery(const String& host) {
  GoveeInfo r;
  IPAddress ip;
  if (host.length() == 0) { r.error = "Enter the light's IP address first."; return r; }
  if (!ip.fromString(host)) { r.error = "A Govee light is addressed by its IP address (for example 192.168.1.60)."; return r; }
  GoveeState st;
  const bool got = goveeStatus(ip, st);
  if (got) { r.ok = true; r.on = st.on; r.bri = st.bri; }
  {
    GLock l;                                                             // the model and versions come with the answer to a scan
    const int s = openReplySocket();
    if (s >= 0) {
      sendScan(s, (uint32_t)ip);
      const uint32_t end = millis() + 2500;
      JsonDocument doc;
      while ((int32_t)(end - millis()) > 0) {
        uint32_t from = 0;
        if (!recvJson(s, doc, &from, end - millis()) || from != (uint32_t)ip) continue;
        if (String(doc["msg"]["cmd"] | "") != "scan") continue;
        JsonVariantConst x = doc["msg"]["data"];
        r.sku = x["sku"] | "";
        r.device = x["device"] | "";
        r.wifiVer = x["wifiVersionSoft"] | "";
        r.ok = true;
        break;
      }
      close(s);
    }
  }
  if (!r.ok) r.error = "The light at " + host + " did not answer. Check the address, and that LAN control is switched on for it in the Govee Home app.";
  return r;
}
