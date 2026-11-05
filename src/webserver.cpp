// Web UI + API. Served over plain HTTP (port 80) for setup and over HTTPS (port 443) for the phone camera page.
// Both servers use the same handlers. Everything is JSON; POSTs must be application/json (blocks cross-site form posts).
#include <WiFi.h>
#include <esp_http_server.h>
#include <esp_system.h>
#include <lwip/sockets.h>

#include "app.h"

// The secure server exists only for the phone's camera (browsers allow camera access on HTTPS only), so it is built only with it.
#if FWB_PHONE_CAL && __has_include(<esp_https_server.h>)
#include <esp_https_server.h>
#include "tls_cert.h"
#define FWB_HAS_HTTPS 1
#endif

// The pages are embedded gzip-compressed (tools/gzip_web.py makes web_gz/<file>.gz from web/<file> at every build; platformio.ini embeds them).
#define ASSET(name)                                                           \
  extern const char name##_start[] asm("_binary_web_gz_" #name "_gz_start"); \
  extern const char name##_end[] asm("_binary_web_gz_" #name "_gz_end");
ASSET(index_html)
ASSET(style_css)
#if FWB_PHONE_CAL
ASSET(calibrate_html)
ASSET(screen_html)
ASSET(analysis_js)
ASSET(calibration_js)
#endif

static httpd_handle_t gHttp = nullptr, gHttps = nullptr;
static String gCert, gKey;

bool webHasHttps() { return gHttps != nullptr; }

// ------------------------------------------------------------------ helpers
static esp_err_t sendBody(httpd_req_t* req, const char* type, const char* body, size_t len, const char* status = nullptr) {
  if (status) httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body, len);
}
static esp_err_t sendJson(httpd_req_t* req, const String& s, const char* status = nullptr) {
  return sendBody(req, "application/json", s.c_str(), s.length(), status);
}
static esp_err_t sendDoc(httpd_req_t* req, JsonDocument& d, const char* status = nullptr) {
  String o;
  serializeJson(d, o);
  return sendJson(req, o, status);
}
static esp_err_t sendError(httpd_req_t* req, const char* status, const String& msg) {
  JsonDocument d;
  d["error"] = msg;
  return sendDoc(req, d, status);
}
static esp_err_t sendOk(httpd_req_t* req) { return sendJson(req, "{\"ok\":true}"); }

static bool readJson(httpd_req_t* req, JsonDocument& doc, String& err) {
  char ct[64] = {0};
  if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof ct) != ESP_OK || !strstr(ct, "application/json")) {
    err = "Content-Type must be application/json";
    return false;
  }
  size_t len = req->content_len;
  if (len == 0) return true;
  if (len > 16384) { err = "Request too large"; return false; }
  char* buf = (char*)malloc(len + 1);
  if (!buf) { err = "Out of memory"; return false; }
  size_t got = 0;
  while (got < len) {
    int r = httpd_req_recv(req, buf + got, len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
    if (r <= 0) { free(buf); err = "Read error"; return false; }
    got += r;
  }
  buf[len] = 0;
  DeserializationError e = deserializeJson(doc, buf, len);
  free(buf);
  if (e) { err = "Invalid JSON"; return false; }
  return true;
}

#define NEED_JSON(doc)                                       \
  JsonDocument doc;                                          \
  {                                                          \
    String _e;                                               \
    if (!readJson(req, doc, _e)) return sendError(req, "400 Bad Request", _e); \
  }

// ------------------------------------------------------------------ pages
// A page is sent compressed (every browser accepts gzip) with an ETag made from its content. The browser keeps its copy but asks every time
// whether it is still current ("no-cache" means "check first", not "do not keep"); when it is, the answer is "304 Not Modified" and no page
// goes over the air. Because the ETag comes from the content, a new firmware (or a rebuilt page) is picked up at once.
static uint32_t assetTag(const char* p, size_t n) {
  uint32_t h = 2166136261u;                                     // FNV-1a
  for (size_t i = 0; i < n; i++) { h ^= (uint8_t)p[i]; h *= 16777619u; }
  return h ? h : 1u;
}
static esp_err_t sendAsset(httpd_req_t* req, const char* type, const char* gz, size_t len, uint32_t& tag) {
  if (!tag) tag = assetTag(gz, len);                           // once per page (a repeat by the other server task gives the same value)
  char etag[16];
  snprintf(etag, sizeof etag, "\"%08x\"", (unsigned)tag);
  httpd_resp_set_hdr(req, "ETag", etag);                       // (the header is sent before this function returns)
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  char had[80] = {0};
  if (httpd_req_get_hdr_value_str(req, "If-None-Match", had, sizeof had) == ESP_OK && strstr(had, etag)) {
    httpd_resp_set_status(req, "304 Not Modified");
    return httpd_resp_send(req, nullptr, 0);
  }
  httpd_resp_set_type(req, type);
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  return httpd_resp_send(req, gz, len);
}
#define PAGE(fn, type, sym) \
  static esp_err_t fn(httpd_req_t* req) { static uint32_t tag = 0; return sendAsset(req, type, sym##_start, (size_t)(sym##_end - sym##_start), tag); }
PAGE(h_index, "text/html; charset=utf-8", index_html)
PAGE(h_style, "text/css", style_css)
#if FWB_PHONE_CAL
PAGE(h_calibrate, "text/html; charset=utf-8", calibrate_html)
PAGE(h_screen_page, "text/html; charset=utf-8", screen_html)
PAGE(h_analysis, "application/javascript", analysis_js)
PAGE(h_calibration, "application/javascript", calibration_js)
#endif

static esp_err_t h_favicon(httpd_req_t* req) {
  httpd_resp_set_status(req, "204 No Content");
  return httpd_resp_send(req, nullptr, 0);
}

static esp_err_t h_404(httpd_req_t* req, httpd_err_code_t) {
  if (wifiIsAp()) {                                    // captive portal: send any stray request to the setup page
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://" FWB_AP_IP_STR "/");
    return httpd_resp_send(req, nullptr, 0);
  }
  httpd_resp_set_status(req, "404 Not Found");
  return httpd_resp_send(req, "Not found", HTTPD_RESP_USE_STRLEN);
}

// ------------------------------------------------------------------ API
static esp_err_t h_status(httpd_req_t* req) {
  RelayStatus s;
  relayStatus(s, true);
  JsonDocument d;
  d["ver"] = FWB_VERSION;
  d["mode"] = wifiIsAp() ? "ap" : "sta";
  d["ssid"] = WiFi.SSID();
  d["ip"] = (wifiIsAp() ? WiFi.softAPIP() : WiFi.localIP()).toString();
  d["hostname"] = FWB_HOSTNAME;
  d["id"] = deviceId();
  d["rssi"] = WiFi.RSSI();
  d["uptime"] = (uint32_t)(millis() / 1000);
  d["state"] = s.state;
  { Lock l; d["enabled"] = gSet.enabled; }
  d["https"] = webHasHttps();
  d["heap"] = ESP.getFreeHeap();
  { const int rr = (int)esp_reset_reason();         // why the board last restarted: a crash, a watchdog or a brownout looks different from a normal restart
    JsonObject rs = d["reset"].to<JsonObject>();
    rs["code"] = rr; rs["reason"] = fwb::resetReasonName(rr); rs["unexpected"] = fwb::resetReasonUnexpected(rr); }
  { MqttStatus m; mqttStatus(m);
    JsonObject mq = d["mqtt"].to<JsonObject>();
    mq["enabled"] = m.enabled; mq["connected"] = m.connected; mq["error"] = m.error; mq["base"] = m.base; }
  JsonObject b = d["box"].to<JsonObject>();
  b["fps"] = s.boxFps;
  b["age"] = s.boxAgeMs == UINT32_MAX ? -1 : (int)s.boxAgeMs;
  b["sender"] = s.boxSender;
  b["host"] = s.boxFilter;                          // "" = any box is accepted; otherwise the address the relay is locked to
  JsonObject ig = b["ignored"].to<JsonObject>();    // readable frames from another address than the lock (the box may have been given a new one)
  ig["n"] = s.boxIgnored; ig["ip"] = s.boxIgnoredIp; ig["agoMs"] = s.boxIgnoredAgoMs;
  b["problem"] = s.boxProblem;                      // frames arrive but none can be read
  b["tvOff"] = s.tvOff;                             // the picture has been completely black for the configured time: the TV is off
  b["blackS"] = s.tvBlackS;                         // seconds it has been black so far (0 = not black)
  JsonObject bf = b["frames"].to<JsonObject>();
  bf["understood"] = s.framesOk;
  bf["newLayout"] = s.framesNewLayout;
  bf["unreadable"] = s.framesUnreadable;
  d["features"]["phoneCal"] = FWB_PHONE_CAL != 0;
  JsonArray ta = d["targets"].to<JsonArray>();
  for (int i = 0; i < s.nTargets; i++) {
    JsonObject o = ta.add<JsonObject>();
    o["name"] = s.targets[i].name;
    o["kind"] = s.targets[i].kind == 1 ? "govee" : "wled";
    o["host"] = s.targets[i].host;
    o["ip"] = s.targets[i].ip;                      // the address in use: for a name, what it currently points to ("" = not found yet)
    o["ipAge"] = s.targets[i].ipAgeS;               // seconds since it was last confirmed (-1 = never)
    o["enabled"] = s.targets[i].enabled;
    o["ok"] = s.targets[i].ok;
    o["leds"] = s.targets[i].leds;
    o["wledName"] = s.targets[i].wledName;
    // delay tracking (docs/LATENCY.md); a field is left out until it has been measured
    const TargetStatus& ts = s.targets[i];
    auto tenth = [](float v) { return ((int)(v * 10.f + 0.5f)) / 10.f; };
    JsonObject la = o["lat"].to<JsonObject>();
    if (ts.ageAvgMs >= 0) { la["ageMs"] = tenth(ts.ageAvgMs); la["ageMaxMs"] = tenth(ts.ageMaxMs); }
    if (ts.rttSamples > 0) {
      la["probes"] = ts.rttSamples; la["lost"] = ts.rttLost; la["probeAgeS"] = ts.rttAgeS;
      if (ts.rttAvgMs >= 0) { la["rttMs"] = tenth(ts.rttAvgMs); la["rttMinMs"] = tenth(ts.rttMinMs); la["rttMaxMs"] = tenth(ts.rttMaxMs); }
    }
    if (ts.showMs >= 0) la["showMs"] = tenth(ts.showMs);
  }
  JsonObject w = d["wled"].to<JsonObject>();       // the first target, under its old name (tools written for one target)
  w["host"] = s.targets[0].host;
  w["ok"] = s.targets[0].ok;
  w["leds"] = s.targets[0].leds;
  w["name"] = s.targets[0].wledName;
  return sendDoc(req, d);
}

// A small, stable summary for the Home Assistant integration (custom_components/fancy2wled). "api" is bumped on breaking changes.
static esp_err_t h_ha(httpd_req_t* req) {
  RelayStatus s;
  relayStatus(s);
  JsonDocument d;
  d["api"] = 1;
  d["id"] = deviceId();
  d["ver"] = FWB_VERSION;
  d["model"] = "AtomS3 Lite";
  d["hostname"] = FWB_HOSTNAME;
  d["state"] = s.state;
  d["uptime"] = (uint32_t)(millis() / 1000);
  d["rssi"] = WiFi.RSSI();
  JsonObject b = d["box"].to<JsonObject>();
  b["connected"] = s.boxFps > 0.f;
  b["fps"] = s.boxFps;
  JsonObject w = d["wled"].to<JsonObject>();       // over all enabled targets
  w["ok"] = s.wledOk;
  w["leds"] = s.wledLeds;
  w["targets"] = s.nTargets;
  { Lock l;
    d["enabled"] = gSet.enabled;
    JsonObject k = d["look"].to<JsonObject>();
    k["brightness"] = gSet.kBri;
    k["gamma"] = gSet.kGam;
    k["saturation"] = gSet.kSat;
    k["smoothing"] = gSet.kSmo;
    k["boost"] = gSet.kBoost;
    k["motion"] = gSet.motion;
    k["boxSpeed"] = gSet.boxSpeed;
    k["smoothOut"] = gSet.smoothOut; }
  return sendDoc(req, d);
}

// For a bug report when the Sync Box changes what it sends: counters, a few raw datagrams we could not read and one that we could.
// Colours and sizes only: no addresses, nothing private.
static esp_err_t h_diag(httpd_req_t* req) {
  JsonDocument d;
  d["format"] = "fancy2wled-diagnostics";
  d["version"] = 1;
  d["firmware"] = FWB_VERSION;
  d["uptimeSeconds"] = (uint32_t)(millis() / 1000);
  { const int rr = (int)esp_reset_reason();         // why the board last restarted (no addresses in this file)
    JsonObject rs = d["lastReset"].to<JsonObject>();
    rs["code"] = rr; rs["reason"] = fwb::resetReasonName(rr); rs["unexpected"] = fwb::resetReasonUnexpected(rr); }
  JsonObject e = d["expected"].to<JsonObject>();
  e["port"] = 19875;
  e["sizes"] = "86-93 bytes";
  e["start"] = "55 aa 5a, then the length byte = size - 5";
  e["checksum"] = "header byte 5 = 167 + the sum of every byte except bytes 3 and 5, mod 256";
  e["payloadStartsWith"] = "06 04 06 after undoing the key (the key is size - 83 bytes at offset 8)";
  relayDiagnostics(d["relay"].to<JsonObject>());
  RelayStatus s;
  relayStatus(s);
  d["state"] = s.state;
  d["boxFps"] = s.boxFps;
  d["how"] = "Attach this file to an issue on the project's GitHub page together with your Sync Box's firmware versions (from its app).";
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"fancy2wled-diagnostics.json\"");
  return sendDoc(req, d);
}

static esp_err_t h_config_get(httpd_req_t* req) {
  JsonDocument d;
  settingsToJson(d);
  return sendDoc(req, d);
}

static esp_err_t h_config_post(httpd_req_t* req) {
  NEED_JSON(doc);
  const bool preview = doc["preview"] | false;      // live preview from the setup page: apply now, save later
  String err = settingsFromJson(doc.as<JsonVariantConst>(), !preview);
  if (err.length()) return sendError(req, "400 Bad Request", err);
  return h_config_get(req);
}

static esp_err_t h_export(httpd_req_t* req) {
  NEED_JSON(doc);
  JsonDocument d;
  settingsExport(d, doc["secrets"] | false);
  return sendDoc(req, d);
}

static esp_err_t h_import(httpd_req_t* req) {
  NEED_JSON(doc);
  String err = settingsImport(doc.as<JsonVariantConst>());
  if (err.length()) return sendError(req, "400 Bad Request", err);
  return h_config_get(req);
}

static esp_err_t h_map(httpd_req_t* req) {
  NEED_JSON(doc);
  const char* hex = doc["hex"] | "";
  String err = tableStoreHex(doc["target"] | 0, doc["start"] | -1, doc["count"] | 0, hex, strlen(hex));
  if (err.length()) return sendError(req, "400 Bad Request", err);
  return sendOk(req);
}

static esp_err_t h_targets(httpd_req_t* req) {
  NEED_JSON(doc);
  String action = doc["action"] | "";
  String err;
  if (action == "add") { int idx = 0; err = targetAdd(&idx); }
  else if (action == "remove") err = targetRemove(doc["index"] | -1);
  else err = "action must be add or remove";
  if (err.length()) return sendError(req, "400 Bad Request", err);
  return h_config_get(req);
}

// The picture map for settings that are being edited but not saved: nothing is stored.
static esp_err_t h_usage(httpd_req_t* req) {
  NEED_JSON(doc);
  float u[fwb::kZones];
  int leds = 0;
  String err = targetUsage(doc["target"] | 0, doc.as<JsonVariantConst>(), u, &leds);
  if (err.length()) return sendError(req, "400 Bad Request", err);
  JsonDocument d;
  d["leds"] = leds;
  JsonArray a = d["zones"].to<JsonArray>();
  for (int z = 0; z < fwb::kZones; z++) a.add(((int)(u[z] * 10.f + 0.5f)) / 10.f);
  return sendDoc(req, d);
}

static esp_err_t h_test(httpd_req_t* req) {
  NEED_JSON(doc);
  String p = doc["pattern"] | "off";
  int secs = doc["seconds"] | 20;
  if (secs < 1) secs = 1;
  if (secs > 3600) secs = 3600;
  testPattern(doc["target"] | -1, p == "rainbow" ? secs : 0);       // no target given: every target
  if (p != "rainbow") settingsRevert();             // test over: forget unsaved preview changes
  return sendOk(req);
}

static esp_err_t h_mark(httpd_req_t* req) {
  NEED_JSON(doc);
  MarkBlock bl[8];
  int n = 0;
  for (JsonVariantConst v : doc["blocks"].as<JsonArrayConst>()) {
    if (n >= 8) break;
    const int s = v["start"] | -1, c = v["count"] | 0;
    if (s < 0 || s > 20000 || c <= 0 || c > 200) continue;
    JsonArrayConst col = v["color"].as<JsonArrayConst>();
    auto ch = [&](size_t i, int def) { int x = col.size() > i ? col[i].as<int>() : def; return (uint8_t)(x < 0 ? 0 : (x > 255 ? 255 : x)); };
    bl[n++] = MarkBlock{s, c, ch(0, 255), ch(1, 0), ch(2, 0)};
  }
  int secs = doc["seconds"] | 8;
  if (secs < 0) secs = 0;
  if (secs > 300) secs = 300;
  markShow(doc["target"] | 0, bl, n, secs);
  return sendOk(req);
}

// Click on a zone of the picture map: light the LEDs that follow it on every WLED (seconds 0 = off).
static esp_err_t h_zonemark(httpd_req_t* req) {
  NEED_JSON(doc);
  const int zone = doc["zone"] | -1;
  if (zone < 0 || zone >= fwb::kZones) return sendError(req, "400 Bad Request", "zone must be 0 to 23");
  int secs = doc["seconds"] | 8;
  if (secs < 0) secs = 0;
  if (secs > 60) secs = 60;
  int per[kMaxTargets];
  const int total = zoneMark(zone, secs, per);
  JsonDocument d;
  d["ok"] = true;
  d["leds"] = total;
  JsonArray a = d["targets"].to<JsonArray>();
  for (int i = 0; i < kMaxTargets; i++) a.add(per[i]);
  return sendDoc(req, d);
}

static esp_err_t h_zones(httpd_req_t* req) {
  uint8_t z[fwb::kZones][3];
  uint32_t age;
  JsonDocument d;
  if (relayZones(z, &age)) {
    d["age"] = age;
    JsonArray a = d["zones"].to<JsonArray>();
    for (int i = 0; i < fwb::kZones; i++) {
      JsonArray c = a.add<JsonArray>();
      c.add(z[i][0]); c.add(z[i][1]); c.add(z[i][2]);
    }
  } else {
    d["age"] = nullptr;
    d["zones"] = nullptr;
  }
  return sendDoc(req, d);
}

#if FWB_PHONE_CAL
static esp_err_t h_screen_get(httpd_req_t* req) {
  JsonDocument d;
  d["mode"] = calScreenMode();
  return sendDoc(req, d);
}
#endif

static esp_err_t h_cal_begin(httpd_req_t* req) {
  NEED_JSON(doc);
  int total = 0;
  String err = calBegin(doc["target"] | 0, &total);
  if (err.length()) return sendError(req, "502 Bad Gateway", err);
  JsonDocument d;
  d["total"] = total;
  return sendDoc(req, d);
}
static esp_err_t h_cal_led(httpd_req_t* req) {
  NEED_JSON(doc);
  calLed(doc["start"] | 0, doc["count"] | 0, doc["alt"] | false);
  return sendOk(req);
}
#if FWB_PHONE_CAL
static esp_err_t h_cal_screen(httpd_req_t* req) {
  NEED_JSON(doc);
  calScreen(doc["mode"] | "idle");
  return sendOk(req);
}
#endif
static esp_err_t h_cal_end(httpd_req_t* req) {
  NEED_JSON(doc);
  calEnd();
  return sendOk(req);
}

static esp_err_t h_wled_discover(httpd_req_t* req) {
  String out;
  wledDiscover(out);
  return sendJson(req, out);
}

// "http%3A%2F%2Fwled.local%2F" -> "http://wled.local/" (the page encodes what was typed in the address box)
static void urlDecode(const char* in, char* out, size_t cap) {
  size_t n = 0;
  auto hex = [](char c) { return c >= '0' && c <= '9' ? c - '0' : (c >= 'a' && c <= 'f' ? c - 'a' + 10 : (c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1)); };
  for (; *in && n + 1 < cap; in++) {
    if (in[0] == '%' && hex(in[1]) >= 0 && hex(in[2]) >= 0) { out[n++] = (char)(hex(in[1]) * 16 + hex(in[2])); in += 2; }
    else out[n++] = in[0] == '+' ? ' ' : in[0];
  }
  out[n] = 0;
}

static esp_err_t h_wled_info(httpd_req_t* req) {
  char q[200] = {0}, enc[160] = {0}, raw[160] = {0}, host[64] = {0};
  if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "host", enc, sizeof enc) != ESP_OK)
    return sendError(req, "400 Bad Request", "host is required");
  urlDecode(enc, raw, sizeof raw);
  if (fwb::normalizeHost(raw, host, sizeof host) < 0) return sendError(req, "400 Bad Request", "WLED address is too long");
  WledInfo i = wledQuery(String(host));
  if (!i.ok) return sendError(req, "502 Bad Gateway", i.error);
  JsonDocument d;
  d["name"] = i.name;
  d["ver"] = i.ver;
  d["leds"] = i.leds;
  d["rgbw"] = i.rgbw;
  d["on"] = i.on;
  d["bri"] = i.bri;
  return sendDoc(req, d);
}

static esp_err_t h_govee_discover(httpd_req_t* req) {
  String out;
  goveeDiscover(out);
  return sendJson(req, out);
}

static esp_err_t h_govee_info(httpd_req_t* req) {
  char q[200] = {0}, enc[160] = {0}, raw[160] = {0}, host[64] = {0};
  if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "host", enc, sizeof enc) != ESP_OK)
    return sendError(req, "400 Bad Request", "host is required");
  urlDecode(enc, raw, sizeof raw);
  if (fwb::normalizeHost(raw, host, sizeof host) < 0) return sendError(req, "400 Bad Request", "The address is too long");
  GoveeInfo i = goveeQuery(String(host));
  if (!i.ok) return sendError(req, "502 Bad Gateway", i.error);
  JsonDocument d;
  d["name"] = i.sku.length() ? "Govee " + i.sku : String("Govee light");
  d["sku"] = i.sku;
  d["device"] = i.device;
  d["wifi"] = i.wifiVer;
  d["on"] = i.on;
  d["bri"] = i.bri;
  return sendDoc(req, d);
}

static esp_err_t h_wifi_scan(httpd_req_t* req) { return sendJson(req, wifiScanJson()); }

static esp_err_t h_wifi_post(httpd_req_t* req) {
  if (!wifiIsAp()) return sendError(req, "403 Forbidden", "Wi-Fi can only be changed from the setup access point. Hold the button for 8 seconds to reset.");
  NEED_JSON(doc);
  String ssid = doc["ssid"] | "", pass = doc["pass"] | "";
  if (!wifiSave(ssid, pass)) return sendError(req, "400 Bad Request", "Enter the network name (and a password of at most 63 characters).");
  return sendOk(req);
}

static esp_err_t h_reboot(httpd_req_t* req) {
  NEED_JSON(doc);
  requestRestart(500);
  return sendOk(req);
}
static esp_err_t h_reset(httpd_req_t* req) {
  NEED_JSON(doc);
  settingsReset();
  requestRestart(500);
  return sendOk(req);
}

// ------------------------------------------------------------------ firmware updates
static esp_err_t h_ota_get(httpd_req_t* req) {
  OtaInfo o;
  otaInfo(o);
  JsonDocument d;
  d["enabled"] = o.enabled;
  d["passSet"] = o.passSet;
  d["version"] = o.version;
  d["slot"] = o.slot;
  d["imageState"] = o.imageState;           // "pending" for the first minute of a freshly updated firmware, then "valid"
  d["maxBytes"] = o.maxBytes;
  d["rolledBack"] = o.rolledBack;           // the previous update did not start properly and was undone
  d["busy"] = otaBusy();
  return sendDoc(req, d);
}

// POST /api/ota: the body is the raw firmware.bin, the header X-Update-Password carries the update password. (A custom header cannot be set by a
// form on another web site, so this cannot be triggered from a page you happen to visit; the Content-Type is not JSON on purpose.)
static esp_err_t h_ota_post(httpd_req_t* req) {
  httpd_resp_set_hdr(req, "Connection", "close");               // the body may stay unread when we refuse: do not reuse the connection
  OtaInfo o;
  otaInfo(o);
  if (!o.enabled) return sendError(req, "403 Forbidden", "Firmware updates are switched off. Set an update password and switch updates on in the setup page first.");
  char pw[100] = {0};
  if (httpd_req_get_hdr_value_str(req, "X-Update-Password", pw, sizeof pw) != ESP_OK) return sendError(req, "401 Unauthorized", "Enter the update password.");
  String stored;
  { Lock l; stored = gSet.otaPass; }
  bool locked = false;
  if (!otaPasswordOk(String(pw), stored, &locked))
    return locked ? sendError(req, "429 Too Many Requests", "Too many wrong passwords. Wait a minute and try again.") : sendError(req, "401 Unauthorized", "Wrong update password.");
  const size_t total = req->content_len;
  if (total < fwb::kImageHeadBytes) return sendError(req, "400 Bad Request", "That file is too small to be a firmware image.");
  if (otaBusy()) return sendError(req, "409 Conflict", "An update is already running.");

  const size_t kChunk = 4096;
  uint8_t* buf = (uint8_t*)malloc(kChunk);
  if (!buf) return sendError(req, "503 Service Unavailable", "Out of memory.");
  auto fail = [&](const char* status, const String& msg) { free(buf); otaUploadAbort(); return sendError(req, status, msg); };

  size_t have = 0, got = 0;
  int idle = 0;
  while (have < fwb::kImageHeadBytes) {                          // the first bytes decide whether this is firmware at all
    const int r = httpd_req_recv(req, (char*)buf + have, kChunk - have);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) { if (++idle > 10) return fail("408 Request Timeout", "The upload stalled."); continue; }
    if (r <= 0) return fail("400 Bad Request", "The upload was interrupted.");
    have += (size_t)r;
    idle = 0;
  }
  String e = otaUploadBegin(total, buf, have);
  if (e.length()) { free(buf); return sendError(req, "400 Bad Request", e); }
  e = otaUploadWrite(buf, have);
  if (e.length()) return fail("500 Internal Server Error", e);
  got = have;
  while (got < total) {
    const size_t want = total - got < kChunk ? total - got : kChunk;
    const int r = httpd_req_recv(req, (char*)buf, want);
    if (r == HTTPD_SOCK_ERR_TIMEOUT) { if (++idle > 10) return fail("408 Request Timeout", "The upload stalled. Nothing was changed."); continue; }
    if (r <= 0) return fail("400 Bad Request", "The upload was interrupted. Nothing was changed.");
    idle = 0;
    e = otaUploadWrite(buf, (size_t)r);
    if (e.length()) return fail("500 Internal Server Error", e);
    got += (size_t)r;
  }
  free(buf);
  e = otaUploadEnd();
  if (e.length()) { otaUploadAbort(); return sendError(req, "400 Bad Request", e); }
  requestRestart(1200);                                         // the answer goes out first, then the board starts the new firmware
  return sendJson(req, "{\"ok\":true,\"restarting\":true}");
}

// ------------------------------------------------------------------ routes
struct Route { const char* uri; httpd_method_t method; esp_err_t (*fn)(httpd_req_t*); };
static const Route kRoutes[] = {
    {"/", HTTP_GET, h_index},
#if FWB_PHONE_CAL
    {"/calibrate", HTTP_GET, h_calibrate},
    {"/screen", HTTP_GET, h_screen_page},
    {"/analysis.js", HTTP_GET, h_analysis},
    {"/calibration.js", HTTP_GET, h_calibration},
#endif
    {"/style.css", HTTP_GET, h_style},
    {"/favicon.ico", HTTP_GET, h_favicon},
    {"/api/status", HTTP_GET, h_status},
    {"/api/ha", HTTP_GET, h_ha},
    {"/api/diag", HTTP_GET, h_diag},
    {"/api/config", HTTP_GET, h_config_get},
    {"/api/config", HTTP_POST, h_config_post},
    {"/api/map", HTTP_POST, h_map},
    {"/api/export", HTTP_POST, h_export},
    {"/api/import", HTTP_POST, h_import},
    {"/api/test", HTTP_POST, h_test},
    {"/api/zones", HTTP_GET, h_zones},
    {"/api/mark", HTTP_POST, h_mark},
    {"/api/zonemark", HTTP_POST, h_zonemark},
#if FWB_PHONE_CAL
    {"/api/screen", HTTP_GET, h_screen_get},
    {"/api/cal/screen", HTTP_POST, h_cal_screen},
#endif
    {"/api/cal/begin", HTTP_POST, h_cal_begin},
    {"/api/cal/led", HTTP_POST, h_cal_led},
    {"/api/cal/end", HTTP_POST, h_cal_end},
    {"/api/targets", HTTP_POST, h_targets},
    {"/api/usage", HTTP_POST, h_usage},
    {"/api/wled/discover", HTTP_GET, h_wled_discover},
    {"/api/wled/info", HTTP_GET, h_wled_info},
    {"/api/govee/discover", HTTP_GET, h_govee_discover},
    {"/api/govee/info", HTTP_GET, h_govee_info},
    {"/api/wifi/scan", HTTP_GET, h_wifi_scan},
    {"/api/wifi", HTTP_POST, h_wifi_post},
    {"/api/ota", HTTP_GET, h_ota_get},
    {"/api/ota", HTTP_POST, h_ota_post},
    {"/api/reboot", HTTP_POST, h_reboot},
    {"/api/reset", HTTP_POST, h_reset},
};

static void registerAll(httpd_handle_t s) {
  for (const Route& r : kRoutes) {
    httpd_uri_t u = {};
    u.uri = r.uri;
    u.method = r.method;
    u.handler = r.fn;
    u.user_ctx = nullptr;
    httpd_register_uri_handler(s, &u);
  }
  httpd_register_err_handler(s, HTTPD_404_NOT_FOUND, h_404);
}

#ifdef FWB_HAS_HTTPS
// ESP-IDF 4.x calls the server certificate field `cacert_pem`; 5.x calls it `servercert`. Support both.
template <typename T>
static auto setServerCert(T& c, const uint8_t* p, size_t n, int) -> decltype(c.servercert, void()) { c.servercert = p; c.servercert_len = n; }
template <typename T>
static void setServerCert(T& c, const uint8_t* p, size_t n, long) { c.cacert_pem = p; c.cacert_len = n; }
#endif

// Every new connection gets TCP_NODELAY. A reply is written in two pieces (headers, then the body); without this the second small piece
// waits until the browser acknowledges the first, which measured about 65 ms of delay on every small reply (/api/status, /api/zones).
static esp_err_t onOpen(httpd_handle_t, int fd) {
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  return ESP_OK;
}

void webBegin() {
  // Few sockets on purpose: lwIP on the ESP32 only has 10 in total (shared with UDP and the WLED client).
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 80;
  c.open_fn = onOpen;
  c.max_open_sockets = 2;
  c.lru_purge_enable = true;
  c.max_uri_handlers = 40;
  c.stack_size = 10240;
  if (httpd_start(&gHttp, &c) == ESP_OK) registerAll(gHttp);
  else Serial.println("HTTP server failed to start");

#ifdef FWB_HAS_HTTPS
  if (!wifiIsAp() && tlsGetCert(gCert, gKey)) {
    httpd_ssl_config_t sc = HTTPD_SSL_CONFIG_DEFAULT();
    setServerCert(sc, (const uint8_t*)gCert.c_str(), gCert.length() + 1, 0);
    sc.prvtkey_pem = (const uint8_t*)gKey.c_str();
    sc.prvtkey_len = gKey.length() + 1;
    sc.httpd.max_open_sockets = 2;
    sc.httpd.lru_purge_enable = true;
    sc.httpd.max_uri_handlers = 40;
    sc.httpd.stack_size = 12288;
    sc.httpd.ctrl_port = 32769;
    if (httpd_ssl_start(&gHttps, &sc) == ESP_OK) registerAll(gHttps);
    else { gHttps = nullptr; Serial.println("HTTPS server failed to start"); }
  }
#endif
}

void webLoop() {}
