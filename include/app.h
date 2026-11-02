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

// The camera-based phone calibration (the /calibrate page, the TV test screen and the secure web server it needs) is built only when this
// is 1: build with `pio run -e atoms3-phonecal`. Without it the firmware is smaller and keeps more memory free. The LED finder and the
// red/blue markers on the setup page never needed the camera and are always there.
#ifndef FWB_PHONE_CAL
#define FWB_PHONE_CAL 0
#endif

// ---------------------------------------------------------------- settings
enum LayoutType { LAYOUT_LINEAR = 0, LAYOUT_LOOP = 1, LAYOUT_TABLE = 2 };

constexpr int kMaxTargets = 4;       // WLED devices (or separate runs on one WLED) the relay can stream to
constexpr int kMaxTotalLeds = 2000;  // LEDs over all targets together

// One place the picture is streamed to: a run of LEDs on a WLED, with its own layout (which part of the picture it follows).
struct Target {
  String name;                         // shown on the setup page ("TV strip", "Cabinet")
  bool enabled = true;                 // false: nothing is streamed to it and its WLED is left alone
  // WLED
  String wHost;
  int wStart = 0, wCount = 0;
  bool wTurnOn = true, wRestore = true, whiteMin = false;
  int wBri = -1;                       // -1 = leave WLED's brightness alone
  float trim = 1.f;                    // this target's brightness relative to the others (0..2)
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
};

struct Settings {
  Target t[kMaxTargets];
  int nTargets = 1;
  // relay
  bool enabled = true;                 // false = paused: nothing is streamed and WLED is handed back (Home Assistant switch)
  // look (shared by all targets; each target has its own brightness trim)
  float kBri = 1.f, kGam = 1.f, kSat = 1.f, kSmo = 0.5f;
  float kBoost = 0.f;                  // reaction boost 0..1: undo the Sync Box's own smoothing (0 = off)
  int smoothOut = 100;                 // 0 = off .. 100: LEDs beyond the TV settle on their end-corner zone colour (softens the hand-over at the TV edge)
  int fps = 30;
  // Home Assistant over MQTT (discovery): the board connects OUT to the broker
  bool mqttOn = false;
  String mqttHost, mqttUser, mqttPass;
  int mqttPort = 1883;
  String mqttBase;                     // topic prefix; "" = fancy2wled/<last 4 of the board id>
  String mqttDisc = "homeassistant";   // Home Assistant's discovery prefix
  bool perfDebug = false;              // measure and publish performance statistics over MQTT (off = nothing is measured)
  // network
  String boxHost;                      // only accept the broadcast from this IP ("" = anyone)
  String ssid, pass;
};

extern Settings gSet;
extern fwb::Entry* gTable[kMaxTargets];    // per-LED zone blend of each target (calibrated or generated); null when the target has no LEDs
extern int gTableN[kMaxTargets];           // entries in each gTable
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
// Everything worth moving to another board, as a self-describing document (POST /api/export): the settings, the calibrated
// table and the MQTT settings. Never the Wi-Fi credentials; the MQTT password only when `secrets` is true.
void settingsExport(JsonDocument& d, bool secrets);
// Loads such a document (POST /api/import). Returns "" on success or an error message.
String settingsImport(JsonVariantConst doc);
// Partial update from POST /api/config. Returns "" on success or an error message.
// persist=false is a live preview: used immediately but not written to flash (settingsRevert() drops it).
String settingsFromJson(JsonVariantConst d, bool persist = true);
void settingsRevert();
// Store a calibrated table for a target (hex: 8 chars per LED = a, b, f, d). Switches that target's layout to "table".
String tableStoreHex(int target, int start, int count, const char* hex, size_t hexLen);
void tableRebuild();                        // regenerate every gTable from the layout settings
bool tableHasStored(int target);
// The picture map of a target: how many of its LEDs follow each zone (for an unsaved edit in `req`, or the saved settings when req is null).
String targetUsage(int target, JsonVariantConst req, float out[fwb::kZones], int* leds);
String targetAdd(int* index);               // new empty target at the end; "" or an error
String targetRemove(int index);             // removes a target (the later ones move up); "" or an error

// ---------------------------------------------------------------- relay
struct TargetStatus { bool enabled = true, ok = false; int leds = 0; String name, host, wledName; };
struct RelayStatus {
  float boxFps = 0;
  uint32_t boxAgeMs = 0;                    // since the last Sync Box frame (UINT32_MAX = never)
  String boxSender;
  const char* state = "starting";
  bool wledOk = false;                      // every enabled target's WLED answered
  int wledLeds = 0;                         // LEDs over all targets
  String wledName;                          // first target's WLED name
  int nTargets = 0;
  TargetStatus targets[kMaxTargets];
  // what arrives on the Sync Box port (see classifyFrame in core.h)
  uint32_t framesOk = 0, framesNewLayout = 0, framesUnreadable = 0;
  bool boxProblem = false;                  // frames arrive but none can be read: the box probably changed what it sends
};

void relayBegin();
void relayWait(uint32_t maxMs);              // sleep until a Sync Box packet arrives or the relay needs to act (max maxMs)
void relayLoop();
void relayDiagnostics(JsonObject d);                       // counters and a few raw frames, for a bug report (GET /api/diag)
void relayStatus(RelayStatus& s, bool detail = false);    // detail: also names, hosts and the sender (allocates Strings; the web page wants them, the LED and MQTT do not)
bool relayZones(uint8_t zones[fwb::kZones][3], uint32_t* ageMs);   // latest decoded frame
// WLED queries used by the web UI
struct WledInfo { bool ok = false; String name, ver, error; int leds = 0; bool rgbw = false, on = false; int bri = 0; };
WledInfo wledQuery(const String& host);
int wledDiscover(String& jsonOut);          // mDNS scan, writes a JSON array

// LED finder and phone calibration sessions (driven through the web API); one target at a time
String calBegin(int target, int* total);    // "" or error
void calLed(int start, int count);
void calScreen(const char* mode);           // phone calibration only (the TV test screen)
const char* calScreenMode();
void calEnd();
// test pattern ("rainbow" across the picture for `seconds`, 0 = off) on one target, or on every target when target < 0
void testPattern(int target, int seconds);
// marker blocks: light these LEDs (the strip's own numbers) of a target in the given colours for `seconds` (0 = clear), the rest of
// the run dark, so the setup page can show where the run / the TV part starts and ends. Ignored during calibration.
struct MarkBlock { int start, count; uint8_t r, g, b; };
void markShow(int target, const MarkBlock* blocks, int n, int seconds);

// ---------------------------------------------------------------- Home Assistant (MQTT)
struct MqttStatus { bool enabled = false, connected = false; String error, base; };
// ---------------------------------------------------------------- performance statistics (debug; see perf.cpp)
struct PerfSnapshot {
  float cpu0 = 0, cpu1 = 0;                // % of the time each core was busy
  float loopBusy = 0, loopMaxMs = 0;       // % of the time the main loop spends working; its longest pass (ms)
  float frameAvgMs = 0, frameMaxMs = 0;    // time to build and send one frame to WLED
  float outFps = 0, windowS = 0;           // frames sent per second; length of the measuring window
  float boxGapAvgMs = 0, boxGapMaxMs = 0;  // time between Sync Box frames (source cadence and Wi-Fi jitter)
  float sendAvgMs = 0, sendMaxMs = 0;      // time to hand one frame to the network stack
  float ageAvgMs = 0, ageMaxMs = 0;        // delay from reading a Sync Box frame to the first packet to WLED that carries it
  uint16_t gapHist[11] = {0};              // Sync Box frame gaps: <5, <20, <40, <60, <80, <100, <120, <160, <200, <300, >=300 ms
  uint32_t skipped = 0, sendFails = 0;     // output frames not sent because they were identical to the last; packets the stack refused
  uint32_t heapFree = 0, heapMin = 0, heapBig = 0;   // bytes: free now, lowest since boot, largest free block
  uint32_t stackLoop = 0, stackMqtt = 0;   // bytes of stack the main task / MQTT task have never used
};
extern volatile bool gPerfOn;              // true while statistics are being collected
void perfSync(bool wanted);                // start / stop collecting (starts or stops the sampling timers)
void perfLoopBegin();                      // call only when gPerfOn
void perfLoopEnd();
void perfFrame(uint32_t workUs, uint32_t sendUs, uint32_t ageUs, bool firstOfBoxFrame);
void perfBoxFrame(uint32_t nowUs);         // a Sync Box frame was read
void perfSkip();                           // an output frame identical to the last one was not sent
void perfSendFail();                       // the network stack refused a packet
void perfTake(PerfSnapshot& p);            // statistics since the last call; starts a new window

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
