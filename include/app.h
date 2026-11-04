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
  int kind = 0;                        // 0 = a WLED, 1 = a Govee light (LAN API, streamed per segment: wCount = number of segments)
  bool enabled = true;                 // false: nothing is streamed to it and its WLED is left alone
  // WLED
  String wHost;
  int wStart = 0, wCount = 0;
  bool wTurnOn = true, wRestore = true, whiteMin = false;
  int wBri = -1;                       // -1 = leave WLED's brightness alone
  float trim = 1.f;                    // this target's brightness relative to the others (0..2)
  int sharp = 0;                       // zone edges: 0 = smooth blend between zones .. 100 = every LED takes the colour of the zone it sits in (a Govee light defaults to 100)
  float sat = 1.f;                     // this target's colour saturation on top of the shared one (0..3)
  bool vivid = false;                  // blend neighbouring zones along the colour wheel instead of mixing them in RGB (a Govee light defaults to on)
  bool darkOutside = false;            // switch off the LEDs / segments outside the TV part (otherwise they hold the colour of the corner zone)
  int blur = 0;                        // blur along the picture part of this strip, in LEDs (0 = off .. 20): softens the steps between zones (blurColours in core.h)
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
  // When the TV is off (the picture is completely black for tvSecs seconds; the Sync Box sends no separate signal, see core.h)
  int tvMode = 1;                      // 0 = do nothing (the "TV on" sensor still works), 1 = hand the WLEDs back (default), 2 = switch the WLEDs off
  int tvSecs = 15;                     // 3..600
  // look (shared by all targets; each target has its own brightness trim)
  float kBri = 1.f, kGam = 1.f, kSat = 1.f, kSmo = 0.5f;
  float kBoost = 0.f;                  // reaction boost 0..1: undo the Sync Box's own smoothing (0 = off)
  int boxSpeed = 40;                   // the box's own "Speed of Diffusion" 10..100 % (how far each packet moves towards the new picture): what Reaction boost undoes; 100 = the box does not smooth
  int motion = 0;                      // motion smoothing 0..100 %: slide between the Sync Box's pictures instead of jumping, shown that share of one picture interval late (0 = off; while on it replaces smoothing over time and the reaction boost)
  int smoothOut = 100;                 // 0 = off .. 100: LEDs beyond the TV settle on their end-corner zone colour (softens the hand-over at the TV edge)
  int fps = 30;
  // Home Assistant over MQTT (discovery): the board connects OUT to the broker
  bool mqttOn = false;
  String mqttHost, mqttUser, mqttPass;
  int mqttPort = 1883;
  String mqttBase;                     // topic prefix; "" = fancy2wled/<last 4 of the board id>
  String mqttDisc = "homeassistant";   // Home Assistant's discovery prefix
  bool perfDebug = false;              // measure and publish performance statistics over MQTT (off = nothing is measured)
  // Firmware updates over Wi-Fi (the setup page's upload and PlatformIO's espota). Off until an update password exists; never exported.
  bool otaOn = false;
  String otaPass;
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
// imported=true (a config file): the firmware-update settings in it are ignored, so a file cannot set the update password.
String settingsFromJson(JsonVariantConst d, bool persist = true, bool imported = false);
void settingsRevert();
// Store a calibrated table for a target (hex: 8 chars per LED = a, b, f, d). Switches that target's layout to "table".
String tableStoreHex(int target, int start, int count, const char* hex, size_t hexLen);
void tableRebuild();                        // regenerate every gTable from the layout settings
bool tableHasStored(int target);
// The picture map of a target: how many of its LEDs follow each zone (for an unsaved edit in `req`, or the saved settings when req is null).
String targetUsage(int target, JsonVariantConst req, float out[fwb::kZones], int* leds);
String targetAdd(int* index);               // new empty target at the end; "" or an error
String targetRemove(int index);             // removes a target (the later ones move up); "" or an error

// ---------------------------------------------------------------- Govee lights (govee.cpp)
struct GoveeState { bool ok = false, on = false; int bri = 0, r = 0, g = 0, b = 0; };     // bri in percent
struct GoveeInfo { bool ok = false, on = false; int bri = 0; String sku, device, wifiVer, error; };
void goveeBegin();
bool goveeStatus(const IPAddress& ip, GoveeState& st);                      // asks the light (blocks up to about 2 s)
bool goveePrepare(const IPAddress& ip, bool turnOn, int briPct, GoveeState& before);   // remembers its state, switches it on, starts streaming
void goveeRestore(const IPAddress& ip, bool restore, bool haveSnap, const GoveeState& snap, bool forceOff);   // stops streaming, puts it back (or off)
void goveeStream(const IPAddress& ip, bool on);
float goveeRttMs(const IPAddress& ip);                                      // one status question to the light and how long the answer took (ms); -1 = no answer within about a second
// The state a light was in before this board took it over, kept in flash for as long as the board streams to it. A board that restarts while
// streaming (update, crash, power cut) cannot hand the light back; without this the next start would take the light's streaming state for its
// original one. At the next start the light is handed back from this record. One record per target number.
bool goveeSessionSave(int target, const String& host, const GoveeState& s);
bool goveeSessionLoad(int target, String& host, GoveeState& s);
void goveeSessionClear(int target);
GoveeInfo goveeQuery(const String& host);                                   // the setup page's "Check this light"
int goveeDiscover(String& jsonOut);                                         // multicast scan, writes {"devices":[...]}

// ---------------------------------------------------------------- firmware updates (ota.cpp)
void otaBegin();                                 // after Wi-Fi is up (does nothing in setup mode)
void otaLoop();                                  // PlatformIO's espota listener, and marking a new firmware as good once it has run for a while
// Is this the update password? Constant time; after 5 wrong tries in a row everything is refused for a minute (`locked` tells which).
bool otaPasswordOk(const String& given, const String& stored, bool* locked = nullptr);
bool otaBusy();                                  // an update is being written
// The web upload, one call per phase (called from the HTTP handler). "" = fine, otherwise the message to show.
String otaUploadBegin(size_t total, const uint8_t* head, size_t headLen);   // checks the image header, opens the next firmware slot
String otaUploadWrite(const uint8_t* data, size_t n);
String otaUploadEnd();                           // checks the marker, closes the slot and chooses it for the next boot; restart afterwards
void otaUploadAbort();
struct OtaInfo { bool enabled, passSet, rolledBack; String version, slot, imageState; size_t maxBytes; };   // imageState: new / pending / valid / invalid / aborted / undefined of the running firmware
void otaInfo(OtaInfo& o);

// ---------------------------------------------------------------- relay
struct TargetStatus {
  bool enabled = true, ok = false; int kind = 0; int leds = 0; String name, host, wledName, ip; int ipAgeS = -1;   // ip: the address in use (a name's current address)
  // Delay tracking (docs/LATENCY.md). -1 = not measured yet.
  float ageAvgMs = -1, ageMaxMs = -1;                                  // a Sync Box frame read -> the packet that carries it handed to the network, over the last few seconds
  float rttAvgMs = -1, rttMinMs = -1, rttMaxMs = -1;                   // probe round trips to this target (a TCP connect to a WLED, a status question to a Govee light)
  int rttSamples = 0, rttLost = 0, rttAgeS = -1;                       // probes in the last 20; how many got no answer; seconds since the latest
  float showMs = -1;                                                   // a WLED: the modelled time to show a received frame (wledShowMs)
};
struct RelayStatus {
  bool tvOff = false;                       // the picture has been black for the configured time (frames are still arriving)
  int tvBlackS = 0;                         // how long it has been black so far, in seconds (0 = not black)
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

void relayUpdateBegin();                    // a firmware update is about to be written: stop streaming and hand the WLEDs back
void relayUpdateEnd();                      // ... it failed: carry on
void relayBegin();
void relayWait(uint32_t maxMs);              // sleep until a Sync Box packet arrives or the relay needs to act (max maxMs)
void relayLoop();
void relayDiagnostics(JsonObject d);                       // counters and a few raw frames, for a bug report (GET /api/diag)
void relayStatus(RelayStatus& s, bool detail = false);    // detail: also names, hosts and the sender (allocates Strings; the web page wants them, the LED and MQTT do not)
bool relayZones(uint8_t zones[fwb::kZones][3], uint32_t* ageMs);   // latest decoded frame
// WLED queries used by the web UI
struct WledInfo { bool ok = false; String name, ver, error; int leds = 0; bool rgbw = false, on = false; int bri = 0; int fps = 0; };   // fps: the refresh rate WLED reports
WledInfo wledQuery(const String& host);
int wledDiscover(String& jsonOut);          // mDNS scan, writes a JSON array

// LED finder and phone calibration sessions (driven through the web API); one target at a time
String calBegin(int target, int* total);    // "" or error
void calLed(int start, int count, bool alt = false);   // light LEDs start..start+count-1 white, or (alt) all of them alternately red and blue
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
