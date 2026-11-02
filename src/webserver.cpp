// Web UI + API. Served over plain HTTP (port 80) for setup and over HTTPS (port 443) for the phone camera page.
// Both servers use the same handlers. Everything is JSON; POSTs must be application/json (blocks cross-site form posts).
#include <WiFi.h>
#include <esp_http_server.h>

#if __has_include(<esp_https_server.h>)
#include <esp_https_server.h>
#define FWB_HAS_HTTPS 1
#endif

#include "app.h"
#include "tls_cert.h"

#define ASSET(name)                                                    \
  extern const char name##_start[] asm("_binary_web_" #name "_start"); \
  extern const char name##_end[] asm("_binary_web_" #name "_end");
ASSET(index_html)
ASSET(calibrate_html)
ASSET(screen_html)
ASSET(analysis_js)
ASSET(calibration_js)
ASSET(style_css)

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
#define PAGE(fn, type, sym) \
  static esp_err_t fn(httpd_req_t* req) { return sendBody(req, type, sym##_start, (size_t)(sym##_end - sym##_start) - 1); }
PAGE(h_index, "text/html; charset=utf-8", index_html)
PAGE(h_calibrate, "text/html; charset=utf-8", calibrate_html)
PAGE(h_screen_page, "text/html; charset=utf-8", screen_html)
PAGE(h_analysis, "application/javascript", analysis_js)
PAGE(h_calibration, "application/javascript", calibration_js)
PAGE(h_style, "text/css", style_css)

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
  relayStatus(s);
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
  { MqttStatus m; mqttStatus(m);
    JsonObject mq = d["mqtt"].to<JsonObject>();
    mq["enabled"] = m.enabled; mq["connected"] = m.connected; mq["error"] = m.error; mq["base"] = m.base; }
  JsonObject b = d["box"].to<JsonObject>();
  b["fps"] = s.boxFps;
  b["age"] = s.boxAgeMs == UINT32_MAX ? -1 : (int)s.boxAgeMs;
  b["sender"] = s.boxSender;
  JsonObject w = d["wled"].to<JsonObject>();
  { Lock l; w["host"] = gSet.wHost; }
  w["ok"] = s.wledOk;
  w["leds"] = s.wledLeds;
  w["name"] = s.wledName;
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
  JsonObject w = d["wled"].to<JsonObject>();
  w["ok"] = s.wledOk;
  w["leds"] = s.wledLeds;
  { Lock l;
    d["enabled"] = gSet.enabled;
    JsonObject k = d["look"].to<JsonObject>();
    k["brightness"] = gSet.kBri;
    k["gamma"] = gSet.kGam;
    k["saturation"] = gSet.kSat;
    k["smoothing"] = gSet.kSmo;
    k["smoothOut"] = gSet.smoothOut; }
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

static esp_err_t h_map(httpd_req_t* req) {
  NEED_JSON(doc);
  const char* hex = doc["hex"] | "";
  String err = tableStoreHex(doc["start"] | -1, doc["count"] | 0, hex, strlen(hex));
  if (err.length()) return sendError(req, "400 Bad Request", err);
  return sendOk(req);
}

static esp_err_t h_test(httpd_req_t* req) {
  NEED_JSON(doc);
  String p = doc["pattern"] | "off";
  int secs = doc["seconds"] | 20;
  if (secs < 1) secs = 1;
  if (secs > 3600) secs = 3600;
  testPattern(p == "rainbow" ? secs : 0);
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
  markShow(bl, n, secs);
  return sendOk(req);
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

static esp_err_t h_screen_get(httpd_req_t* req) {
  JsonDocument d;
  d["mode"] = calScreenMode();
  return sendDoc(req, d);
}

static esp_err_t h_cal_begin(httpd_req_t* req) {
  NEED_JSON(doc);
  int total = 0;
  String err = calBegin(&total);
  if (err.length()) return sendError(req, "502 Bad Gateway", err);
  JsonDocument d;
  d["total"] = total;
  return sendDoc(req, d);
}
static esp_err_t h_cal_led(httpd_req_t* req) {
  NEED_JSON(doc);
  calLed(doc["start"] | 0, doc["count"] | 0);
  return sendOk(req);
}
static esp_err_t h_cal_screen(httpd_req_t* req) {
  NEED_JSON(doc);
  calScreen(doc["mode"] | "idle");
  return sendOk(req);
}
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

static esp_err_t h_wled_info(httpd_req_t* req) {
  char q[96] = {0}, host[72] = {0};
  if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "host", host, sizeof host) != ESP_OK)
    return sendError(req, "400 Bad Request", "host is required");
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

// ------------------------------------------------------------------ routes
struct Route { const char* uri; httpd_method_t method; esp_err_t (*fn)(httpd_req_t*); };
static const Route kRoutes[] = {
    {"/", HTTP_GET, h_index},
    {"/calibrate", HTTP_GET, h_calibrate},
    {"/screen", HTTP_GET, h_screen_page},
    {"/analysis.js", HTTP_GET, h_analysis},
    {"/calibration.js", HTTP_GET, h_calibration},
    {"/style.css", HTTP_GET, h_style},
    {"/favicon.ico", HTTP_GET, h_favicon},
    {"/api/status", HTTP_GET, h_status},
    {"/api/ha", HTTP_GET, h_ha},
    {"/api/config", HTTP_GET, h_config_get},
    {"/api/config", HTTP_POST, h_config_post},
    {"/api/map", HTTP_POST, h_map},
    {"/api/test", HTTP_POST, h_test},
    {"/api/zones", HTTP_GET, h_zones},
    {"/api/mark", HTTP_POST, h_mark},
    {"/api/screen", HTTP_GET, h_screen_get},
    {"/api/cal/begin", HTTP_POST, h_cal_begin},
    {"/api/cal/led", HTTP_POST, h_cal_led},
    {"/api/cal/screen", HTTP_POST, h_cal_screen},
    {"/api/cal/end", HTTP_POST, h_cal_end},
    {"/api/wled/discover", HTTP_GET, h_wled_discover},
    {"/api/wled/info", HTTP_GET, h_wled_info},
    {"/api/wifi/scan", HTTP_GET, h_wifi_scan},
    {"/api/wifi", HTTP_POST, h_wifi_post},
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

void webBegin() {
  // Few sockets on purpose: lwIP on the ESP32 only has 10 in total (shared with UDP and the WLED client).
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 80;
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
#else
  Serial.println("This build has no HTTPS server; the phone camera page will not work.");
#endif
}

void webLoop() {}
