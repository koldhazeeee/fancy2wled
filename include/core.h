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

// ---- Sync Box frame -------------------------------------------------------
// Decodes one UDP payload. On success returns true and fills zones[24][3] (r,g,b).
// Frame: 55 AA 5A n 00 b5 b6 b7 | KEY (len-83 bytes) | DATA (75 bytes), plaintext = DATA xor KEY = 06 04 06 + 24 x RGB.
bool decodeFrame(const uint8_t* data, size_t len, uint8_t zones[kZones][3]);

// ---- zone -> LED mapping --------------------------------------------------
// Each LED blends two zones: colour = zone[a] * (1 - f/255) + zone[b] * (f/255).
// d says how far the LED is beyond the TV along its edge (0 = lined up with the TV): 0..255 = 0..kOutMax picture widths.
// LEDs beyond the TV ease toward the average picture colour (see applyTable), so the TV-length part is exact and the rest
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

// The two tails of a strip (the LEDs before / after the part lined up with the TV) each ease to the average colour,
// reaching it at the strip's own end when percent = 100. Lengths are in d units; 0 = no smoothing.
struct SmoothInfo {
  int firstZero, lastZero;   // first / last LED with d == 0 (lined up with the TV); count / -1 when there is none
  int lenStart, lenEnd;      // ease length for the LEDs before firstZero / after lastZero
};
SmoothInfo smoothInfo(const Entry* table, int count, int percent);   // percent: 0 = off, 100 = reach the average at the strip end

// out[i*3 + c] = colour of LED i.
void applyTable(const Entry* table, int count, const float zones[kZones][3], float* out, const SmoothInfo& smooth);

// ---- DDP (UDP 4048) ---------------------------------------------------------
constexpr size_t kDdpHeader = 10;
constexpr size_t kDdpMaxData = 1440;
// Writes the 10-byte header. dataType: 0x0B = RGB 8-bit, 0x1B = RGBW 8-bit. byteOffset = LED index * bytes per LED.
void ddpHeader(uint8_t* out, uint8_t seq, uint8_t dataType, uint32_t byteOffset, uint16_t length, bool push);

}  // namespace fwb
