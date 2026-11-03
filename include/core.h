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

// The Sync Box smooths its own output before broadcasting it: each frame it moves 40 % of the way to the new colour
// (y = 0.6 * previous y + 0.4 * target; measured on a real box, see docs/PROTOCOL.md). That is most of the delay between the picture and
// the lights. Undoing it needs only the previous frame: target = y + k * (y - previous y) with k = 0.6 / 0.4 = 1.5. `boost` blends
// between the box's own (smoothed) value (0) and the reconstructed target (1). The result is clamped to a valid colour.
constexpr float kBoxLead = 1.5f;
float leadBoost(float y, float yPrev, float boost);

// How many LEDs follow each picture zone: an LED adds (1 - f/255) to zone a and f/255 to zone b of its table entry, so the 24 numbers add
// up to `count`. The setup page draws this as the "picture map" (which part of the screen each target pulls from). LEDs past the ends of the
// TV hold the corner zone, so the corners usually come out highest.
void zoneUsage(const Entry* table, int count, float out[kZones]);

// out[i*3 + c] = colour of LED i.
void applyTable(const Entry* table, int count, const float zones[kZones][3], float* out, const SmoothInfo& smooth);

// ---- DDP (UDP 4048) ---------------------------------------------------------
constexpr size_t kDdpHeader = 10;
constexpr size_t kDdpMaxData = 1440;
// Writes the 10-byte header. dataType: 0x0B = RGB 8-bit, 0x1B = RGBW 8-bit. byteOffset = LED index * bytes per LED.
void ddpHeader(uint8_t* out, uint8_t seq, uint8_t dataType, uint32_t byteOffset, uint16_t length, bool push);

}  // namespace fwb
