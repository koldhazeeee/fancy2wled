// Home Assistant over MQTT. The board connects OUT to your broker, announces itself with MQTT discovery (so it appears
// in Home Assistant as a "Fancy2WLED" device with no YAML) and then publishes its state / accepts commands.
//
//   <base>/status            online / offline (retained, last-will)
//   <base>/state             JSON with everything the entities need (every ~10 s and on change)
//   <base>/perf              JSON with performance statistics, every 5 s, ONLY while "Performance stats (debug)" is switched on
//   <base>/sync/set          ON / OFF       pause or resume the sync
//   <base>/look/<name>/set   a number       brightness, gamma, saturation, smoothing, smooth_out
//   <base>/target/<n>/set        ON / OFF   stream to target n (0-based) or not
//   <base>/target/<n>/trim/set   a number   that target's brightness, 0-150 % (Home Assistant's light slider uses 0-100)
//   <base>/button/<name>/press   PRESS      rainbow, stop_test, restart
//   <discovery>/<component>/<id>/<object>/config   discovery messages (retained)
//
// The MQTT client runs in its own task, so a slow or unreachable broker never disturbs the relay.
#include <WiFi.h>
#include <mqtt_client.h>

#include "app.h"

static esp_mqtt_client_handle_t gClient = nullptr;
static volatile bool gConnected = false;
static String gError;
static uint32_t gGen = 0xFFFFFFFF;
static String gBase, gDisc, gId, gClientId, gLwtTopic, gHost, gUser, gPass;
static volatile bool gForce = true;
static uint32_t gLastCheck = 0, gLastForce = 0;
static String gLastKey;

String mqttBaseTopic() {
  Lock l;
  if (gSet.mqttBase.length()) return gSet.mqttBase;
  String id = deviceId();
  id.toLowerCase();
  return "fancy2wled/" + id.substring(id.length() - 4);
}

static String T(const String& s) { return gBase + "/" + s; }

static void pub(const String& topic, const String& payload, bool retain, int qos = 0) {
  if (gClient) esp_mqtt_client_enqueue(gClient, topic.c_str(), payload.c_str(), (int)payload.length(), qos, retain ? 1 : 0, true);
}

// ------------------------------------------------------------------ state
// The targets as far as Home Assistant cares: how many, whether each is on, and its brightness trim.
static String targetsKey() {
  String k = "|" + String(gSet.nTargets);
  for (int i = 0; i < gSet.nTargets; i++) k += String(gSet.t[i].enabled ? "1" : "0") + (int)(gSet.t[i].trim * 100.f + 0.5f) + ",";
  return k;
}

// What makes the published state change: cheap to build, compared 5 times a second.
static String stateKey(const RelayStatus& s) {
  Lock l;
  return String(s.state) + (s.boxProblem ? "P" : "") + (s.boxFps > 0.f ? "1" : "0") + (s.wledOk ? "1" : "0") + (gSet.enabled ? "1" : "0") + (int)(gSet.kBri * 100.f + 0.5f) + "," + gSet.kGam + "," +
         (int)(gSet.kSat * 100.f + 0.5f) + "," + (int)(gSet.kSmo * 100.f + 0.5f) + "," + gSet.smoothOut + "," + (int)(gSet.kBoost * 100.f + 0.5f) + targetsKey();
}

static String stateJson(const RelayStatus& s) {
  JsonDocument d;
  d["state"] = s.state;
  d["box_connected"] = s.boxFps > 0.f;
  d["box_problem"] = s.boxProblem;
  d["frames_odd"] = s.framesNewLayout + s.framesUnreadable;
  d["box_fps"] = ((int)(s.boxFps * 10.f + 0.5f)) / 10.f;
  d["wled_ok"] = s.wledOk;
  d["wled_leds"] = s.wledLeds;
  d["rssi"] = WiFi.RSSI();
  d["uptime"] = (uint32_t)(millis() / 1000);
  int bri, sat, smo, out, boo;
  float gam;
  bool en;
  { Lock l;
    en = gSet.enabled;
    bri = (int)(gSet.kBri * 100.f + 0.5f); sat = (int)(gSet.kSat * 100.f + 0.5f); smo = (int)(gSet.kSmo * 100.f + 0.5f); boo = (int)(gSet.kBoost * 100.f + 0.5f);
    out = gSet.smoothOut; gam = gSet.kGam; }
  d["enabled"] = en;
  d["brightness"] = bri > 100 ? 100 : bri;      // the Home Assistant control is 0-100 % (the setup page can go higher)
  d["gamma"] = gam;
  d["saturation"] = sat;
  d["smoothing"] = smo;
  d["boost"] = boo;
  d["smooth_out"] = out;
  d["relaying"] = String(s.state) == "relaying";
  { Lock l;
    JsonArray ta = d["targets"].to<JsonArray>();
    for (int i = 0; i < gSet.nTargets; i++) {
      JsonObject o = ta.add<JsonObject>();
      o["on"] = gSet.t[i].enabled;
      o["trim"] = (int)(gSet.t[i].trim * 100.f + 0.5f);
      o["ok"] = s.targets[i].ok;
    } }
  String o;
  serializeJson(d, o);
  return o;
}

// ------------------------------------------------------------------ discovery
static void discover(const char* comp, const char* obj, JsonDocument& d) {
  d["unique_id"] = gId + "_" + obj;
  d["availability_topic"] = T("status");
  d["payload_available"] = "online";
  d["payload_not_available"] = "offline";
  JsonObject dev = d["device"].to<JsonObject>();
  dev["identifiers"].to<JsonArray>().add("fancy2wled_" + gId);
  dev["name"] = "Fancy2WLED";
  dev["manufacturer"] = "Fancy2WLED";
  dev["model"] = "AtomS3 Lite relay";
  dev["sw_version"] = FWB_VERSION;
  dev["configuration_url"] = "http://" + WiFi.localIP().toString() + "/";
  String out;
  serializeJson(d, out);
  pub(gDisc + "/" + comp + "/" + gId + "/" + obj + "/config", out, true, 1);
}

static void withState(JsonDocument& d, const String& tmpl) {
  d["state_topic"] = T("state");
  d["value_template"] = tmpl;
}

static void number(const char* obj, const char* name, float mn, float mx, float step, const char* unit, const char* tmpl) {
  JsonDocument d;
  d["name"] = name;
  withState(d, tmpl);
  d["command_topic"] = T(String("look/") + obj + "/set");
  d["min"] = mn;
  d["max"] = mx;
  d["step"] = step;
  if (unit && *unit) d["unit_of_measurement"] = unit;
  d["entity_category"] = "config";
  d["icon"] = "mdi:tune";
  discover("number", obj, d);
}

static void button(const char* obj, const char* name, const char* icon, const char* devClass, bool diagnostic) {
  JsonDocument d;
  d["name"] = name;
  d["command_topic"] = T(String("button/") + obj + "/press");
  d["payload_press"] = "PRESS";
  d["icon"] = icon;
  if (devClass) d["device_class"] = devClass;
  if (diagnostic) d["entity_category"] = "config";
  discover("button", obj, d);
}

static void binary(const char* obj, const char* name, const char* devClass, const char* field) {
  JsonDocument d;
  d["name"] = name;
  withState(d, String("{{ 'ON' if value_json.") + field + " else 'OFF' }}");
  d["device_class"] = devClass;
  discover("binary_sensor", obj, d);
}

// Entities that earlier versions announced and that no longer exist: an empty retained message makes Home Assistant remove them.
static void removeOld(const char* comp, const char* obj) {
  pub(gDisc + "/" + comp + "/" + gId + "/" + obj + "/config", "", true, 1);
}

// ---- performance statistics (debug switch): sensors exist in Home Assistant only while the switch is on
struct PerfEnt { const char* obj; const char* name; const char* unit; const char* field; const char* devClass; };
static const PerfEnt kPerf[] = {
    {"perf_cpu0", "CPU core 0 load", "%", "cpu0", nullptr},
    {"perf_cpu1", "CPU core 1 load", "%", "cpu1", nullptr},
    {"perf_loop_busy", "Main loop busy", "%", "loop_busy", nullptr},
    {"perf_loop_max", "Main loop longest pass", "ms", "loop_max_ms", nullptr},
    {"perf_frame_avg", "Frame work (average)", "ms", "frame_avg_ms", nullptr},
    {"perf_frame_max", "Frame work (longest)", "ms", "frame_max_ms", nullptr},
    {"perf_out_fps", "Frames sent to WLED", "fps", "out_fps", nullptr},
    {"perf_age_avg", "Delay to WLED (average)", "ms", "age_avg_ms", nullptr},
    {"perf_age_max", "Delay to WLED (longest)", "ms", "age_max_ms", nullptr},
    {"perf_box_gap_max", "Sync Box frame gap (longest)", "ms", "box_gap_max_ms", nullptr},
    {"perf_heap_free", "Free memory", "B", "heap_free", "data_size"},
    {"perf_heap_min", "Lowest free memory since boot", "B", "heap_min", "data_size"},
    {"perf_heap_big", "Largest free memory block", "B", "heap_big", "data_size"},
    {"perf_stack_loop", "Main task stack left", "B", "stack_loop", "data_size"},
    {"perf_stack_mqtt", "MQTT task stack left", "B", "stack_mqtt", "data_size"},
};

static void perfDiscovery(bool announce) {
  for (const PerfEnt& e : kPerf) {
    if (!announce) { removeOld("sensor", e.obj); continue; }
    JsonDocument d;
    d["name"] = e.name;
    d["state_topic"] = T("perf");
    d["value_template"] = String("{{ value_json.") + e.field + " }}";
    d["unit_of_measurement"] = e.unit;
    d["state_class"] = "measurement";
    d["entity_category"] = "diagnostic";
    if (e.devClass) { d["device_class"] = e.devClass; d["suggested_unit_of_measurement"] = "kB"; }
    d["icon"] = "mdi:speedometer";
    discover("sensor", e.obj, d);
  }
}

static String perfJson(const PerfSnapshot& p) {
  JsonDocument d;
  auto r1 = [](float v) { return ((int)(v * 10.f + 0.5f)) / 10.f; };
  auto r3 = [](float v) { return ((int)(v * 1000.f + 0.5f)) / 1000.f; };
  d["cpu0"] = r1(p.cpu0);
  d["cpu1"] = r1(p.cpu1);
  d["loop_busy"] = r1(p.loopBusy);
  d["loop_max_ms"] = r3(p.loopMaxMs);
  d["frame_avg_ms"] = r3(p.frameAvgMs);
  d["frame_max_ms"] = r3(p.frameMaxMs);
  d["out_fps"] = r1(p.outFps);
  d["age_avg_ms"] = r1(p.ageAvgMs);
  d["age_max_ms"] = r1(p.ageMaxMs);
  d["box_gap_avg_ms"] = r1(p.boxGapAvgMs);
  d["box_gap_max_ms"] = r1(p.boxGapMaxMs);
  d["send_avg_ms"] = r3(p.sendAvgMs);
  d["send_max_ms"] = r3(p.sendMaxMs);
  JsonArray gh = d["gap_hist"].to<JsonArray>();
  for (int i = 0; i < 11; i++) gh.add(p.gapHist[i]);
  d["skipped"] = p.skipped;
  d["send_fails"] = p.sendFails;
  d["heap_free"] = p.heapFree;
  d["heap_min"] = p.heapMin;
  d["heap_big"] = p.heapBig;
  d["stack_loop"] = p.stackLoop;
  d["stack_mqtt"] = p.stackMqtt;
  d["window_s"] = r1(p.windowS);
  String o;
  serializeJson(d, o);
  return o;
}

static bool perfSetting() { Lock l; return gSet.perfDebug; }

// One light per target: on / off is "stream to this target", the brightness slider is the target's brightness compared with the
// others (0-100 %; the setup page can go up to 150 %, shown as 100 here). Lights of targets that no longer exist are removed again,
// and so are the separate switch and brightness number that earlier versions announced.
static void targetDiscovery() {
  int n;
  String names[kMaxTargets];
  { Lock l; n = gSet.nTargets; for (int i = 0; i < n; i++) names[i] = gSet.t[i].name; }
  for (int i = 0; i < kMaxTargets; i++) {
    const String obj = "target_" + String(i), tr = "target_" + String(i) + "_trim";
    removeOld("switch", obj.c_str());
    removeOld("number", tr.c_str());
    if (i >= n) { removeOld("light", obj.c_str()); continue; }
    JsonDocument d;
    d["name"] = names[i];
    d["state_topic"] = T("state");
    d["state_value_template"] = "{{ 'ON' if value_json.targets[" + String(i) + "].on else 'OFF' }}";
    d["command_topic"] = T("target/" + String(i) + "/set");
    d["payload_on"] = "ON";
    d["payload_off"] = "OFF";
    d["brightness_state_topic"] = T("state");
    d["brightness_value_template"] = "{{ [value_json.targets[" + String(i) + "].trim, 100] | min }}";
    d["brightness_command_topic"] = T("target/" + String(i) + "/trim/set");
    d["brightness_scale"] = 100;
    d["icon"] = "mdi:led-strip-variant";
    discover("light", obj.c_str(), d);
  }
}
static void publishDiscovery() {
  perfDiscovery(perfSetting());                 // announce the debug sensors, or remove them again when the switch is off
  targetDiscovery();
  removeOld("switch", "sync");                  // the Sync switch became a light (below)
  removeOld("number", "brightness");            // ... whose brightness slider replaces the separate Brightness number
  {  // the sync as a light: on / off pauses and resumes it, the brightness slider is the Look brightness (0-100 %)
    JsonDocument d;
    d["name"] = "Sync";
    d["state_topic"] = T("state");
    d["state_value_template"] = "{{ 'ON' if value_json.enabled else 'OFF' }}";
    d["command_topic"] = T("sync/set");
    d["payload_on"] = "ON";
    d["payload_off"] = "OFF";
    d["brightness_state_topic"] = T("state");
    d["brightness_value_template"] = "{{ value_json.brightness }}";
    d["brightness_command_topic"] = T("look/brightness/set");
    d["brightness_scale"] = 100;
    d["icon"] = "mdi:television-ambient-light";
    discover("light", "sync", d);
  }
  {  // what the relay is doing
    JsonDocument d;
    d["name"] = "State";
    withState(d, "{{ value_json.state }}");
    d["device_class"] = "enum";
    JsonArray o = d["options"].to<JsonArray>();
    for (const char* s : {"relaying", "waiting for the Sync Box", "Sync Box frames not understood", "paused", "needs setup", "calibrating", "showing markers", "showing test pattern", "starting"}) o.add(s);
    d["icon"] = "mdi:information-outline";
    discover("sensor", "state", d);
  }
  binary("sync_box", "Sync Box", "connectivity", "box_connected");
  binary("box_problem", "Sync Box frames not understood", "problem", "box_problem");     // ON: the box is sending, but nothing can be read (a firmware change?)
  {
    JsonDocument d;
    d["name"] = "Frames not understood";
    withState(d, "{{ value_json.frames_odd }}");
    d["state_class"] = "total_increasing";
    d["entity_category"] = "diagnostic";
    d["icon"] = "mdi:alert-circle-outline";
    discover("sensor", "frames_odd", d);
  }
  binary("wled", "WLED", "connectivity", "wled_ok");
  binary("relaying", "Relaying", "running", "relaying");
  {
    JsonDocument d;
    d["name"] = "Sync Box frame rate";
    withState(d, "{{ value_json.box_fps }}");
    d["unit_of_measurement"] = "fps";
    d["state_class"] = "measurement";
    d["suggested_display_precision"] = 1;
    d["icon"] = "mdi:speedometer";
    discover("sensor", "box_fps", d);
  }
  {
    JsonDocument d;
    d["name"] = "Wi-Fi signal";
    withState(d, "{{ value_json.rssi }}");
    d["device_class"] = "signal_strength";
    d["unit_of_measurement"] = "dBm";
    d["state_class"] = "measurement";
    d["entity_category"] = "diagnostic";
    discover("sensor", "rssi", d);
  }
  {
    JsonDocument d;
    d["name"] = "Uptime";
    withState(d, "{{ value_json.uptime }}");
    d["device_class"] = "duration";
    d["unit_of_measurement"] = "s";
    d["entity_category"] = "diagnostic";
    discover("sensor", "uptime", d);
  }
  number("gamma", "Gamma", 0.6f, 2.4f, 0.05f, "", "{{ value_json.gamma }}");
  number("saturation", "Saturation", 0, 200, 5, "%", "{{ value_json.saturation }}");
  number("smoothing", "Smoothing over time", 0, 90, 5, "%", "{{ value_json.smoothing }}");
  number("boost", "Reaction boost", 0, 100, 5, "%", "{{ value_json.boost }}");
  number("smooth_out", "Smooth beyond the TV", 0, 100, 5, "%", "{{ value_json.smooth_out }}");
  button("rainbow", "Rainbow test", "mdi:looks", nullptr, false);
  button("stop_test", "Stop test", "mdi:stop", nullptr, false);
  button("restart", "Restart", "mdi:restart", "restart", true);
}

// ------------------------------------------------------------------ commands
// Parse a whole payload as a number ("banana" or "12abc" are rejected, not read as 0).
static bool parseNumber(const String& s, float* out) {
  String t = s;
  t.trim();
  if (t.length() == 0 || t.length() > 16) return false;
  char* end = nullptr;
  const float v = strtof(t.c_str(), &end);
  if (!end || *end != 0 || isnan(v) || isinf(v)) return false;
  *out = v;
  return true;
}

// Same ranges as the Home Assistant entities announce (the light's brightness is 0-100); anything outside them is ignored.
static bool applyLook(const String& key, const String& payload) {
  float v;
  if (!parseNumber(payload, &v)) return false;
  JsonDocument d;
  JsonObject look = d["look"].to<JsonObject>();
  if (key == "brightness" && v >= 0.f && v <= 100.f) look["brightness"] = v / 100.f;
  else if (key == "gamma" && v >= 0.6f && v <= 2.4f) look["gamma"] = v;
  else if (key == "saturation" && v >= 0.f && v <= 200.f) look["saturation"] = v / 100.f;
  else if (key == "smoothing" && v >= 0.f && v <= 90.f) look["smoothing"] = v / 100.f;
  else if (key == "boost" && v >= 0.f && v <= 100.f) look["boost"] = v / 100.f;
  else if (key == "smooth_out" && v >= 0.f && v <= 100.f) look["smoothOut"] = v;
  else return false;
  return settingsFromJson(d.as<JsonVariantConst>(), true).length() == 0;
}

static void onCommand(const String& topic, const String& payload) {
  if (!topic.startsWith(gBase + "/")) return;
  const String rel = topic.substring(gBase.length() + 1);
  if (rel == "sync/set") {
    String p = payload;
    p.trim();
    const bool on = p.equalsIgnoreCase("ON") || p == "1" || p.equalsIgnoreCase("true");
    const bool off = p.equalsIgnoreCase("OFF") || p == "0" || p.equalsIgnoreCase("false");
    if (!on && !off) return;                              // anything else is ignored, never read as "off"
    JsonDocument d;
    d["relay"]["enabled"] = on;
    settingsFromJson(d.as<JsonVariantConst>(), true);
  } else if (rel.startsWith("target/")) {                   // target/<n>/set (ON / OFF) and target/<n>/trim/set (0-150)
    const String r = rel.substring(7);
    const int slash = r.indexOf('/');
    if (slash < 1) return;
    const String num = r.substring(0, slash), tail = r.substring(slash + 1);
    const int idx = num.toInt();
    if (String(idx) != num) return;
    { Lock l; if (idx < 0 || idx >= gSet.nTargets) return; }
    JsonDocument d;
    d["target"] = idx;
    if (tail == "set") {
      String p = payload;
      p.trim();
      const bool on = p.equalsIgnoreCase("ON") || p == "1" || p.equalsIgnoreCase("true");
      const bool off = p.equalsIgnoreCase("OFF") || p == "0" || p.equalsIgnoreCase("false");
      if (!on && !off) return;
      d["enabled"] = on;
    } else if (tail == "trim/set") {
      float v;
      if (!parseNumber(payload, &v) || v < 0.f || v > 150.f) return;
      d["trim"] = v / 100.f;
    } else {
      return;
    }
    if (settingsFromJson(d.as<JsonVariantConst>(), true).length()) return;
  } else if (rel.startsWith("look/") && rel.endsWith("/set")) {
    if (!applyLook(rel.substring(5, rel.length() - 4), payload)) return;
  } else if (rel.startsWith("button/") && rel.endsWith("/press")) {
    const String b = rel.substring(7, rel.length() - 6);
    if (b == "rainbow") testPattern(-1, 60);
    else if (b == "stop_test") { testPattern(-1, 0); settingsRevert(); }
    else if (b == "restart") requestRestart(800);
    else return;
  } else {
    return;
  }
  gForce = true;
}

// ------------------------------------------------------------------ client
static void onEvent(void*, esp_event_base_t, int32_t id, void* data) {
  esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)data;
  switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
      gConnected = true;
      gError = "";
      pub(T("status"), "online", true, 1);
      publishDiscovery();
      esp_mqtt_client_subscribe(gClient, T("sync/set").c_str(), 1);
      esp_mqtt_client_subscribe(gClient, T("look/+/set").c_str(), 1);
      esp_mqtt_client_subscribe(gClient, T("target/+/set").c_str(), 1);
      esp_mqtt_client_subscribe(gClient, T("target/+/trim/set").c_str(), 1);
      esp_mqtt_client_subscribe(gClient, T("button/+/press").c_str(), 1);
      gForce = true;
      break;
    }
    case MQTT_EVENT_DISCONNECTED:
      gConnected = false;
      if (gError.length() == 0) gError = "Disconnected from the broker";
      break;
    case MQTT_EVENT_ERROR:
      gConnected = false;
      if (ev && ev->error_handle) {
        if (ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
          const int c = ev->error_handle->connect_return_code;
          gError = (c == MQTT_CONNECTION_REFUSE_BAD_USERNAME || c == MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED) ? "The broker refused the user name / password"
                                                                                                          : "The broker refused the connection (code " + String(c) + ")";
        } else {
          gError = "Cannot reach the broker (check the address, the port and the firewall)";
        }
      }
      break;
    case MQTT_EVENT_DATA:
      if (ev && ev->topic && ev->current_data_offset == 0 && ev->total_data_len == ev->data_len) {   // commands are tiny: one fragment
        String topic, payload;
        topic.concat(ev->topic, ev->topic_len);
        payload.concat(ev->data, ev->data_len);
        onCommand(topic, payload);
      }
      break;
    default:
      break;
  }
}

static void stopClient() {
  if (gClient) {
    if (gConnected) {                                    // a clean disconnect does not trigger the last-will, so announce it
      esp_mqtt_client_publish(gClient, gLwtTopic.c_str(), "offline", 7, 1, 1);
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    esp_mqtt_client_stop(gClient);
    esp_mqtt_client_destroy(gClient);
    gClient = nullptr;
  }
  gConnected = false;
}

static void startClient() {
  stopClient();
  bool on;
  int port;
  {
    Lock l;
    on = gSet.mqttOn && gSet.mqttHost.length() > 0;
    gHost = gSet.mqttHost; gUser = gSet.mqttUser; gPass = gSet.mqttPass; gDisc = gSet.mqttDisc; port = gSet.mqttPort;
  }
  gBase = mqttBaseTopic();
  gId = deviceId();
  gError = "";
  gLastKey = "";
  if (!on) return;
  gClientId = "fancy2wled-" + gId;
  gLwtTopic = T("status");
  esp_mqtt_client_config_t cfg = {};
  cfg.host = gHost.c_str();
  cfg.port = (uint32_t)port;
  cfg.transport = MQTT_TRANSPORT_OVER_TCP;
  cfg.client_id = gClientId.c_str();
  cfg.username = gUser.length() ? gUser.c_str() : nullptr;
  cfg.password = gUser.length() ? gPass.c_str() : nullptr;
  cfg.lwt_topic = gLwtTopic.c_str();
  cfg.lwt_msg = "offline";
  cfg.lwt_msg_len = 7;
  cfg.lwt_qos = 1;
  cfg.lwt_retain = 1;
  cfg.keepalive = 30;
  cfg.buffer_size = 2048;
  cfg.out_buffer_size = 2048;
  cfg.task_stack = 9216;
  cfg.reconnect_timeout_ms = 10000;
  gClient = esp_mqtt_client_init(&cfg);
  if (!gClient) { gError = "Could not start the MQTT client"; return; }
  esp_mqtt_client_register_event(gClient, MQTT_EVENT_ANY, onEvent, nullptr);
  esp_mqtt_client_start(gClient);
}

void mqttBegin() { gGen = 0xFFFFFFFF; }

static uint32_t gLastPerf = 0;
static String gTargetSig;                                    // the targets' names as last announced to Home Assistant

// The names and number of targets: when they change the per-target entities are announced again.
static String targetSig() {
  Lock l;
  String s = String(gSet.nTargets);
  for (int i = 0; i < gSet.nTargets; i++) s += "|" + gSet.t[i].name;
  return s;
}

void mqttLoop() {
  if (gGen != gMqttGen) { gGen = gMqttGen; startClient(); }
  const bool wantPerf = gClient && gConnected && perfSetting();     // measured only while the switch is on AND the broker is reachable
  perfSync(wantPerf);
  if (!gClient || !gConnected) return;
  const uint32_t now = millis();
  { const String sig = targetSig();
    if (sig != gTargetSig) { if (gTargetSig.length()) publishDiscovery(); gTargetSig = sig; } }
  if (wantPerf && now - gLastPerf >= 5000) {
    gLastPerf = now;
    PerfSnapshot p;
    perfTake(p);
    pub(T("perf"), perfJson(p), false, 0);
  }
  if (now - gLastCheck < 200) return;                     // look for a change 5 times a second (building the state JSON is not free)
  gLastCheck = now;
  RelayStatus rs;
  relayStatus(rs);
  const String key = stateKey(rs);
  const bool changed = key != gLastKey;
  if (gForce || changed || now - gLastForce > 10000) {
    pub(T("state"), stateJson(rs), false, 0);
    gLastKey = key;
    gLastForce = now;
    gForce = false;
  }
}

void mqttStatus(MqttStatus& s) {
  Lock l;
  s.enabled = gSet.mqttOn;
  s.connected = gConnected;
  s.error = gError;
  s.base = mqttBaseTopic();
}
