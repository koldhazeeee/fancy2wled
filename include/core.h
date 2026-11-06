// Portable core of Fancy2WLED (Sync Box -> WLED): frame decoding, zone->LED mapping, DDP packets.
// No Arduino / ESP-IDF dependencies, so it also builds and is tested on a PC (see test/).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace fwb {

constexpr int kZones = 24;      // 6 columns x 4 rows, zone = row * 6 + col (row 0 = top, col 0 = left)
constexpr int kCols = 6;
constexpr int kRows = 4;
constexpr uint16_t kBoxPort = 19875;
constexpr int kMaxLeds = 1000;  // per-LED table size limit

// ---- firmware updates (OTA) -----------------------------------------------
// What an uploaded file has to look like before the board starts writing it to flash. An ESP32 app image starts with a header
// (offset 0: magic 0xE9, 12: the chip id as 16 bits little endian) followed by the first segment, which begins with the app descriptor
// (offset 32: magic 0xABCD5432). A bootloader, a partition table, a merged image or a text file fail one of these checks.
enum ImageCheck { kImageOk = 0, kImageTooShort, kImageBadMagic, kImageWrongChip, kImageNoDescriptor };
constexpr uint16_t kChipEsp32S3 = 9;
constexpr size_t kImageHeadBytes = 112;                         // how much of the file checkAppImage() needs
ImageCheck checkAppImage(const uint8_t* head, size_t len, uint16_t chip);
const char* imageCheckMessage(ImageCheck c);

// Every Fancy2WLED image contains this text (it is the pattern the update scans the whole upload for), so an image of some other
// firmware for the same chip (WLED, a different project) is refused before it replaces this one.
extern const char kImageMark[];
struct MarkScanner { size_t matched = 0; bool found = false; };
void markScan(MarkScanner& s, const uint8_t* data, size_t n);  // feed the upload chunk by chunk, in order

bool validUpdatePassword(const char* pw);                       // 8 to 64 printable ASCII characters (letters, digits, symbols, spaces)
bool secureEquals(const char* a, const char* b);                // the time taken does not depend on where the first difference is

// ---- WLED address ---------------------------------------------------------
// Cleans up what was typed or pasted into the "WLED address" box so that "  HTTP://Wled-Ceiling.local/ " becomes "wled-ceiling.local":
// leading / trailing spaces, an "http://" or "https://" in front, everything from the first '/', '?' or '#', a ":80" port, trailing dots,
// upper case. Writes at most cap-1 characters and a terminator. Returns the length, or -1 when it does not fit (or the input is null).
int normalizeHost(const char* in, char* out, int cap);

// A dotted IPv4 address and nothing else: four numbers 0..255 separated by single dots, no spaces, no names, no leading zeros ("010" would be read as octal
// by some programs). Writes the four bytes to out (may be null).
bool parseIpv4(const char* s, uint8_t out[4]);
// An address the Sync Box could send from (the setup page's "only accept this box"): a valid IPv4 address that is a single device, so not 0.0.0.0 (which the
// relay reads as "accept anyone"), not loopback (127.x), not multicast / reserved (224 and up).
bool validSenderAddress(const char* s);

// Why the ESP32 last restarted: the ESP-IDF reset reason code (esp_reset_reason()) in words, and whether it points at a problem (a crash, a watchdog, a brownout)
// rather than a normal start or restart. The setup page and the diagnostics file show it.
const char* resetReasonName(int code);
bool resetReasonUnexpected(int code);

// ---- Sync Box frame -------------------------------------------------------
// Decodes one UDP payload. On success returns true and fills zones[24][3] (r,g,b).
// Frame: 55 AA 5A n 00 b5 b6 b7 | KEY (len-83 bytes) | DATA (75 bytes), plaintext = DATA xor KEY = 06 04 06 + 24 x RGB.
bool decodeFrame(const uint8_t* data, size_t len, uint8_t zones[kZones][3]);

// What a datagram on the Sync Box port is, for telling "the box is silent" from "the box changed what it sends":
//   kFrameOk         decodes: the colours are in zones
//   kFrameNewLayout  has the Sync Box framing (magic bytes, and the length byte matching the size) but the payload is not one we can read:
//                    a box firmware that changed the payload (a new marker, more zones, a new key scheme)
//   kFrameUnreadable anything else (other data on that port, a changed header, a damaged datagram)
// Header byte 5 is a checksum (167 + the sum of every byte except bytes 3 and 5, mod 256); decodeFrame does not check it so a
// changed checksum cannot break the relay, but it is reported: a valid checksum on a frame we cannot read means "same protocol, new payload".
enum FrameClass { kFrameOk = 0, kFrameNewLayout = 1, kFrameUnreadable = 2 };
struct FrameInfo {
  bool checksumOk = false;
  bool headKnown = false;    // head is valid (the size is one of the known 86-93 so the key length is known)
  uint8_t head[3] = {0, 0, 0};   // the first 3 payload bytes after undoing the key: 06 04 06 on every box seen so far
};
bool frameChecksumOk(const uint8_t* data, size_t len);
FrameClass classifyFrame(const uint8_t* data, size_t len, uint8_t zones[kZones][3], FrameInfo* info);

// ---- Govee lights (LAN API) -------------------------------------------------
// A Govee light with LAN control switched on takes JSON over UDP on port 4003. The "razer" streaming mode takes one colour per segment and
// stretches them evenly over the whole light (checked on an H61C3: 5, 10 and 20 entries all covered the full length, entry 0 at its right
// end). These build the complete UDP payloads; tools/reference/govee.py is the independent reference they are tested against.
constexpr int kGoveeMaxSegments = 60;
constexpr uint16_t kGoveeScanPort = 4001, kGoveeReplyPort = 4002, kGoveeControlPort = 4003;
size_t base64Encode(const uint8_t* in, size_t n, char* out, size_t cap);          // NUL-terminated; returns the length, 0 if it does not fit
size_t goveeEnableJson(bool on, char* out, size_t cap);                           // streaming on / off
size_t goveeFrameJson(const uint8_t* rgb, int n, char* out, size_t cap);          // n colours (3 bytes each), 1..kGoveeMaxSegments; 0 if invalid or it does not fit

// ---- TV off ---------------------------------------------------------------
// The Sync Box sends no separate packet when the TV is switched off (checked on a real box: CEC power off and on, every port that could be
// heard). It keeps streaming at the normal rate, with all 24 zones exactly 0,0,0, and the colours come back with the first non-black frame.
// So "TV off" is inferred: the picture has been completely black for a while. Normal viewing does not do that (a 4 minute capture of real
// content had only 0.1-0.2 s blips), but a long black screen would, which is why the delay is a setting.
struct BlackTracker {
  bool black = false;          // the last frame was completely black
  uint32_t since = 0;          // millis() when the current run of black frames began
};
bool isBlackPicture(const uint8_t zones[kZones][3]);
void blackFrame(BlackTracker& t, const uint8_t zones[kZones][3], uint32_t nowMs);   // call for every frame that is read
void blackReset(BlackTracker& t);                                                   // the box went silent: nothing is known any more
bool tvIsOff(const BlackTracker& t, uint32_t nowMs, uint32_t delayMs);              // black for at least delayMs (wrap-safe)
uint32_t blackForMs(const BlackTracker& t, uint32_t nowMs);                         // how long it has been black (0 = not black)

// ---- zone -> LED mapping --------------------------------------------------
// Each LED blends two zones: colour = zone[a] * (1 - f/255) + zone[b] * (f/255).
// d says how far the LED is beyond the TV along its edge (0 = lined up with the TV): 0..255 = 0..kOutMax picture widths.
// LEDs beyond the TV show the colour of the zone at the end of the strip (see applyTable), so the TV-length part is exact and the rest
// of the strip is smooth instead of repeating the edge colour.
struct Entry {
  uint8_t a, b, f, d;
};
constexpr float kOutMax = 2.0f;     // picture widths represented by d = 255
constexpr int kMinOutQ = 19;        // smoothing never ramps faster than 0.15 picture widths (a strip hugging the TV is untouched)
constexpr float kDeadZone = 0.02f;  // overshoot (picture widths) ignored before a LED counts as beyond the TV
constexpr float kDefaultAspect = 9.0f / 16.0f;

// Table entry for a LED at picture position (u, v): (0,0) top-left of the picture, (1,1) bottom-right,
// positions outside the picture are allowed and snap to the nearest picture edge. aspect = picture height / width.
// edgeHint: -1 = the nearest edge, otherwise force 0 top / 1 right / 2 bottom / 3 left (a straight run whose side is known).
Entry uvToEntry(float u, float v, float aspect = kDefaultAspect, int edgeHint = -1);

// A straight run of `count` LEDs along one side of the picture. edge: 0 top (ceiling: uses the top row of the colour map),
// 1 right, 2 bottom (uses the bottom row), 3 left. led0Right: LED 0 is at the right end of a top/bottom run (facing the TV), or
// at the bottom end of a left/right run. spanFirst/spanLast: LED indexes lined up with the TV's two ends along that side;
// pass spanFirst < 0 for "the whole run is as long as the TV".
void tableLinear(Entry* out, int count, bool led0Right, int spanFirst, int spanLast, int edge = 0);

// LEDs running around the four edges. startCorner: 0 top-left, 1 top-right, 2 bottom-right, 3 bottom-left.
// sides: {top,right,bottom,left} LED counts (must sum to count) or nullptr for automatic from the aspect ratio.
// Returns false if `sides` does not add up to `count`.
bool tableLoop(Entry* out, int count, int startCorner, bool counterClockwise, const int* sides, int shift,
               int aspectW, int aspectH);

// The two tails of a strip (the LEDs before / after the part lined up with the TV) show the colour of the zone at their end of
// the strip (a ceiling strip: the top-left zone on the left, the top-right zone on the right) and ease toward it, reaching it at
// the strip's own end when percent = 100. Only that zone counts, so a colour anywhere else on screen (another side of the
// picture, or even the middle of the same edge) never reaches the LEDs beyond the TV. Lengths are in d units; 0 = no smoothing.
struct SmoothInfo {
  int firstZero, lastZero;           // first / last LED with d == 0 (lined up with the TV); count / -1 when there is none
  int lenStart, lenEnd;              // ease length for the LEDs before firstZero / after lastZero
  uint32_t maskStart, maskEnd;       // the zone each tail eases toward (bit z = zone z); 0 = none (tail absent)
};
SmoothInfo smoothInfo(const Entry* table, int count, int percent);   // percent: 0 = off, 100 = reach the average at the strip end

// Temporal smoothing that does not depend on how often it runs. `smoothing` is the setting (0 = none .. 0.95 = very slow) and
// is the fraction of the remaining distance to the target that is left after one reference tick (refMs, e.g. 33 ms at 30 fps).
// Returns the fraction left after dtMs: cur = target + (cur - target) * smoothDecay(...). Applying it in many small steps gives
// the same result as one big step, so an extra frame in between ticks does not change how the lights feel.
float smoothDecay(float smoothing, float dtMs, float refMs);

// The Sync Box smooths its own output before broadcasting it: with its "Speed of Diffusion" set to s, every packet moves s of the way from the last
// one to the new colour (y = (1 - s) * previous y + s * target; at the default 40 % the packets of a change show 40, 64, 78, 87, 92 % ...). Measured with
// tools/sync_probe, which also showed that at 100 % the box does not smooth at all. That smoothing is most of the delay between the picture and the lights.
// Undoing it needs only the previous packet: after m packets of smoothing target = y + k * (y - previous y) with k = q^m / (1 - q^m), q = 1 - s
// (m = 1, s = 0.4: k = 0.6 / 0.4 = 1.5). `boost` blends between the box's own (smoothed) value (0) and the reconstructed target (1).
// speedPercent is the box's setting (10..100; 100 = nothing to undo); steps = how many smoothing steps lie between the two packets (see boxStepsForGap).
// The result is clamped to a valid colour. One case cannot be told from the stream: when the packet just before a change is lost, the first packet after
// it already holds two steps while looking like one, so that single frame overshoots (about 60 % of the change at boost 1; boost 0.6 halves it).
constexpr float kBoxLead = 1.5f;
float leadBoost(float y, float yPrev, float boost, int speedPercent = 40, int steps = 1);
// How many smoothing steps happened between two packets that arrived gapMs apart: the box makes one about every 73 ms (13.7 packets a second) and the
// link drops some, so a long gap holds several (measured: a gap of 200 ms held 3 steps). Packets that arrive together, or a normal 100 ms apart, are
// one step apart. 0 = a pause of more than 600 ms: the colours were not following each other, do not extrapolate across it.
int boxStepsForGap(uint32_t gapMs);

// Finding out the box's Speed of Diffusion from its packets alone (no knowledge of the picture). With the setting s the box moves every colour channel a fraction
// s of the way to the new picture with each packet, so after a sudden change the second step is (1 - s) times the first (40 %: 0.6 times). Whenever channels that
// were still start to move together (a scene cut, a flash), the least squares slope of the second step on the first over all of them (at least kSpeedMinChannels)
// estimates q = 1 - s. Real video keeps moving during the second step, which pushes such an estimate anywhere, while the clean changes all agree: so the answer
// is a vote, not a median. The latest kSpeedRing estimates vote for their bin of 0.05 (the box's speeds are multiples of 5 %); the winner is the answer once
// kSpeedMinEvents estimates exist, and is only called sure when it has at least kSpeedMinPeak votes, a quarter of all, and twice the runner-up's. (Measured on a real
// box at 75 % with normal video: 27 % voted for 75, 12 % for the runner-up; a median with a spread limit was wrong in 10 of 82 windows.) Packets are only used when no
// packet was lost between the four of them (boxStepsForGap == 1 for each) and values near black are ignored (the box's own black cut-off distorts them). This is
// advice only: the relay does not change its behaviour from it. Independent version: tools/reference/speed.py.
constexpr int kSpeedRing = 24, kSpeedMinEvents = 10, kSpeedMinChannels = 6, kSpeedMinPeak = 5;
// What the page shows is held steady: an answer is shown once the vote has been sure of it kSpeedShowRun estimates in a row, and replaced by a different one only after
// kSpeedChangeRun in a row (on a live picture the vote flickers between sure, unsure and a neighbouring value; the true speed rarely changes).
constexpr int kSpeedShowRun = 3, kSpeedChangeRun = 5;
struct SpeedDetect {
  uint8_t f[4][kZones][3] = {};     // the last four packets, the newest last
  uint8_t steps[4] = {};            // for each: boxStepsForGap since the packet before it (clamped to 255)
  int have = 0;                     // how many of f hold a packet (up to 4)
  float q[kSpeedRing] = {};         // the latest estimates of q = 1 - s
  int nq = 0, head = 0;             // how many are valid, and where the next one goes
  uint32_t events = 0;              // estimates made since the start
  int shown = -1;                   // the held answer (percent), -1 = none yet
  int cand = -1, run = 0;           // the answer the vote is sure of now, and for how many estimates in a row
};
struct SpeedResult {
  int estimates = 0;                // how many estimates the answer rests on (0..kSpeedRing)
  int percent = -1;                 // the box's Speed of Diffusion the vote chose, in 5 % steps (10..100); -1 = fewer than kSpeedMinEvents estimates yet
  bool sure = false;                // the winner clearly wins the vote
  int peak = 0, second = 0;         // votes of the winner and of the runner-up
};
void speedReset(SpeedDetect& d);
void speedPush(SpeedDetect& d, const uint8_t zones[kZones][3], int steps);   // steps = boxStepsForGap since the previous packet
SpeedResult speedResult(const SpeedDetect& d);

// The finishing stage of the output: what is done to one target's LEDs after every temporal stage (motion smoothing or easing) and just before the colours are rounded
// to bytes. It comes after the temporal stages on purpose: Reaction boost works earlier, on the Sync Box's own frames, so nothing here can fight it. All three parts are off
// by default (then the colours pass through untouched).
//  * black cut-off: an LED whose brightest channel is below blackCut (levels, 0 = off) goes fully dark, so dark noise stays black instead of shimmering. One level of hysteresis:
//    once dark, it needs blackCut + 1 to light again, so an LED sitting right at the edge does not flicker either.
//  * deadband (levels, 0 = off): a channel keeps the value it showed until the colour has moved more than this far from it (the anti-flicker hold). Channels under one level
//    are never held, so a fade to black reaches black. The price is up to `deadband` levels of standing error; 1 to 3 is plenty.
//  * dither: while the colours are moving, the rounding error of each channel is carried to the next frame (error feedback), so a slow fade or a dark scene shows its steps
//    between whole levels by flickering between the two neighbours instead of sticking to one. When the colours are still the exact rounded value is shown.
// State per LED (held, resid: 3 floats; dark: 1 byte) belongs to the caller; fresh = the state is not valid (first frame, or the settings changed). out gets whole numbers
// 0..255 as floats. Independent version: tools/reference/finish.py.
struct FinishParams { float deadband = 0.f; int blackCut = 0; bool dither = false; };
inline bool finishActive(const FinishParams& p) { return p.deadband > 0.f || p.blackCut > 0 || p.dither; }
void finishColours(const float* cur, int n, float* held, float* resid, uint8_t* dark, const FinishParams& p, bool moving, bool fresh, float* out);

// Vivid blending between two zone colours. Mixing two colours in RGB (red and green half and half is a dull dark yellow) turns unlike hues into
// grey or brown in between. This blends along the colour wheel instead: the hue takes the shorter way round from one colour to the other (red -> orange ->
// yellow -> green), saturation and brightness blend straight. A grey or black end has no hue of its own and takes the other end's. w = 0 gives a, 1 gives b.
void vividBlend(const float a[3], const float b[3], float w, float out[3]);
// Switches off every LED (n colours, 3 floats each) outside the TV part first..last (the strip's own numbers within the run). Nothing happens when
// no TV part is set (first < 0), so a strip without one never goes dark.
void darkOutside(float* rgb, int n, int first, int last);

// How soft the colour changes where one zone ends and the next begins. Each LED blends two zones (colour = zone[a] * (1 - f/255) + zone[b] * (f/255), f
// running from 0 at the first zone's centre to 255 at the next one's, the line between them at 127.5). percent steepens that around the line: 0 leaves
// it as it is (smooth), 100 snaps every LED to the zone it sits in (hard edges, like the Sync Box's own strip), in between the change is squeezed into a
// narrower band. Entries that use one zone (the stretch beyond the TV) are left alone. Not for phone-calibrated tables.
void sharpenTable(Entry* table, int count, int percent);

// Saturation of finished LED colours (r, g, b as 0..255 floats, 3 per LED): 1 leaves them, 0 makes them grey, above 1 pushes each away from the grey of
// the same brightness. Results are clamped to 0..255.
void saturateColours(float* rgb, int n, float sat);

// The runs of consecutive LEDs that follow `zone` more than any other zone (each LED's table entry blends zone a and zone b; it belongs to a when f < 128, else to b).
// out[k] = {first LED, number of LEDs}; returns the number of runs (at most maxRuns). The setup page lights these when a zone is clicked on the picture map.
int zoneRuns(const Entry* table, int count, int zone, int out[][2], int maxRuns);

// How many LEDs follow each picture zone: an LED adds (1 - f/255) to zone a and f/255 to zone b of its table entry, so the 24 numbers add
// up to `count`. The setup page draws this as the "picture map" (which part of the screen each target pulls from). LEDs past the ends of the
// TV hold the corner zone, so the corners usually come out highest.
void zoneUsage(const Entry* table, int count, float out[kZones]);

// out[i*3 + c] = colour of LED i.
void applyTable(const Entry* table, int count, const float zones[kZones][3], float* out, const SmoothInfo& smooth, bool vivid = false);   // vivid: blend zones with vividBlend()

// ---- spatial smoothing: blur along a strip ---------------------------------
// The "Blur (LEDs)" setting (0 = off) as the half width of each of three box averages run one after the other; together they come close to a Gaussian with
// sigma = blurLeds / 2 LEDs (blur 5: three averages 5 LEDs wide, sigma about 2.5). At least 1 when blurLeds > 0.
int blurHalfWidth(int blurLeds);
// Blurs LEDs first..last (the picture part of a strip; first < 0 = the whole strip) of n colours (3 floats each) in place. The two ends of that part are
// held (the first and last LED are repeated beyond it), so the LEDs outside the part are not touched and nothing leaks in from them.
// scratch: room for 2 * (last - first + 1) * 3 floats.
void blurColours(float* rgb, int n, int first, int last, int blurLeds, float* scratch);

// ---- motion smoothing: time interpolation between Sync Box pictures ----------
// The box sends about 14 frames a second but a new picture only about every 100 ms, and they reach the board unevenly (bursts, gaps). Instead of jumping to
// each picture, the output slides from one to the next, shown about one picture interval late. Pictures are the 24 zones (already adjusted for
// brightness / gamma / saturation). Each picture is placed on a slowly adjusting clock (so bursts and gaps do not make the motion lurch); pictures that
// arrive within 25 ms of the previous one replace it (a burst is one picture). Independent implementation: tools/reference/motion.py.
constexpr int kMotionKnots = 4;
struct MotionKnot { uint32_t ms = 0; float z[kZones][3] = {}; };
struct Motion {
  MotionKnot k[kMotionKnots];       // the latest pictures, the newest last
  int n = 0;                        // how many are in use
  uint32_t lastRawMs = 0;           // when the newest picture really arrived
  float periodMs = 100.f;           // the clock's estimate of the time between pictures
};
void motionReset(Motion& m);
void motionPush(Motion& m, uint32_t nowMs, const float zones[kZones][3]);
// The picture to show at time timeMs (holds the oldest before the first knot and the newest after the last one). True when the newest picture has been
// reached, or when the pictures still to be slid through are all identical (a still picture): nothing changes until a different one arrives, so the caller
// can stop producing output. No picture yet: all zones 0 and true.
bool motionAt(const Motion& m, uint32_t timeMs, float out[kZones][3]);
// How far behind the newest picture to show: percent (0..100) of one picture interval.
float motionDelayMs(const Motion& m, int percent);

// ---- delay tracking --------------------------------------------------------
// The last kRttSamples round trips to one target (a probe packet there and back, in milliseconds; a probe that got no answer counts as lost).
// The one-way network delay is taken as half of it. Plain arrays, no allocation.
constexpr int kRttSamples = 20;
struct RttRing {
  float ms[kRttSamples] = {};       // < 0 = lost
  int head = 0, count = 0;          // next slot to write; how many slots hold a probe (up to kRttSamples)
};
struct RttSummary {
  int samples = 0, lost = 0;        // probes in the ring; how many of them got no answer
  float avgMs = -1, minMs = -1, maxMs = -1;   // over the answered ones (-1 = none answered yet)
};
void rttAdd(RttRing& r, float ms);
RttSummary rttSummary(const RttRing& r);
// An estimate of how long a WLED needs to show a frame it has received: it clocks the LEDs out one after the other (about 30 microseconds each for
// WS281x strips, over the strip's whole configured length) and a received frame waits on average half a refresh for the next one. fps = the refresh rate
// WLED reports (0 = unknown: only the clocking-out time is counted). It is a model, not a measurement.
float wledShowMs(int totalLeds, float fps);

// ---- DDP (UDP 4048) ---------------------------------------------------------
constexpr size_t kDdpHeader = 10;
constexpr size_t kDdpMaxData = 1440;
// Writes the 10-byte header. dataType: 0x0B = RGB 8-bit, 0x1B = RGBW 8-bit. byteOffset = LED index * bytes per LED.
void ddpHeader(uint8_t* out, uint8_t seq, uint8_t dataType, uint32_t byteOffset, uint16_t length, bool push);

}  // namespace fwb
