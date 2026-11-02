// Shared declarations for the Fancy2WLED firmware.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <IPAddress.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "core.h"

#define FWB_NAME "Fancy2WLED"
#define FWB_HOSTNAME "fancy2wled"            // http://fancy2wled.local
#define FWB_AP_SSID "Fancy2WLED-Setup"        // setup access point
#define FWB_AP_IP 192, 168, 5, 5              // its address: not .1 (usually a router) and not in the low subnets routers use
#define FWB_AP_IP_STR "192.168.5.5"

#ifndef FWB_VERSION
#define FWB_VERSION "dev"
#endif

// ---------------------------------------------------------------- settings
enum LayoutType { LAYOUT_LINEAR = 0, LAYOUT_LOOP = 1, LAYOUT_TABLE = 2 };

struct Settings {
  // WLED
  String wHost;
  int wStart = 0, wCount = 0;
  bool wTurnOn = true, wRestore = true, whiteMin = false;
  int wBri = -1;                       // -1 = leave WLED's brightness alone
  // layout
  int lType = LAYOUT_LINEAR;
  bool led0Right = false;              // straight run: LED 0 at the right end (top/bottom run) or at the bottom end (left/right run)
  int linEdge = 0;                     // straight run: which side of the TV the LEDs are on: 0 top (ceiling), 1 right, 2 bottom, 3 left
  int spanFirst = -1, spanLast = -1;
  int corner = 0;
  bool ccw = false;
  int sides[4] = {0, 0, 0, 0};         // 0 = automatic
  int shift = 0;
  int aw = 16, ah = 9;
  // relay
  bool enabled = true;                 // false = paused: nothing is streamed and WLED is handed back (Home Assistant switch)
  // look
  float kBri = 1.f, kGam = 1.f, kSat = 1.f, kSmo = 0.5f;
  int smoothOut = 100;                 // 0 = off .. 100: LEDs beyond the TV ease to the average of their nearest zones, reaching it at the strip end
  int fps = 30;
  // Home Assistant over MQTT (discovery): the board connects OUT to the broker
  bool mqttOn = false;
  String mqttHost, mqttUser, mqttPass;
  int mqttPort = 1883;
  String mqttBase;                     // topic prefix; "" = fancy2wled/<last 4 of the board id>
  String mqttDisc = "homeassistant";   // Home Assistant's discovery prefix
  // network
  String boxHost;                      // only accept the broadcast from this IP ("" = anyone)
  String ssid, pass;
};

extern Settings gSet;
extern fwb::Entry gTable[fwb::kMaxLeds];   // per-LED zone blend (calibrated or generated)
extern int gTableN;                         // entries currently in gTable
extern volatile uint32_t gSettingsGen;      // bumped whenever settings change
extern volatile uint32_t gMqttGen;          // bumped when the MQTT settings change (reconnects)
extern SemaphoreHandle_t gLock;

struct Lock {
  Lock() { xSemaphoreTakeRecursive(gLock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(gLock); }
};

void settingsBegin();                       // create lock, load from flash
void settingsSave();
void settingsReset();                       // erase everything incl. Wi-Fi
void settingsToJson(JsonDocument& d);       // GET /api/config
// Partial update from POST /api/config. Returns "" on success or an error message.
// persist=false is a live preview: used immediately but not written to flash (settingsRevert() drops it).
String settingsFromJson(JsonVariantConst d, bool persist = true);
void settingsRevert();
// Store a calibrated table (hex: 6 chars per LED = a, b, f). Switches the layout to "table".
String tableStoreHex(int start, int count, const char* hex, size_t hexLen);
void tableRebuild();                        // regenerate gTable from the layout settings
bool tableHasStored();

// ---------------------------------------------------------------- relay
struct RelayStatus {
  float boxFps = 0;
  uint32_t boxAgeMs = 0;                    // since the last Sync Box frame (UINT32_MAX = never)
  String boxSender;
  const char* state = "starting";
  bool wledOk = false;
  int wledLeds = 0;
  String wledName;
};

void relayBegin();
void relayLoop();
void relayStatus(RelayStatus& s);
bool relayZones(uint8_t zones[fwb::kZones][3], uint32_t* ageMs);   // latest decoded frame
// WLED queries used by the web UI
struct WledInfo { bool ok = false; String name, ver, error; int leds = 0; bool rgbw = false, on = false; int bri = 0; };
WledInfo wledQuery(const String& host);
int wledDiscover(String& jsonOut);          // mDNS scan, writes a JSON array

// calibration (driven by the phone through the web API)
String calBegin(int* total);                // "" or error
void calLed(int start, int count);
void calScreen(const char* mode);
const char* calScreenMode();
void calEnd();
// test pattern ("rainbow" across the picture for `seconds`, 0 = off)
void testPattern(int seconds);
// marker blocks: light these LEDs (the strip's own numbers) in the given colours for `seconds` (0 = clear), the rest of the
// run dark, so the setup page can show where the run / the TV part starts and ends. Ignored during calibration.
struct MarkBlock { int start, count; uint8_t r, g, b; };
void markShow(const MarkBlock* blocks, int n, int seconds);

// ---------------------------------------------------------------- Home Assistant (MQTT)
struct MqttStatus { bool enabled = false, connected = false; String error, base; };
String mqttBaseTopic();                     // the configured topic prefix, or the default
void mqttBegin();
void mqttLoop();                            // call often: (re)connects when settings change, publishes state
void mqttStatus(MqttStatus& s);

// ---------------------------------------------------------------- web / network
void webBegin();                            // start HTTP (+ HTTPS if available)
void webLoop();
bool webHasHttps();

void wifiBegin();                           // joins the saved network or starts the setup access point
void wifiLoop();
bool wifiIsAp();
String deviceId();                          // stable id of this board (12 hex digits from its MAC), used by Home Assistant
String wifiScanJson();
bool wifiSave(const String& ssid, const String& pass);   // store + schedule restart
String wifiSsid();

void ledStatusBegin();
void ledStatusLoop();

void requestRestart(uint32_t inMs = 600);
bool restartDue();
