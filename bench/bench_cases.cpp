// Benchmark cases for the hot paths of the relay: what runs for every Sync Box packet (about 14 a second) and for every output tick of every target (up to 30 a
// second). The same file is built for the PC (bench/bench_host.cpp) and for the board (the atoms3-bench environment, src/bench_main.cpp), so before/after numbers
// can be compared on both. See docs/PERFORMANCE.md for how to run it and what it measured.
//
// Every case does `iters` repetitions of one operation on realistic, deterministic input and feeds a result into gBenchSink so nothing can be optimised away.
#include "bench_cases.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

using namespace fwb;

volatile uint32_t gBenchSink = 0;

namespace {
// Two real frames from a Sync Box (colours only): 88 and 91 bytes.
const char* kFrame88 =
    "55aa5a5300c934219ae71786219ce311358e2953a732923554a035962848a52b883052a637942b49a2378f364fbb32912e55b92d923548a336932f57a73390344ab52590374da72a882b4abd368d3356ba2c90374da62b8b";
const char* kFrame91 =
    "55aa5a56001b39004d2e92721063fb004b2a94c1bfd04fb0f99d3dc1a7d04cb2e29c3fdbbad64ab1f89f3cc7a1cd57a8e19a22c6a2cd50b3e28126c2a2d64bb0f89f3cdfb3c04aade79e3edba1ce51b0e18723dfbad256aafc8338";
uint8_t gFrame88[88], gFrame91[91];

constexpr int kLeds = 266;              // a typical long strip (the author's longest)
constexpr int kPics = 8;                // pictures rotated through, so no case sees the same input twice in a row
float gZonesF[kPics][kZones][3];
uint8_t gZonesB[kPics][kZones][3];
Entry gTable[kLeds];
Entry gLoop[kLeds];
SmoothInfo gSmooth, gSmoothLoop;
float gTgt[kLeds * 3], gCur[kLeds * 3], gOut[kLeds * 3], gHeld[kLeds * 3], gResid[kLeds * 3], gScratch[kLeds * 6];
uint8_t gDark[kLeds];
uint8_t gPayload[kLeds * 4];
uint8_t gRgb50[50 * 3];
char gJson[1024];
Motion gMotion;
SpeedDetect gSpeed;
BlackTracker gBlack;
uint32_t gClock = 1000;

uint32_t rng = 12345u;
uint32_t rnd() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }

void fromHexInto(const char* h, uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned v = 0;
    for (int k = 0; k < 2; k++) { char c = h[i * 2 + k]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10); }
    out[i] = (uint8_t)v;
  }
}

// ---- box path (once per Sync Box packet)
void classify88(int n) { uint8_t z[kZones][3]; FrameInfo fi; for (int i = 0; i < n; i++) { classifyFrame(gFrame88, 88, z, &fi); gBenchSink += z[i % kZones][0]; } }
void classify91(int n) { uint8_t z[kZones][3]; FrameInfo fi; for (int i = 0; i < n; i++) { classifyFrame(gFrame91, 91, z, &fi); gBenchSink += z[i % kZones][1]; } }
void blackframe(int n) { for (int i = 0; i < n; i++) { blackFrame(gBlack, gZonesB[i % kPics], gClock += 70); gBenchSink += gBlack.black; } }
void leadboost72(int n) {
  float acc = 0;
  for (int i = 0; i < n; i++) {
    const uint8_t* a = &gZonesB[i % kPics][0][0];
    const uint8_t* b = &gZonesB[(i + 3) % kPics][0][0];
    for (int k = 0; k < kZones * 3; k++) acc += leadBoost((float)a[k], (float)b[k], 0.6f, 50, 1);
  }
  gBenchSink += (uint32_t)acc;
}
void adjustDefault(int n) { float o[kZones][3]; for (int i = 0; i < n; i++) { adjustZones(o, gZonesF[i % kPics], 0.5f, 1.f, 1.f); gBenchSink += (uint32_t)o[i % kZones][0]; } }
void adjustGamma(int n) { float o[kZones][3]; for (int i = 0; i < n; i++) { adjustZones(o, gZonesF[i % kPics], 1.f, 1.2f, 1.3f); gBenchSink += (uint32_t)o[i % kZones][0]; } }
void motionPushAt(int n) {
  float o[kZones][3];
  for (int i = 0; i < n; i++) {
    gClock += 100;
    motionPush(gMotion, gClock, gZonesF[i % kPics]);
    motionAt(gMotion, gClock - 60, o);
    gBenchSink += (uint32_t)o[i % kZones][2];
  }
}
void speedPush1(int n) { for (int i = 0; i < n; i++) { speedPush(gSpeed, gZonesB[i % kPics], 1); gBenchSink += gSpeed.events; } }

// ---- one target, one output tick (266 LEDs)
void applyLinear(int n) { for (int i = 0; i < n; i++) { applyTable(gTable, kLeds, gZonesF[i % kPics], gTgt, gSmooth, false); gBenchSink += (uint32_t)gTgt[i % (kLeds * 3)]; } }
void applyLinearVivid(int n) { for (int i = 0; i < n; i++) { applyTable(gTable, kLeds, gZonesF[i % kPics], gTgt, gSmooth, true); gBenchSink += (uint32_t)gTgt[i % (kLeds * 3)]; } }
void applyLoop(int n) { for (int i = 0; i < n; i++) { applyTable(gLoop, kLeds, gZonesF[i % kPics], gTgt, gSmoothLoop, false); gBenchSink += (uint32_t)gTgt[i % (kLeds * 3)]; } }
void saturate(int n) { for (int i = 0; i < n; i++) { memcpy(gOut, gTgt, sizeof gTgt); saturateColours(gOut, kLeds, 1.3f); gBenchSink += (uint32_t)gOut[i % (kLeds * 3)]; } }
void blur8(int n) { for (int i = 0; i < n; i++) { memcpy(gOut, gTgt, sizeof gTgt); blurColours(gOut, kLeds, 40, 225, 8, gScratch); gBenchSink += (uint32_t)gOut[i % (kLeds * 3)]; } }
void darkout(int n) { for (int i = 0; i < n; i++) { memcpy(gOut, gTgt, sizeof gTgt); darkOutside(gOut, kLeds, 40, 225); gBenchSink += (uint32_t)gOut[i % (kLeds * 3)]; } }
void ease(int n) {
  memcpy(gCur, gTgt, sizeof gCur);
  float w = 0;
  for (int i = 0; i < n; i++) { gTgt[i % (kLeds * 3)] += (i & 1) ? 3.f : -3.f; w += easeColours(gCur, gTgt, kLeds, 0.9f, 0.5f); }
  gBenchSink += (uint32_t)w;
}
void pack(int n) { for (int i = 0; i < n; i++) { gCur[i % (kLeds * 3)] += 0.3f; gBenchSink += (uint32_t)packColours(gCur, kLeds, false, gPayload) + gPayload[i % 100]; } }
void packW(int n) { for (int i = 0; i < n; i++) { gCur[i % (kLeds * 3)] += 0.3f; gBenchSink += (uint32_t)packColours(gCur, kLeds, true, gPayload) + gPayload[i % 100]; } }
void finishAll(int n) {
  FinishParams fp; fp.deadband = 2.f; fp.blackCut = 4; fp.dither = true;
  for (int i = 0; i < n; i++) { finishColours(gTgt, kLeds, gHeld, gResid, gDark, fp, true, i == 0, gOut); gBenchSink += (uint32_t)gOut[i % (kLeds * 3)]; }
}

// A whole output tick as the relay does it, for three typical setups.
void tickMinimal(int n) {      // a plain strip: table -> ease -> pack
  memcpy(gCur, gTgt, sizeof gCur);
  for (int i = 0; i < n; i++) {
    applyTable(gTable, kLeds, gZonesF[i % kPics], gTgt, gSmooth, false);
    easeColours(gCur, gTgt, kLeds, 0.9f, 0.5f);
    gBenchSink += (uint32_t)packColours(gCur, kLeds, false, gPayload) + gPayload[i % 100];
  }
}
void tickRich(int n) {         // vivid blending, saturation, blur, dark outside the TV, easing
  memcpy(gCur, gTgt, sizeof gCur);
  for (int i = 0; i < n; i++) {
    applyTable(gTable, kLeds, gZonesF[i % kPics], gTgt, gSmooth, true);
    saturateColours(gTgt, kLeds, 1.3f);
    blurColours(gTgt, kLeds, 40, 225, 8, gScratch);
    darkOutside(gTgt, kLeds, 40, 225);
    easeColours(gCur, gTgt, kLeds, 0.9f, 0.5f);
    gBenchSink += (uint32_t)packColours(gCur, kLeds, false, gPayload) + gPayload[i % 100];
  }
}
void tickEverything(int n) {   // the above plus the finishing stage (black cut-off, hold steady, dither)
  FinishParams fp; fp.deadband = 2.f; fp.blackCut = 4; fp.dither = true;
  memcpy(gCur, gTgt, sizeof gCur);
  for (int i = 0; i < n; i++) {
    applyTable(gTable, kLeds, gZonesF[i % kPics], gTgt, gSmooth, true);
    saturateColours(gTgt, kLeds, 1.3f);
    blurColours(gTgt, kLeds, 40, 225, 8, gScratch);
    darkOutside(gTgt, kLeds, 40, 225);
    easeColours(gCur, gTgt, kLeds, 0.9f, 0.5f);
    finishColours(gCur, kLeds, gHeld, gResid, gDark, fp, true, i == 0, gOut);
    gBenchSink += (uint32_t)packColours(gOut, kLeds, false, gPayload) + gPayload[i % 100];
  }
}

// ---- other
void govee50(int n) { for (int i = 0; i < n; i++) { gRgb50[i % 150] += 7; gBenchSink += (uint32_t)goveeFrameJson(gRgb50, 50, gJson, sizeof gJson); } }
void ddphdr(int n) { uint8_t h[10]; for (int i = 0; i < n; i++) { ddpHeader(h, (uint8_t)i, 0x0B, (uint32_t)(i & 0xFFF), 1440, (i & 7) == 0); gBenchSink += h[i % 10]; } }

const BenchCase kCases[] = {
  {"classify_frame_88", "decode one 88 byte Sync Box packet", classify88},
  {"classify_frame_91", "decode one 91 byte Sync Box packet", classify91},
  {"black_frame", "TV-off tracking for one packet", blackframe},
  {"lead_boost_72ch", "Reaction boost over one packet (72 channels)", leadboost72},
  {"adjust_zones_default", "brightness only, per packet", adjustDefault},
  {"adjust_zones_gamma", "gamma 1.2 and saturation 1.3, per packet", adjustGamma},
  {"motion_push_at", "Motion smoothing: push a picture and read one interpolated", motionPushAt},
  {"speed_push", "box speed detector, per packet", speedPush1},
  {"apply_table_linear", "266 LEDs from the 24 zones (linear layout)", applyLinear},
  {"apply_table_loop", "266 LEDs from the 24 zones (loop layout)", applyLoop},
  {"apply_table_vivid", "266 LEDs, vivid blending", applyLinearVivid},
  {"saturate_266", "per-target saturation, 266 LEDs", saturate},
  {"blur_266", "blur 8 over the TV part, 266 LEDs", blur8},
  {"dark_outside_266", "dark outside the TV part, 266 LEDs", darkout},
  {"ease_266", "one easing step, 266 LEDs", ease},
  {"finish_all_266", "black cut-off + hold steady + dither, 266 LEDs", finishAll},
  {"pack_rgb_266", "round to bytes, RGB, 266 LEDs", pack},
  {"pack_rgbw_266", "round to bytes, RGBW, 266 LEDs", packW},
  {"tick_minimal_266", "a whole tick: plain strip (table, ease, pack)", tickMinimal},
  {"tick_rich_266", "a whole tick: vivid, saturation, blur, dark outside, ease, pack", tickRich},
  {"tick_everything_266", "a whole tick: the above plus the finishing stage", tickEverything},
  {"govee_json_50", "one Govee frame (50 segments) as JSON + base64", govee50},
  {"ddp_header", "one DDP header", ddphdr},
};
}  // namespace

void benchInit() {
  fromHexInto(kFrame88, gFrame88, 88);
  fromHexInto(kFrame91, gFrame91, 91);
  for (int p = 0; p < kPics; p++) {
    const int base = 30 + (int)(rnd() % 150);
    for (int z = 0; z < kZones; z++)
      for (int c = 0; c < 3; c++) {
        int v = base + (int)(rnd() % 90) - 40 + c * 8;
        v = v < 0 ? 0 : (v > 255 ? 255 : v);
        gZonesB[p][z][c] = (uint8_t)v;
        gZonesF[p][z][c] = (float)v + (float)(rnd() % 100) / 100.f;
      }
  }
  tableLinear(gTable, kLeds, false, 40, 225, 0);
  gSmooth = smoothInfo(gTable, kLeds, 100);
  tableLoop(gLoop, kLeds, 0, false, nullptr, 0, 16, 9);
  gSmoothLoop = smoothInfo(gLoop, kLeds, 100);
  applyTable(gTable, kLeds, gZonesF[0], gTgt, gSmooth, false);
  memcpy(gCur, gTgt, sizeof gCur);
  for (size_t i = 0; i < sizeof gRgb50; i++) gRgb50[i] = (uint8_t)rnd();
  speedReset(gSpeed);
  motionReset(gMotion);
}

int benchCaseCount() { return (int)(sizeof kCases / sizeof kCases[0]); }
const BenchCase& benchCase(int i) { return kCases[i]; }
