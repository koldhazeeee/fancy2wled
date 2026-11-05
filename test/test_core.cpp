// Host-side test of the firmware core against golden vectors from the Python reference.
//   build + run:  python test/run_core_tests.py
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core.h"

using namespace fwb;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...)                      \
  do {                                        \
    if (cond) { g_pass++; }                   \
    else { g_fail++; printf("FAIL: " __VA_ARGS__); printf("\n"); } \
  } while (0)

static std::vector<uint8_t> fromHex(const std::string& h) {
  std::vector<uint8_t> v;
  for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)strtol(h.substr(i, 2).c_str(), nullptr, 16));
  return v;
}

static std::vector<Entry> parseEntries(const char* s) {
  std::vector<Entry> v;
  while (*s) {
    int a, b, f, d, n = 0;
    if (sscanf(s, "%d,%d,%d,%d%n", &a, &b, &f, &d, &n) != 4) break;
    v.push_back(Entry{(uint8_t)a, (uint8_t)b, (uint8_t)f, (uint8_t)d});
    s += n;
    while (*s == ' ') s++;
  }
  return v;
}

// Entries are compared as zone-weight vectors so that (2,3,255) == (3,3,0); the distance byte may differ by 2.
static float weightDiff(const Entry& x, const Entry& y) {
  float wx[kZones] = {0}, wy[kZones] = {0};
  wx[x.a] += 1.f - x.f / 255.f; wx[x.b] += x.f / 255.f;
  wy[y.a] += 1.f - y.f / 255.f; wy[y.b] += y.f / 255.f;
  float m = 0;
  for (int i = 0; i < kZones; i++) m = fmaxf(m, fabsf(wx[i] - wy[i]));
  return m;
}
static bool sameEntry(const Entry& x, const Entry& y) { return weightDiff(x, y) <= 0.012f && abs((int)x.d - (int)y.d) <= 2; }

static void compareTables(const char* what, const std::vector<Entry>& got, const std::vector<Entry>& want) {
  if (got.size() != want.size()) { g_fail++; printf("FAIL: %s size %zu vs %zu\n", what, got.size(), want.size()); return; }
  int bad = -1;
  for (size_t i = 0; i < got.size(); i++)
    if (!sameEntry(got[i], want[i])) { bad = (int)i; break; }
  CHECK(bad < 0, "%s: LED %d differs (got %d,%d,%d,%d want %d,%d,%d,%d)", what, bad,
        bad >= 0 ? got[bad].a : 0, bad >= 0 ? got[bad].b : 0, bad >= 0 ? got[bad].f : 0, bad >= 0 ? got[bad].d : 0,
        bad >= 0 ? want[bad].a : 0, bad >= 0 ? want[bad].b : 0, bad >= 0 ? want[bad].f : 0, bad >= 0 ? want[bad].d : 0);
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "test/golden.txt";
  FILE* f = fopen(path, "r");
  if (!f) { printf("cannot open %s\n", path); return 2; }
  static char line[200000];
  int nDecode = 0, nUv = 0, nLin = 0, nLoop = 0, nDdp = 0, nApply = 0, nClassify = 0, nUsage = 0, nGovee = 0, nSharp = 0, nVivid = 0, nMotion = 0, nBlur = 0;
  while (fgets(line, sizeof line, f)) {
    if (line[0] == '#' || line[0] == '\n') continue;
    char kind[16];
    int off = 0;
    if (sscanf(line, "%15s%n", kind, &off) != 1) continue;
    const char* rest = line + off;
    while (*rest == ' ') rest++;
    std::string k = kind;
    if (k == "DECODE") {
      static char hex[4000], zones[4000];
      zones[0] = 0;
      sscanf(rest, "%3999s %3999s", hex, zones);
      auto fr = fromHex(hex);
      uint8_t z[kZones][3];
      bool ok = decodeFrame(fr.data(), fr.size(), z);
      if (zones[0] == '-') {
        CHECK(!ok, "decode: bad frame accepted");
      } else {
        CHECK(ok, "decode: good frame rejected");
        if (ok) {
          auto want = fromHex(zones);
          CHECK(memcmp(want.data(), z, 72) == 0, "decode: zone mismatch");
        }
      }
      nDecode++;
    } else if (k == "CLASSIFY") {
      // CLASSIFY hex class checksumOk head|-
      static char hex[4000], head[16];
      int cls = -1, cs = -1;
      head[0] = 0;
      sscanf(rest, "%3999s %d %d %15s", hex, &cls, &cs, head);
      auto fr = fromHex(hex);
      if (strcmp(hex, "00") == 0 && fr.size() == 1 && fr[0] == 0 && cls == 2) fr.clear();     // the generator writes an empty datagram as "00"
      uint8_t z[kZones][3];
      FrameInfo info;
      const FrameClass got = classifyFrame(fr.data(), fr.size(), z, &info);
      CHECK((int)got == cls, "classify: %zu-byte datagram got class %d want %d", fr.size(), (int)got, cls);
      CHECK(info.checksumOk == (cs != 0), "classify: checksum flag %d want %d (class %d, %zu bytes)", (int)info.checksumOk, cs, cls, fr.size());
      if (cls == 1) {
        if (head[0] == '-') CHECK(!info.headKnown, "classify: head should be unknown for a %zu-byte datagram", fr.size());
        else {
          auto want = fromHex(head);
          CHECK(info.headKnown && memcmp(want.data(), info.head, 3) == 0, "classify: payload head differs (%02x%02x%02x want %s)", info.head[0], info.head[1], info.head[2], head);
        }
      }
      nClassify++;
    } else if (k == "USAGE") {
      // USAGE LINEAR count right first last edge z0..z23   |   USAGE LOOP count corner ccw z0..z23
      char type[8];
      int n0 = 0, count = 0;
      sscanf(rest, "%7s%n", type, &n0);
      const char* p = rest + n0;
      std::vector<Entry> tbl;
      int n = 0;
      if (strcmp(type, "LINEAR") == 0) {
        int right, sf, sl, edge;
        sscanf(p, "%d %d %d %d %d%n", &count, &right, &sf, &sl, &edge, &n);
        tbl.resize(count);
        tableLinear(tbl.data(), count, right != 0, sf, sl, edge);
      } else {
        int corner, ccw;
        sscanf(p, "%d %d %d%n", &count, &corner, &ccw, &n);
        tbl.resize(count);
        tableLoop(tbl.data(), count, corner, ccw != 0, nullptr, 0, 16, 9);
      }
      p += n;
      float got[kZones], sum = 0.f, worst = 0.f;
      zoneUsage(tbl.data(), count, got);
      for (int z = 0; z < kZones; z++) {
        char* e;
        const float want = strtof(p, &e);
        p = e;
        sum += got[z];
        worst = fmaxf(worst, fabsf(got[z] - want));
      }
      CHECK(fabsf(sum - count) < 0.01f * count + 0.01f, "usage %s n=%d: the zones add up to %.2f, not %d", type, count, sum, count);
      CHECK(worst < 0.5f + 0.01f * count, "usage %s n=%d: worst zone differs from the reference by %.3f", type, count, worst);
      nUsage++;
    } else if (k == "UV") {
      float u, v; int a, b, ff, dd;
      if (sscanf(rest, "%f %f %d,%d,%d,%d", &u, &v, &a, &b, &ff, &dd) == 6) {
        Entry e = uvToEntry(u, v);
        CHECK(sameEntry(e, Entry{(uint8_t)a, (uint8_t)b, (uint8_t)ff, (uint8_t)dd}),
              "uv(%.3f,%.3f): got %d,%d,%d,%d want %d,%d,%d,%d", u, v, e.a, e.b, e.f, e.d, a, b, ff, dd);
        nUv++;
      }
    } else if (k == "LINEAR") {
      int count, right, sf, sl, edge, n = 0;
      sscanf(rest, "%d %d %d %d %d%n", &count, &right, &sf, &sl, &edge, &n);
      auto want = parseEntries(rest + n);
      std::vector<Entry> got(count);
      tableLinear(got.data(), count, right != 0, sf, sl, edge);
      char what[80]; snprintf(what, sizeof what, "linear n=%d right=%d span=%d,%d edge=%d", count, right, sf, sl, edge);
      compareTables(what, got, want);
      nLin++;
    } else if (k == "LOOP") {
      int count, corner, ccw, shift, aw, ah, n = 0;
      char sides[64];
      sscanf(rest, "%d %d %d %63s %d %d %d%n", &count, &corner, &ccw, sides, &shift, &aw, &ah, &n);
      auto want = parseEntries(rest + n);
      std::vector<Entry> got(count);
      int s[4];
      bool autoSides = strcmp(sides, "auto") == 0;
      if (!autoSides) sscanf(sides, "%d,%d,%d,%d", &s[0], &s[1], &s[2], &s[3]);
      bool ok = tableLoop(got.data(), count, corner, ccw != 0, autoSides ? nullptr : s, shift, aw, ah);
      char what[96]; snprintf(what, sizeof what, "loop n=%d corner=%d ccw=%d sides=%s shift=%d", count, corner, ccw, sides, shift);
      CHECK(ok, "%s: returned false", what);
      if (ok) compareTables(what, got, want);
      nLoop++;
    } else if (k == "SHARP") {
      // SHARP <percent> <entries before> | <entries after>
      int pct, n = 0;
      sscanf(rest, "%d%n", &pct, &n);
      const char* bar = strchr(rest + n, '|');
      auto before = parseEntries(rest + n);
      auto want = parseEntries(bar ? bar + 1 : "");
      CHECK(bar && before.size() == want.size() && before.size() > 0, "sharp vector is malformed (%zu / %zu entries)", before.size(), want.size());
      if (bar && before.size() == want.size()) {
        std::vector<Entry> got = before;
        sharpenTable(got.data(), (int)got.size(), pct);
        char what[64]; snprintf(what, sizeof what, "sharp %d%% of %zu LEDs", pct, got.size());
        compareTables(what, got, want);
      }
      nSharp++;    } else if (k == "APPLY" || k == "APPLYV") {
      // APPLY count side first last percent lenStart lenEnd z0r,z0g,... | idx:r,g,b ...
      int count, sf, sl, pct, qs, qe, n = 0;
      char side[16];
      static char zl[20000];
      sscanf(rest, "%d %15s %d %d %d %d %d %19999s%n", &count, side, &sf, &sl, &pct, &qs, &qe, zl, &n);
      const char* tail = strchr(rest + n, '|');
      std::vector<Entry> tbl(count);
      tableLinear(tbl.data(), count, strcmp(side, "right") == 0, sf, sl);
      const SmoothInfo si = smoothInfo(tbl.data(), count, pct);
      CHECK(si.lenStart == qs && si.lenEnd == qe, "smoothInfo(%d%%) = %d,%d want %d,%d", pct, si.lenStart, si.lenEnd, qs, qe);
      float zones[kZones][3];
      const char* p = zl;
      for (int z = 0; z < kZones; z++)
        for (int c = 0; c < 3; c++) { char* e; zones[z][c] = strtof(p, &e); p = *e == ',' ? e + 1 : e; }
      std::vector<float> out((size_t)count * 3);
      applyTable(tbl.data(), count, zones, out.data(), si, k == "APPLYV");
      p = tail ? tail + 1 : "";
      int checked = 0;
      while (*p) {
        int idx, nn = 0; float r, g, b;
        if (sscanf(p, " %d:%f,%f,%f%n", &idx, &r, &g, &b, &nn) != 4) break;
        p += nn;
        CHECK(fabsf(out[idx * 3] - r) < 0.7f && fabsf(out[idx * 3 + 1] - g) < 0.7f && fabsf(out[idx * 3 + 2] - b) < 0.7f,
              "apply pct=%d LED %d: got %.2f,%.2f,%.2f want %.2f,%.2f,%.2f", pct, idx, out[idx * 3], out[idx * 3 + 1], out[idx * 3 + 2], r, g, b);
        checked++;
      }
      CHECK(checked > 5, "apply: only %d samples parsed", checked);
      nApply++;
    } else if (k == "VBLEND") {
      // VBLEND r,g,b r,g,b w r g b
      float a[3], b[3], w, want[3];
      char sa[64], sb[64];
      if (sscanf(rest, "%63s %63s %f %f %f %f", sa, sb, &w, &want[0], &want[1], &want[2]) == 6) {
        sscanf(sa, "%f,%f,%f", &a[0], &a[1], &a[2]);
        sscanf(sb, "%f,%f,%f", &b[0], &b[1], &b[2]);
        float got[3];
        vividBlend(a, b, w, got);
        CHECK(fabsf(got[0] - want[0]) < 0.7f && fabsf(got[1] - want[1]) < 0.7f && fabsf(got[2] - want[2]) < 0.7f,
              "vivid blend %s -> %s at %.2f: got %.2f,%.2f,%.2f want %.2f,%.2f,%.2f", sa, sb, w, got[0], got[1], got[2], want[0], want[1], want[2]);
        nVivid++;
      }
    } else if (k == "GOVEE") {
      // GOVEE ENABLE <0|1> <json>   |   GOVEE FRAME <n> <hex rgb> <json>
      char what[16] = {0};
      int cursor = 0;
      sscanf(rest, "%15s%n", what, &cursor);
      char got[800];
      if (!strcmp(what, "ENABLE")) {
        int on = 0; char json[300] = {0};
        sscanf(rest + cursor, "%d %299s", &on, json);
        const size_t n = goveeEnableJson(on != 0, got, sizeof got);
        CHECK(n == strlen(json) && !strcmp(got, json), "govee enable %d: got %s want %s", on, got, json);
      } else {
        int cnt = 0; char hex[400] = {0}; char json[800] = {0};
        sscanf(rest + cursor, "%d %399s %799s", &cnt, hex, json);
        const auto rgb = fromHex(hex);
        const size_t n = goveeFrameJson(rgb.data(), cnt, got, sizeof got);
        CHECK(n == strlen(json) && !strcmp(got, json), "govee frame of %d: got %s want %s", cnt, got, json);
      }
      nGovee++;
    } else if (k == "DDP") {
      int seq, dt, ln, push; unsigned off2; char hex[64];
      sscanf(rest, "%d %d %u %d %d %63s", &seq, &dt, &off2, &ln, &push, hex);
      uint8_t h[10];
      ddpHeader(h, (uint8_t)seq, (uint8_t)dt, off2, (uint16_t)ln, push != 0);
      auto want = fromHex(hex);
      CHECK(want.size() == 10 && memcmp(h, want.data(), 10) == 0, "ddp header mismatch (seq %d)", seq);
      nDdp++;
    } else if (k == "MOTION") {
      // MOTION <pushes> <queries> <percent> <period> <delay> then <ms> <hex zones> per picture and <ms> <reached> <72 floats> per query
      const char* p = rest;
      int nP = 0, nQ = 0, pct = 0, adv = 0;
      float expPeriod = 0, expDelay = 0;
      if (sscanf(p, "%d %d %d %f %f%n", &nP, &nQ, &pct, &expPeriod, &expDelay, &adv) != 5) { CHECK(false, "bad MOTION line"); continue; }
      p += adv;
      Motion m;
      motionReset(m);
      for (int i = 0; i < nP; i++) {
        unsigned ms = 0; char hex[200] = {0};
        sscanf(p, " %u %199s%n", &ms, hex, &adv);
        p += adv;
        const auto zb = fromHex(hex);
        float z[kZones][3];
        for (int q = 0; q < kZones; q++) for (int c = 0; c < 3; c++) z[q][c] = (float)zb[q * 3 + c];
        motionPush(m, ms, z);
      }
      CHECK(fabsf(m.periodMs - expPeriod) < 0.01f && fabsf(motionDelayMs(m, pct) - expDelay) < 0.01f, "motion clock: period %.4f (want %.4f), delay %.4f (want %.4f)", m.periodMs, expPeriod, motionDelayMs(m, pct), expDelay);
      int bad = 0;
      float worst = 0.f;
      for (int i = 0; i < nQ; i++) {
        unsigned ms = 0; int reached = 0;
        sscanf(p, " %u %d%n", &ms, &reached, &adv);
        p += adv;
        float want[kZones * 3];
        for (int j = 0; j < kZones * 3; j++) { sscanf(p, " %f%n", &want[j], &adv); p += adv; }
        float got[kZones][3];
        const bool r = motionAt(m, ms, got);
        if (r != (reached != 0)) bad++;
        for (int j = 0; j < kZones * 3; j++) { const float d = fabsf(got[j / 3][j % 3] - want[j]); if (d > worst) worst = d; }
      }
      CHECK(bad == 0 && worst < 0.02f, "motion: %d of %d queries disagree about reaching the newest picture, worst colour difference %.4f", bad, nQ, worst);
      nMotion++;
    } else if (k == "BLUR") {
      int n = 0, first = 0, last = 0, blur = 0, adv = 0;
      char hex[1000] = {0};
      sscanf(rest, "%d %d %d %d %999s%n", &n, &first, &last, &blur, hex, &adv);
      const char* p = rest + adv;
      const auto cb = fromHex(hex);
      std::vector<float> rgb((size_t)n * 3), scratch((size_t)n * 6 + 6);
      for (int i = 0; i < n * 3; i++) rgb[i] = (float)cb[i];
      blurColours(rgb.data(), n, first, last, blur, scratch.data());
      float worst = 0.f;
      for (int i = 0; i < n * 3; i++) { float want = 0; sscanf(p, " %f%n", &want, &adv); p += adv; const float d = fabsf(rgb[i] - want); if (d > worst) worst = d; }
      CHECK(worst < 0.02f, "blur (n %d, part %d..%d, blur %d): worst colour difference %.4f", n, first, last, blur, worst);
      nBlur++;
    }
  }
  fclose(f);

  // Behavioural checks that don't depend on the reference.
  { Entry t[150]; tableLinear(t, 150, false, -1, -1);
    CHECK(t[0].a == 0 && t[149].b == 5, "linear ends: first=zone0 last=zone5");
    CHECK(t[0].d == 0 && t[75].d == 0 && t[149].d == 0, "a run exactly as wide as the picture has no smoothing distance");
    float z[kZones][3]; for (int i = 0; i < kZones; i++) { z[i][0] = i * 10.f; z[i][1] = 0; z[i][2] = 255.f - i; }
    float out[150 * 3]; applyTable(t, 150, z, out, smoothInfo(t, 150, 0));
    CHECK(fabsf(out[0] - 0.f) < 1.f && fabsf(out[149 * 3] - 50.f) < 1.f, "applyTable endpoints"); }
  { // which side the LEDs are on decides which part of the colour map they use
    Entry top[100], bot[100], lef[100], rig[100];
    tableLinear(top, 100, false, -1, -1, 0); tableLinear(bot, 100, false, -1, -1, 2);
    tableLinear(lef, 100, false, -1, -1, 3); tableLinear(rig, 100, false, -1, -1, 1);
    CHECK(top[0].a == 0 && top[99].b == 5, "ceiling (top) run uses the top row, zones 0..5");
    CHECK(bot[0].a == 18 && bot[99].b == 23, "run under the TV uses the bottom row, zones 18..23");
    CHECK(lef[0].a == 0 && lef[99].b == 18, "run on the left uses the left column, zones 0,6,12,18 (top to bottom)");
    CHECK(rig[0].a == 5 && rig[99].b == 23, "run on the right uses the right column, zones 5,11,17,23 (top to bottom)");
    Entry r[100]; tableLinear(r, 100, true, -1, -1, 2);
    CHECK(r[0].a == 23 && r[99].a == 18, "LED 0 at the right end of a bottom run starts at zone 23"); }
  { // a run along the left side that is longer than the TV: the tail beyond it is smoothed too
    Entry t[180]; tableLinear(t, 180, false, 50, 130, 3);
    CHECK(t[50].d == 0 && t[100].d == 0 && t[130].d == 0 && t[0].d > 0 && t[179].d > 0, "vertical runs get a smoothing tail too"); }
  { // a 210-LED run with the TV on LEDs 91..174: exact over the TV, eases to the average beyond it
    Entry t[210]; tableLinear(t, 210, false, 91, 174);
    CHECK(t[91].d == 0 && t[130].d == 0 && t[174].d == 0, "no distance over the TV");
    CHECK(t[0].d > t[50].d && t[209].d > t[180].d && t[0].d > 0 && t[209].d > 0, "distance grows beyond the TV");
    float z[kZones][3]; for (int i = 0; i < kZones; i++) { z[i][0] = (i % 6 == 0) ? 255.f : 0.f; z[i][1] = 0; z[i][2] = 0; }
    const float avgL = z[0][0], avgRt = z[5][0];   // each tail ends on the corner zone at its end of the strip
    const SmoothInfo si = smoothInfo(t, 210, 100);
    float out[210 * 3]; applyTable(t, 210, z, out, si);
    CHECK(si.firstZero > 0 && si.lastZero < 209 && si.lenStart > si.lenEnd && si.lenEnd >= kMinOutQ, "each tail gets its own length (%d, %d)", si.lenStart, si.lenEnd);
    CHECK(fabsf(out[91 * 3] - 255.f) < 1.f, "over the TV the edge zone colour is exact (%.1f)", out[91 * 3]);
    CHECK(fabsf(out[0] - avgL) < 2.f, "the far end of the strip reaches its corner zone (%.1f vs %.1f)", out[0], avgL);
    CHECK(fabsf(out[209 * 3] - avgRt) < 2.f, "the SHORTER tail also reaches its target at its own end (%.1f vs %.1f)", out[209 * 3], avgRt);
    CHECK(fabsf(out[50 * 3] - out[91 * 3]) < 1.f && fabsf(out[50 * 3] - out[0]) < 1.f, "beyond the TV the strip holds the corner zone colour (%.1f at LED 50)", out[50 * 3]);
    float prev = out[91 * 3]; bool mono = true;
    for (int i = 90; i >= 0; i--) { if (out[i * 3] > prev + 0.01f) mono = false; prev = out[i * 3]; }
    CHECK(mono, "the ease is monotonic (no bumps)");
    applyTable(t, 210, z, out, smoothInfo(t, 210, 0));
    CHECK(fabsf(out[0] - 255.f) < 1.f, "smoothing off: the strip beyond the TV repeats the edge colour"); }
  { // a strip on the ceiling (top edge) must only ever see the top row: a green dot in the bottom-left corner of the picture
    // (zone 18) must not tint the LEDs beyond the TV; they ease toward the average of the TOP ROW
    Entry t[210]; tableLinear(t, 210, false, 82, 172, 0);
    const SmoothInfo si = smoothInfo(t, 210, 100);
    CHECK(si.maskStart == (1u << 0) && si.maskEnd == (1u << 5), "a top strip: left tail -> top-left zone (0), right tail -> top-right zone (5): %x %x", si.maskStart, si.maskEnd);
    float z[kZones][3] = {};
    z[18][0] = 51.f; z[18][1] = 87.f; z[18][2] = 13.f;
    float out[210 * 3]; applyTable(t, 210, z, out, si);
    float worst = 0.f; for (int i = 0; i < 210 * 3; i++) worst = fmaxf(worst, out[i]);
    CHECK(worst < 0.5f, "a green dot at the bottom-left leaves the ceiling strip dark (brightest value %.2f)", worst);
    // an orange dot in the top-left corner (zone 0): the left tail shows it, the right tail must stay dark
    for (int i = 0; i < kZones; i++) z[i][0] = z[i][1] = z[i][2] = 0.f;
    z[0][0] = 255.f; z[0][1] = 120.f;
    applyTable(t, 210, z, out, si);
    CHECK(out[0] > 20.f && out[(81) * 3] > 100.f, "the left tail carries the orange (%.1f / %.1f)", out[0], out[81 * 3]);
    float rmax = 0.f; for (int i = 173 * 3; i < 210 * 3; i++) rmax = fmaxf(rmax, out[i]);
    CHECK(rmax < 0.5f, "an orange dot in the top-left corner leaves the right tail dark (brightest %.2f)", rmax);
    // a blue dot near the top right middle (zone 3) must not reach the right tail until it is in the corner zone (5)
    for (int i = 0; i < kZones; i++) z[i][0] = z[i][1] = z[i][2] = 0.f;
    z[3][2] = 255.f;
    applyTable(t, 210, z, out, si);
    float bmax = 0.f; for (int i = 0; i < 210; i++) if (i > 172 || i < 80) bmax = fmaxf(bmax, out[i * 3 + 2]);
    CHECK(bmax < 0.5f, "a blue dot in zone 3 leaves both tails dark (brightest %.2f)", bmax);
    CHECK(out[140 * 3 + 2] > 100.f, "...while it lights the TV part (%.1f)", out[140 * 3 + 2]);
    z[3][2] = 0.f; z[4][2] = 255.f;                                               // zone 4, the neighbour of the corner
    applyTable(t, 210, z, out, si);
    bmax = 0.f; for (int i = 173; i < 210; i++) bmax = fmaxf(bmax, out[i * 3 + 2]);
    CHECK(bmax < 0.5f, "a blue dot in zone 4 leaves the right tail dark too (brightest %.2f)", bmax);
    z[4][2] = 0.f; z[5][2] = 255.f;                                               // the corner zone itself
    applyTable(t, 210, z, out, si);
    CHECK(out[209 * 3 + 2] > 250.f && out[180 * 3 + 2] > 250.f, "a blue dot in the corner zone lights the right tail (%.1f)", out[209 * 3 + 2]);
    Entry b[210]; tableLinear(b, 210, false, 82, 172, 2);                         // strip under the TV: bottom row
    const SmoothInfo sb = smoothInfo(b, 210, 100);
    CHECK(sb.maskStart == (1u << 18) && sb.maskEnd == (1u << 23), "a bottom strip uses its corner zones (18 / 23): %x %x", sb.maskStart, sb.maskEnd);
    Entry r2[210]; tableLinear(r2, 210, true, 82, 172, 0);                        // LED 0 at the right end: the tails swap sides
    const SmoothInfo sr = smoothInfo(r2, 210, 100);
    CHECK(sr.maskStart == (1u << 5) && sr.maskEnd == (1u << 0), "LED 0 on the right: the first tail uses the top-right corner (%x %x)", sr.maskStart, sr.maskEnd);
    Entry l[180]; tableLinear(l, 180, false, 50, 130, 3);
    const SmoothInfo sl = smoothInfo(l, 180, 100);
    CHECK(sl.maskStart == (1u << 0) && sl.maskEnd == (1u << 18), "a left strip uses its corner zones (0 / 18): %x %x", sl.maskStart, sl.maskEnd); }
  { // the picture map: which zones a strip follows (independent of the Python reference)
    Entry top[210], right[266];
    tableLinear(top, 210, false, 82, 172, 0);
    tableLinear(right, 266, false, -1, -1, 1);
    float ut[kZones], ur[kZones];
    zoneUsage(top, 210, ut);
    zoneUsage(right, 266, ur);
    bool topOnly = true, rightOnly = true, interiorUnused = true;
    for (int z = 0; z < kZones; z++) {
      if (z > 5 && ut[z] > 0.001f) topOnly = false;                                     // a ceiling strip only follows the top row, zones 0-5
      if (z != 5 && z != 11 && z != 17 && z != 23 && ur[z] > 0.001f) rightOnly = false;   // a strip on the right only follows that column
      if ((z % 6 >= 1 && z % 6 <= 4 && z / 6 >= 1 && z / 6 <= 2) && (ut[z] > 0.001f || ur[z] > 0.001f)) interiorUnused = false;
    }
    CHECK(topOnly && rightOnly, "a ceiling strip follows only zones 0-5, a right-hand strip only the right column");
    CHECK(interiorUnused, "the 8 inner zones are never used");
    CHECK(ut[0] > ut[1] && ut[5] > ut[4] && ut[0] > 40.f, "the corner zones carry the stretch of strip beyond the TV (zone 0: %.1f, zone 1: %.1f)", ut[0], ut[1]);
    // the box's zones are not laid out symmetrically (the two middle rows sit a little above the centre of the picture), so the two ends differ a little
    CHECK(fabsf(ur[5] - ur[23]) < 8.f && ur[11] > 40.f && ur[17] > 40.f, "a right-hand strip spreads over its column (zones 5, 11, 17, 23: %.1f %.1f %.1f %.1f)", ur[5], ur[11], ur[17], ur[23]);
  }
  { // clicking a zone on the picture map lights the LEDs that follow it
    Entry top[210];
    tableLinear(top, 210, false, 82, 172, 0);
    int total = 0, zonesWithRuns = 0;
    bool contiguous = true, none = true;
    for (int z = 0; z < kZones; z++) {
      int runs[8][2];
      const int n = zoneRuns(top, 210, z, runs, 8);
      if (n > 0) zonesWithRuns++;
      if (z > 5 && n != 0) none = false;                                  // a ceiling strip never follows a zone of the lower rows
      int prevEnd = -1;
      for (int k = 0; k < n; k++) {
        total += runs[k][1];
        if (runs[k][0] <= prevEnd) contiguous = false;                  // runs are in order and do not touch
        prevEnd = runs[k][0] + runs[k][1];
      }
    }
    CHECK(total == 210, "every LED of the strip belongs to exactly one zone (%d of 210)", total);
    CHECK(zonesWithRuns == 6 && none && contiguous, "a ceiling strip is covered by the six top zones, each a separate stretch (%d zones)", zonesWithRuns);
    int r0[8][2], r3[8][2];
    const int n0 = zoneRuns(top, 210, 0, r0, 8), n3 = zoneRuns(top, 210, 3, r3, 8);
    CHECK(n0 == 1 && r0[0][0] == 0 && r0[0][1] > 60, "zone 0 holds the left end of the strip (run of %d from LED %d)", r0[0][1], r0[0][0]);
    CHECK(n3 == 1 && r3[0][0] > r0[0][1] && r3[0][1] >= 10 && r3[0][1] <= 20, "zone 3 is a stretch of about 15 LEDs in the middle (%d from %d)", r3[0][1], r3[0][0]);
    int few[2][2];
    CHECK(zoneRuns(top, 210, 0, few, 0) == 0, "no room for runs: none returned");
    Entry loop[142];
    CHECK(tableLoop(loop, 142, 0, false, nullptr, 0, 16, 9), "a loop of 142 LEDs builds");
    int lr[8][2];
    CHECK(zoneRuns(loop, 142, 5, lr, 8) <= 2 && zoneRuns(loop, 142, 5, lr, 8) >= 1, "the top-right corner zone is one stretch or two (round the corner)");
  }
  { // firmware updates: what is accepted as an image, the marker scan and the password rules
    uint8_t img[200] = {};
    img[0] = 0xE9; img[12] = 9; img[13] = 0;                              // an ESP32-S3 image ...
    img[32] = 0x32; img[33] = 0x54; img[34] = 0xCD; img[35] = 0xAB;       // ... whose first segment starts with the app descriptor
    CHECK(checkAppImage(img, sizeof img, kChipEsp32S3) == kImageOk, "a well-formed ESP32-S3 app image is accepted");
    CHECK(checkAppImage(img, kImageHeadBytes - 1, kChipEsp32S3) == kImageTooShort && checkAppImage(nullptr, 500, kChipEsp32S3) == kImageTooShort, "too little data to judge: refused");
    uint8_t bad[200]; memcpy(bad, img, sizeof bad); bad[0] = 0x00;
    CHECK(checkAppImage(bad, sizeof bad, kChipEsp32S3) == kImageBadMagic, "a file that does not start with 0xE9 is refused");
    memcpy(bad, img, sizeof bad); bad[12] = 5;
    CHECK(checkAppImage(bad, sizeof bad, kChipEsp32S3) == kImageWrongChip, "an image for another chip (ESP32-C3) is refused");
    memcpy(bad, img, sizeof bad); bad[33] = 0;
    CHECK(checkAppImage(bad, sizeof bad, kChipEsp32S3) == kImageNoDescriptor, "a bootloader / partition image without an app descriptor is refused");
    CHECK(strlen(imageCheckMessage(kImageBadMagic)) > 20 && imageCheckMessage(kImageOk)[0] == 0, "every refusal has a message, success has none");
    // the marker: found whole, split over chunks of any size, and not found in other data
    const size_t ml = strlen(kImageMark);
    std::vector<uint8_t> blob(3000, 0x41);
    memcpy(blob.data() + 1234, kImageMark, ml);
    for (size_t chunk : {1u, 2u, 3u, 7u, 64u, 1000u, 3000u}) {
      MarkScanner sc;
      for (size_t o = 0; o < blob.size(); o += chunk) markScan(sc, blob.data() + o, std::min(chunk, blob.size() - o));
      CHECK(sc.found, "the mark is found with chunks of %zu bytes", chunk);
    }
    MarkScanner none;
    std::vector<uint8_t> other(5000);
    for (size_t i = 0; i < other.size(); i++) other[i] = (uint8_t)(i * 31 + 7);
    markScan(none, other.data(), other.size());
    CHECK(!none.found, "other data does not contain the mark");
    MarkScanner almost;                                                      // the mark with its last character missing, then the real one
    std::string tricky = std::string(kImageMark, ml - 1) + "X" + std::string(kImageMark);
    markScan(almost, (const uint8_t*)tricky.data(), tricky.size());
    CHECK(almost.found, "a near miss right before the real mark does not hide it");
    MarkScanner partial;
    markScan(partial, (const uint8_t*)kImageMark, ml - 1);
    CHECK(!partial.found, "all but the last character is not the mark");
    CHECK(!validUpdatePassword(nullptr) && !validUpdatePassword("") && !validUpdatePassword("short") && !validUpdatePassword("1234567"), "passwords under 8 characters are refused");
    CHECK(validUpdatePassword("12345678") && validUpdatePassword("correct horse battery staple") && validUpdatePassword("p@ss w0rd!"), "8+ printable characters, spaces included, are accepted");
    CHECK(validUpdatePassword(std::string(64, 'a').c_str()) && !validUpdatePassword(std::string(65, 'a').c_str()), "64 characters is the longest");
    CHECK(!validUpdatePassword("pass\tword1") && !validUpdatePassword("passw\xC3\xB6rd1"), "control characters and non-ASCII are refused");
    CHECK(secureEquals("hunter2hunter2", "hunter2hunter2") && !secureEquals("hunter2hunter2", "hunter2hunter3") && !secureEquals("abc", "abcd") && !secureEquals("abcd", "abc") && secureEquals("", ""), "password comparison"); }
  { // vivid blending, on its own terms
    const float red[3] = {255, 0, 0}, green[3] = {0, 255, 0}, blue[3] = {0, 0, 255}, grey[3] = {120, 120, 120}, black[3] = {0, 0, 0}, white[3] = {255, 255, 255};
    float o[3];
    vividBlend(red, green, 0.f, o);
    CHECK(fabsf(o[0] - 255) < 0.01f && fabsf(o[1]) < 0.01f && fabsf(o[2]) < 0.01f, "w = 0 gives the first colour back exactly");
    vividBlend(red, green, 1.f, o);
    CHECK(fabsf(o[0]) < 0.01f && fabsf(o[1] - 255) < 0.01f && fabsf(o[2]) < 0.01f, "w = 1 gives the second colour back exactly");
    vividBlend(red, green, 0.5f, o);
    CHECK(o[0] > 250.f && o[1] > 250.f && o[2] < 5.f, "red and green meet in bright yellow (%.0f,%.0f,%.0f), where an RGB mix gives a dark 127,127,0", o[0], o[1], o[2]);
    vividBlend(red, blue, 0.5f, o);
    CHECK(o[0] > 250.f && o[2] > 250.f && o[1] < 5.f, "red and blue meet in bright magenta (%.0f,%.0f,%.0f)", o[0], o[1], o[2]);
    bool vivid = true;                                         // between two fully saturated colours the result stays fully saturated and bright
    for (float w = 0.f; w <= 1.001f; w += 0.05f) {
      vividBlend(green, blue, w, o);
      const float mx = fmaxf(o[0], fmaxf(o[1], o[2])), mn = fminf(o[0], fminf(o[1], o[2]));
      if (mx < 250.f || mn > 5.f) vivid = false;
    }
    CHECK(vivid, "green to blue never loses its saturation or brightness on the way");
    vividBlend(black, green, 0.5f, o);
    CHECK(o[1] > 125.f && o[1] < 130.f && o[0] < 1.f && o[2] < 1.f, "black to green is a darker green (%.0f,%.0f,%.0f): black has no hue to pull the blend away", o[0], o[1], o[2]);
    vividBlend(grey, red, 0.5f, o);
    CHECK(o[0] > o[1] && o[0] > o[2] && fabsf(o[1] - o[2]) < 1.f, "grey to red keeps red''s hue (%.0f,%.0f,%.0f)", o[0], o[1], o[2]);
    vividBlend(white, white, 0.5f, o);
    CHECK(fabsf(o[0] - 255) < 0.1f && fabsf(o[1] - 255) < 0.1f && fabsf(o[2] - 255) < 0.1f, "white with white stays white");
    // used in a table: LEDs that sit on one zone are untouched, and the result equals the RGB mix there
    std::vector<Entry> tb(60); tableLinear(tb.data(), 60, false, -1, -1, 0);
    float zn[kZones][3] = {}; for (int z = 0; z < 6; z++) { zn[z][0] = (float)(z * 40); zn[z][1] = 255.f - z * 40; zn[z][2] = 30.f; }
    const SmoothInfo si0 = smoothInfo(tb.data(), 60, 0);
    std::vector<float> rgbOut(180), vivOut(180);
    applyTable(tb.data(), 60, zn, rgbOut.data(), si0, false);
    applyTable(tb.data(), 60, zn, vivOut.data(), si0, true);
    int same = 0, diff = 0;
    for (int i = 0; i < 60; i++) {
      const bool pure = tb[i].a == tb[i].b || tb[i].f == 0 || tb[i].f == 255;
      const bool eq = fabsf(rgbOut[i * 3] - vivOut[i * 3]) < 0.01f && fabsf(rgbOut[i * 3 + 1] - vivOut[i * 3 + 1]) < 0.01f && fabsf(rgbOut[i * 3 + 2] - vivOut[i * 3 + 2]) < 0.01f;
      if (pure) same += eq ? 1 : 0; else diff += eq ? 0 : 1;
    }
    CHECK(same > 0 && diff > 0, "in a table, LEDs on a single zone are identical either way (%d) and blended ones differ (%d)", same, diff); }  { // zone edges (sharpenTable) and per-target saturation, checked on their own terms as well as against the Python reference
    auto make = []() { std::vector<Entry> v(210); tableLinear(v.data(), 210, false, 82, 172, 0); return v; };
    const std::vector<Entry> soft = make();
    std::vector<Entry> t0 = soft; sharpenTable(t0.data(), 210, 0);
    bool same = true; for (int i = 0; i < 210; i++) if (t0[i].f != soft[i].f || t0[i].a != soft[i].a || t0[i].b != soft[i].b || t0[i].d != soft[i].d) same = false;
    CHECK(same, "0 %% keeps the smooth blend exactly");
    std::vector<Entry> hard = soft; sharpenTable(hard.data(), 210, 100);
    bool onlyEnds = true, tailsKept = true, zonesKept = true;
    for (int i = 0; i < 210; i++) {
      if (hard[i].a != soft[i].a || hard[i].b != soft[i].b || hard[i].d != soft[i].d) zonesKept = false;
      if (soft[i].a == soft[i].b) { if (hard[i].f != soft[i].f) tailsKept = false; }
      else if (hard[i].f != 0 && hard[i].f != 255) onlyEnds = false;
    }
    CHECK(onlyEnds && tailsKept && zonesKept, "100 %% gives every blended LED to one zone (f is 0 or 255), leaves the tails and the zone numbers alone");
    { Entry e[3] = {Entry{2, 3, 127, 0}, Entry{2, 3, 128, 0}, Entry{4, 4, 77, 9}}; sharpenTable(e, 3, 100);
      CHECK(e[0].f == 0 && e[1].f == 255 && e[2].f == 77, "the line between two zones is at 127.5: 127 goes to the first, 128 to the second, a single-zone entry is untouched"); }
    bool monotone = true;
    for (int pct : {10, 40, 50, 75, 90, 99}) {
      uint8_t prev = 0;
      for (int f = 0; f < 256; f++) {
        Entry e{2, 3, (uint8_t)f, 0}; sharpenTable(&e, 1, pct);
        if (e.f < prev) monotone = false;
        prev = e.f;
      }
    }
    CHECK(monotone, "more f never gives less of the second zone, at any sharpness");
    { Entry a{2, 3, 60, 0}, b{2, 3, 195, 0}, c{2, 3, 100, 0}; sharpenTable(&a, 1, 50); sharpenTable(&b, 1, 50); sharpenTable(&c, 1, 50);
      CHECK(a.f == 0 && b.f == 255 && c.f > 60 && c.f < 100, "50 %% squeezes the change into the middle half: 60 -> 0, 195 -> 255, 100 -> %d (less than before)", c.f); }
    { // the picture map of a hard top strip: whole LEDs per zone, still adding up to the LED count
      float u[kZones], us[kZones];
      zoneUsage(hard.data(), 210, u); zoneUsage(soft.data(), 210, us);
      float sum = 0; bool whole = true;
      for (int z = 0; z < kZones; z++) { sum += u[z]; if (fabsf(u[z] - roundf(u[z])) > 1e-3f) whole = false; }
      CHECK(fabsf(sum - 210.f) < 1e-2f && whole, "a hard strip puts whole LEDs on each zone (%.1f in total)", sum);
      CHECK(u[2] > us[2] - 0.01f || u[2] > 10.f, "an inner top zone keeps about its own stretch of the strip when the edges are hard (%.1f vs %.1f)", u[2], us[2]); }
    // saturation
    float c1[6] = {200, 100, 100, 50, 50, 50};
    saturateColours(c1, 2, 1.f);
    CHECK(c1[0] == 200 && c1[1] == 100 && c1[3] == 50, "saturation 1 changes nothing");
    float c2[6] = {200, 100, 100, 50, 50, 50};
    saturateColours(c2, 2, 0.f);
    const float y = 0.299f * 200 + 0.587f * 100 + 0.114f * 100;
    CHECK(fabsf(c2[0] - y) < 1e-3f && fabsf(c2[1] - y) < 1e-3f && fabsf(c2[2] - y) < 1e-3f, "saturation 0 is the grey of the same brightness (%.1f)", c2[0]);
    float c3[6] = {200, 100, 100, 50, 50, 50};
    saturateColours(c3, 2, 1.5f);
    CHECK(c3[0] > 200 && c3[1] < 100 && c3[3] == 50 && c3[4] == 50 && c3[5] == 50, "saturation 1.5 pushes a colour away from its grey; a grey stays grey");
    float c4[3] = {250, 10, 0};
    saturateColours(c4, 1, 3.f);
    CHECK(c4[0] <= 255.f && c4[1] >= 0.f && c4[2] >= 0.f, "results stay between 0 and 255"); }  { // darkOutside: only what lies outside the TV part goes dark
    float d1[30];
    for (int k = 0; k < 30; k++) d1[k] = 100.f + k;
    darkOutside(d1, 10, 3, 6);
    bool inside = true, outside = true;
    for (int i = 0; i < 10; i++)
      for (int c = 0; c < 3; c++) {
        const bool in = i >= 3 && i <= 6;
        if (in && d1[i * 3 + c] != 100.f + i * 3 + c) inside = false;
        if (!in && d1[i * 3 + c] != 0.f) outside = false;
      }
    CHECK(inside, "darkOutside leaves the LEDs of the TV part (3..6) untouched");
    CHECK(outside, "darkOutside switches off the LEDs before and after it (0..2, 7..9)");
    float d2[6] = {1, 2, 3, 4, 5, 6};
    darkOutside(d2, 2, -1, -1);
    CHECK(d2[0] == 1 && d2[5] == 6, "no TV part set: nothing goes dark");
    darkOutside(d2, 2, 5, 3);
    CHECK(d2[0] == 1 && d2[5] == 6, "an upside-down TV part is ignored");
    float d3[6] = {1, 2, 3, 4, 5, 6};
    darkOutside(d3, 2, 0, 9);
    CHECK(d3[0] == 1 && d3[5] == 6, "a TV part longer than the strip darkens nothing");
    darkOutside(nullptr, 2, 0, 1); }  { // delay tracking: the ring of round trips, and the model of how long a WLED needs to show a frame
    RttRing r;
    const RttSummary s0 = rttSummary(r);
    CHECK(s0.samples == 0 && s0.lost == 0 && s0.avgMs < 0 && s0.minMs < 0 && s0.maxMs < 0, "no probe yet: nothing to report");
    rttAdd(r, 4.f); rttAdd(r, 6.f); rttAdd(r, 5.f);
    const RttSummary s1 = rttSummary(r);
    CHECK(s1.samples == 3 && s1.lost == 0 && fabsf(s1.avgMs - 5.f) < 1e-4f && s1.minMs == 4.f && s1.maxMs == 6.f, "three probes: avg %.2f min %.2f max %.2f", s1.avgMs, s1.minMs, s1.maxMs);
    rttAdd(r, -1.f);
    const RttSummary s2 = rttSummary(r);
    CHECK(s2.samples == 4 && s2.lost == 1 && fabsf(s2.avgMs - 5.f) < 1e-4f && s2.minMs == 4.f && s2.maxMs == 6.f, "a lost probe is counted as lost and left out of the times");
    for (int i = 0; i < 3 * kRttSamples; i++) rttAdd(r, 10.f);
    const RttSummary s3 = rttSummary(r);
    CHECK(s3.samples == kRttSamples && s3.lost == 0 && s3.avgMs == 10.f && s3.minMs == 10.f && s3.maxMs == 10.f, "only the last %d probes count (the older ones are overwritten)", kRttSamples);
    RttRing lost;
    for (int i = 0; i < 5; i++) rttAdd(lost, -5.f);
    const RttSummary s4 = rttSummary(lost);
    CHECK(s4.samples == 5 && s4.lost == 5 && s4.avgMs < 0, "every probe lost: no times, 5 of 5 lost");
    CHECK(fabsf(wledShowMs(600, 35.f) - (18.f + 500.f / 35.f)) < 1e-3f, "600 LEDs at 35 fps: 18 ms clocking out + half a refresh = %.1f ms", wledShowMs(600, 35.f));
    CHECK(fabsf(wledShowMs(210, 0.f) - 6.3f) < 1e-3f && wledShowMs(0, 30.f) < 0.f && wledShowMs(-3, 30.f) < 0.f, "fps unknown: only the clocking out; no LEDs: no estimate"); }  { // motion smoothing: what must hold whatever the numbers
    Motion m;
    motionReset(m);
    float zl[kZones][3], zh[kZones][3], o[kZones][3];
    for (int q = 0; q < kZones; q++) for (int c = 0; c < 3; c++) { zl[q][c] = 0.f; zh[q][c] = 100.f; }
    CHECK(motionAt(m, 123u, o) && o[0][0] == 0.f && o[23][2] == 0.f, "no picture yet: all zones 0, nothing to wait for");
    motionPush(m, 1000u, zl);
    motionPush(m, 1100u, zh);
    CHECK(m.n == 2 && m.k[0].ms == 1000u && m.k[1].ms == 1100u, "two pictures 100 ms apart sit 100 ms apart on the clock (%u, %u)", (unsigned)m.k[0].ms, (unsigned)m.k[1].ms);
    bool reached = motionAt(m, 1050u, o);
    CHECK(!reached && fabsf(o[5][1] - 50.f) < 1e-3f, "half way between two pictures: half way between their colours (%.2f)", o[5][1]);
    reached = motionAt(m, 1100u, o);
    CHECK(reached && o[5][1] == 100.f, "at the newest picture: its colours, and nothing more to wait for");
    reached = motionAt(m, 5000u, o);
    CHECK(reached && o[5][1] == 100.f, "long after it: the newest picture is held (no guessing ahead)");
    reached = motionAt(m, 900u, o);
    CHECK(!reached && o[5][1] == 0.f, "before the first picture: the first is held");
    motionPush(m, 1110u, zl);                                      // 10 ms after the last one: a burst, it replaces that picture
    CHECK(m.n == 2 && m.k[1].ms == 1100u && m.k[1].z[3][0] == 0.f, "a picture within 25 ms of the previous one replaces it instead of adding a knot");
    motionPush(m, 4000u, zh);                                      // the box was silent for 2.9 s: start over
    CHECK(m.n == 1 && m.k[0].ms == 4000u && m.periodMs == 100.f, "after a long silence the clock starts over");
    Motion r6;
    motionReset(r6);
    for (int i = 0; i < 6; i++) { zl[0][0] = (float)(i * 10); motionPush(r6, 10000u + i * 100u, zl); }
    CHECK(r6.n == kMotionKnots && r6.k[0].z[0][0] == 20.f && r6.k[kMotionKnots - 1].z[0][0] == 50.f, "the ring keeps the %d latest pictures", kMotionKnots);
    Motion w;
    motionReset(w);
    zl[0][0] = 0.f; zh[0][0] = 100.f;
    motionPush(w, 0xFFFFFF00u, zl);
    motionPush(w, 0xFFFFFF64u, zh);
    motionAt(w, 0xFFFFFF32u, o);
    CHECK(fabsf(o[0][0] - 50.f) < 1e-3f, "half way across a pair of pictures just before the 32-bit millisecond clock wraps (%.2f)", o[0][0]);
    reached = motionAt(w, 0x00000010u, o);
    CHECK(reached && o[0][0] == 100.f, "after the clock wrapped, the newest picture is held and reached");
    Motion s3;                                                      // a still picture: sliding between identical pictures changes nothing, so it counts as settled
    motionReset(s3);
    zl[0][0] = 5.f;
    for (int i = 0; i < 4; i++) motionPush(s3, 7000u + i * 100u, zl);
    reached = motionAt(s3, 7150u, o);
    CHECK(reached && o[0][0] == 5.f, "between identical pictures the output is settled (nothing to tick for)");
    zh[0][0] = 99.f;
    motionPush(s3, 7400u, zh);                                      // a different picture arrives
    reached = motionAt(s3, 7380u, o);
    CHECK(!reached, "as soon as a different picture is in the history, sliding is under way again");
    reached = motionAt(s3, 7250u, o);
    CHECK(!reached && o[0][0] == 5.f, "...even before the different picture is reached (it is the stretch before it that still moves)");
    CHECK(motionDelayMs(m, 0) == 0.f && fabsf(motionDelayMs(m, 100) - 100.f) < 1e-3f && fabsf(motionDelayMs(m, 50) - 50.f) < 1e-3f && motionDelayMs(m, 150) == motionDelayMs(m, 100), "the delay is a share of one interval, from 0 to 100 percent"); }  { // blur along a strip
    CHECK(blurHalfWidth(0) == 0 && blurHalfWidth(-4) == 0 && blurHalfWidth(1) == 1 && blurHalfWidth(5) == 2 && blurHalfWidth(20) == 10, "blur widths: 0 off, 1 -> 1, 5 -> 2, 20 -> 10 (%d %d %d)", blurHalfWidth(1), blurHalfWidth(5), blurHalfWidth(20));
    float flat[20 * 3], scratch[2 * 20 * 3];
    for (int i = 0; i < 20 * 3; i++) flat[i] = 77.f;
    blurColours(flat, 20, -1, -1, 9, scratch);
    bool same = true;
    for (int i = 0; i < 20 * 3; i++) if (fabsf(flat[i] - 77.f) > 1e-3f) same = false;
    CHECK(same, "a flat colour stays flat, ends included");
    float dot[41 * 3] = {}, scr2[2 * 41 * 3];
    dot[20 * 3] = 255.f;
    blurColours(dot, 41, -1, -1, 5, scr2);
    float total = 0.f, asym = 0.f;
    int widest = 0;
    for (int i = 0; i < 41; i++) { total += dot[i * 3]; if (dot[i * 3] > 0.01f && abs(i - 20) > widest) widest = abs(i - 20); asym = fmaxf(asym, fabsf(dot[(20 - (i - 20)) * 3 < 0 || 20 - (i - 20) > 40 ? 0 : (20 - (i - 20)) * 3] - dot[i * 3])); }
    CHECK(fabsf(total - 255.f) < 0.05f, "a single bright LED keeps its light when blurred (%.2f of 255)", total);
    CHECK(dot[20 * 3] < 100.f && dot[20 * 3] > 30.f && widest == 6 && asym < 1e-3f, "it spreads symmetrically over 6 LEDs each way (peak %.1f, reach %d)", dot[20 * 3], widest);
    float part[20 * 3], orig[20 * 3];
    for (int i = 0; i < 20 * 3; i++) orig[i] = part[i] = (float)((i * 37) % 200);
    blurColours(part, 20, 5, 14, 5, scratch);
    bool outsideSame = true, insideChanged = false;
    for (int i = 0; i < 20; i++) for (int c = 0; c < 3; c++) {
      const bool in = i >= 5 && i <= 14;
      if (!in && part[i * 3 + c] != orig[i * 3 + c]) outsideSame = false;
      if (in && fabsf(part[i * 3 + c] - orig[i * 3 + c]) > 1.f) insideChanged = true;
    }
    CHECK(outsideSame && insideChanged, "only the picture part (5..14) is blurred; the LEDs outside it keep their colours");
    blurColours(nullptr, 20, -1, -1, 5, scratch);
    blurColours(flat, 1, -1, -1, 5, scratch);
    blurColours(flat, 20, -1, -1, 0, scratch); }  { // Govee: base64 (RFC 4648 vectors), the two packets known from other projects, and the limits
    struct { const char* in; const char* want; } b64[] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    for (const auto& c : b64) {
      char o[32];
      const size_t n = base64Encode((const uint8_t*)c.in, strlen(c.in), o, sizeof o);
      CHECK(strlen(c.want) == n && !strcmp(o, c.want), "base64(\"%s\") = %s (want %s)", c.in, o, c.want);
    }
    char small[8];
    CHECK(base64Encode((const uint8_t*)"foobar", 6, small, sizeof small) == 0, "base64 refuses a buffer that is too small");
    char o[400];
    goveeEnableJson(true, o, sizeof o);
    CHECK(strstr(o, "\"pt\":\"uwABsQEK\"") != nullptr, "streaming on is the packet other Govee projects use (uwABsQEK): %s", o);
    goveeEnableJson(false, o, sizeof o);
    CHECK(strstr(o, "\"pt\":\"uwABsQAL\"") != nullptr, "streaming off is uwABsQAL: %s", o);
    uint8_t rgb[3 * (kGoveeMaxSegments + 1)] = {};
    CHECK(goveeFrameJson(rgb, 0, o, sizeof o) == 0 && goveeFrameJson(rgb, kGoveeMaxSegments + 1, o, sizeof o) == 0, "0 and 61 segments are refused");
    CHECK(goveeFrameJson(rgb, 60, o, sizeof o) > 0 && goveeFrameJson(rgb, 1, o, sizeof o) > 0, "1 and 60 segments are fine");
    CHECK(goveeFrameJson(rgb, 20, small, sizeof small) == 0, "a frame that does not fit the buffer is refused, not cut");
    CHECK(goveeFrameJson(nullptr, 5, o, sizeof o) == 0, "no colours, no frame"); }  { // TV off: the picture has been completely black for a while
    uint8_t blk[kZones][3] = {}, dim[kZones][3] = {}, one[kZones][3] = {};
    dim[7][2] = 1;                                                // a single blue count in one zone is not black
    one[23][0] = 1;
    CHECK(isBlackPicture(blk) && !isBlackPicture(dim) && !isBlackPicture(one), "a picture is black only when every channel of every zone is 0");
    BlackTracker t;
    CHECK(!tvIsOff(t, 5000, 0) && blackForMs(t, 5000) == 0, "nothing seen yet: not off");
    blackFrame(t, blk, 1000);
    CHECK(blackForMs(t, 1000) == 0 && !tvIsOff(t, 1000, 15000), "the first black frame starts the clock");
    blackFrame(t, blk, 8000); blackFrame(t, blk, 15999);
    CHECK(!tvIsOff(t, 15999, 15000) && blackForMs(t, 15999) == 14999, "14.999 s of black is not yet off (the clock runs from the first black frame)");
    blackFrame(t, blk, 16000);
    CHECK(tvIsOff(t, 16000, 15000), "15 s of black = off");
    blackFrame(t, blk, 16100);
    CHECK(tvIsOff(t, 16100, 15000) && blackForMs(t, 16100) == 15100, "more black frames do not restart the clock");
    blackFrame(t, dim, 16200);
    CHECK(!tvIsOff(t, 16200, 15000) && blackForMs(t, 16200) == 0, "the first frame with any colour switches it back on at once");
    blackFrame(t, blk, 16300); blackFrame(t, blk, 20000);
    CHECK(!tvIsOff(t, 20000, 15000) && blackForMs(t, 20000) == 3700, "a new black run starts a new clock");
    blackReset(t);
    CHECK(!tvIsOff(t, 90000, 15000), "a silent box resets it: nothing is known");
    blackFrame(t, blk, 90000);
    CHECK(blackForMs(t, 90500) == 500 && !tvIsOff(t, 90500, 3000), "after a reset the clock starts again");
    // millis() wraps after 49.7 days: the arithmetic must not care
    BlackTracker w;
    blackFrame(w, blk, 0xFFFFFF00u);
    CHECK(!tvIsOff(w, 0xFFFFFFF0u, 1000) && tvIsOff(w, 0x00000400u, 1000) && blackForMs(w, 0x00000100u) == 0x200, "a wrap of the millisecond counter does not matter");
    CHECK(tvIsOff(w, 0xFFFFFF00u, 0), "a delay of 0 means off at the first black frame"); }
  { // the WLED address box: pasted URLs are cleaned up
    struct { const char* in; const char* want; } cases[] = {
      {"wled-ceiling.local", "wled-ceiling.local"},
      {"http://wled-ceiling.local", "wled-ceiling.local"},
      {"  HTTP://Wled-Ceiling.local/  ", "wled-ceiling.local"},
      {"https://wled-cabinet.local/json/info?x=1", "wled-cabinet.local"},
      {"192.0.2.51", "192.0.2.51"},
      {"http://192.0.2.51:80/", "192.0.2.51"},
      {"wled.local.", "wled.local"},
      {"wled.local:80", "wled.local"},
      {"wled.local:8080", "wled.local:8080"},          // another port is kept, so the validation can refuse it with a clear message
      {"", ""}, {"   ", ""}, {"http://", ""}, {"/only/a/path", ""},
    };
    for (const auto& c : cases) {
      char out[64];
      const int n = normalizeHost(c.in, out, sizeof out);
      CHECK(n == (int)strlen(c.want) && strcmp(out, c.want) == 0, "normalizeHost(\"%s\") = \"%s\" (want \"%s\")", c.in, n < 0 ? "<too long>" : out, c.want);
    }
    char small[8];
    CHECK(normalizeHost("a-very-long-name.local", small, sizeof small) == -1, "an address that does not fit is refused, not cut short");
    CHECK(normalizeHost("1234567", small, sizeof small) == 7 && strcmp(small, "1234567") == 0, "an address that exactly fits is kept");
    CHECK(normalizeHost(nullptr, small, sizeof small) == -1, "a null address is refused"); }
  { // time-based smoothing: same result however the time is sliced
    CHECK(fabsf(smoothDecay(0.5f, 33.333f, 33.333f) - 0.5f) < 1e-5f, "one reference tick leaves the setting's fraction");
    CHECK(fabsf(smoothDecay(0.5f, 66.667f, 33.333f) - 0.25f) < 1e-5f, "two reference ticks leave its square");
    CHECK(smoothDecay(0.5f, 0.f, 33.f) == 1.f, "no time passed: nothing moves");
    CHECK(smoothDecay(0.f, 5.f, 33.f) == 0.f, "smoothing 0 jumps straight to the target");
    CHECK(fabsf(smoothDecay(0.99f, 33.333f, 33.333f) - 0.95f) < 1e-5f, "smoothing is capped at 0.95");
    CHECK(fabsf(smoothDecay(-1.f, 10.f, 33.f)) < 1e-6f, "a negative setting means none");
    bool ok = true;
    for (float s : {0.1f, 0.3f, 0.5f, 0.8f, 0.95f})
      for (float a : {1.f, 4.f, 9.f, 20.f}) for (float b : {2.f, 7.f, 33.f}) {
        const float split = smoothDecay(s, a, 33.333f) * smoothDecay(s, b, 33.333f), whole = smoothDecay(s, a + b, 33.333f);
        if (fabsf(split - whole) > 1e-5f) { ok = false; printf("  s=%.2f a=%.0f b=%.0f split=%.6f whole=%.6f\n", s, a, b, split, whole); }
      }
    CHECK(ok, "two short steps equal one long step (the lights feel the same whatever the frame timing)");
    // a step response: 100 -> 0 reached after 10 irregular updates over 100 ms equals the regular 3-tick version
    float cur = 100.f; const float dts[10] = {3, 12, 7, 9, 14, 6, 11, 8, 20, 10};
    for (float d : dts) cur = 0.f + (cur - 0.f) * smoothDecay(0.5f, d, 33.333f);
    const float regular = 100.f * smoothDecay(0.5f, 100.f, 33.333f);
    CHECK(fabsf(cur - regular) < 1e-3f, "irregular updates reach the same value as regular ones (%.4f vs %.4f)", cur, regular); }
  { // reaction boost: undoes the Sync Box's own smoothing (y = 0.6 * previous + 0.4 * target)
    CHECK(leadBoost(120.f, 120.f, 1.f) == 120.f && leadBoost(7.f, 7.f, 0.5f) == 7.f, "a steady colour is not changed");
    CHECK(leadBoost(120.f, 40.f, 0.f) == 120.f, "boost 0 = the box's own value");
    // the box's output for a step from 0 to 200 and back, and what the boost reconstructs from it
    float y = 0.f, prev = 0.f; bool ok = true; float worst = 0.f;
    for (int n = 0; n < 12; n++) {
      const float x = n < 6 ? 200.f : 0.f;                       // the colour the box is aiming at
      prev = y; y = 0.6f * y + 0.4f * x;                         // what the box broadcasts
      const float est = leadBoost(y, prev, 1.f);
      if (fabsf(est - x) > worst) worst = fabsf(est - x);
      if (fabsf(est - x) > 0.01f) ok = false;
    }
    CHECK(ok, "with boost 1 the target is rebuilt exactly on every frame (worst error %.4f)", worst);
    CHECK(fabsf(leadBoost(80.f, 0.f, 0.5f) - 140.f) < 1e-3f, "boost 0.5 goes half way to the target: first frame 80 -> 140 (200 is the target)");
    CHECK(leadBoost(10.f, 200.f, 1.f) == 0.f && leadBoost(250.f, 50.f, 1.f) == 255.f, "the result stays a valid colour");
    CHECK(leadBoost(100.f, 0.f, 7.f) == leadBoost(100.f, 0.f, 1.f) && leadBoost(100.f, 0.f, -3.f) == 100.f, "boost is limited to 0..1");
    // other box speeds: y = (1 - s) * previous + s * target, so the target is rebuilt exactly from the packet before it, whatever the speed
    { float worst = 0.f;
      const int speeds[] = {10, 25, 40, 60, 90};
      for (int sp : speeds) {
        const float s = sp / 100.f;
        float y = 0.f;
        for (int n = 0; n < 12; n++) {
          const float prev = y;
          y = (1.f - s) * y + s * 200.f;
          const float est = leadBoost(y, prev, 1.f, sp, 1);
          const float err = fabsf(est - 200.f);
          if (err > worst) worst = err;
        }
      }
      CHECK(worst < 0.01f, "at every box speed the target is rebuilt exactly (worst error %.4f)", worst); }
    CHECK(leadBoost(200.f, 0.f, 1.f, 100, 1) == 200.f && leadBoost(55.f, 20.f, 1.f, 100, 1) == 55.f, "box speed 100 %%: the box does not smooth, nothing is added");
    CHECK(leadBoost(120.f, 40.f, 1.f, 40, 0) == 120.f, "no steps (a pause): the packets do not follow each other, nothing is added");
    // packets lost in between: after m steps the target is rebuilt from the last packet that arrived
    { const float s = 0.4f;
      float y = 0.f;
      for (int n = 0; n < 2; n++) y = (1.f - s) * y + s * 200.f;       // two packets of the change were seen: 80, 128
      const float seen = y;
      float after = seen;
      for (int n = 0; n < 3; n++) after = (1.f - s) * after + s * 200.f;     // three more steps happen, only the last one arrives
      CHECK(fabsf(leadBoost(after, seen, 1.f, 40, 3) - 200.f) < 0.01f, "three steps between two packets: the target is still rebuilt exactly");
      CHECK(leadBoost(after, seen, 1.f, 40, 1) > 200.f + 15.f, "...and counting them as one overshoots (that is why the steps are counted)"); }
    CHECK(boxStepsForGap(0) == 1 && boxStepsForGap(2) == 1 && boxStepsForGap(73) == 1 && boxStepsForGap(100) == 1 && boxStepsForGap(120) == 1, "packets together or a normal gap apart: one step");
    CHECK(boxStepsForGap(146) == 2 && boxStepsForGap(203) == 3 && boxStepsForGap(300) == 4, "a gap of two or three periods: that many steps (measured: 200 ms held 3)");
    CHECK(boxStepsForGap(600) == 8 && boxStepsForGap(601) == 0, "a pause of more than 600 ms: no extrapolation"); }
  { // the Sync Box's address (the setup page's "only accept this box") and why the board last restarted
    uint8_t b[4] = {0, 0, 0, 0};
    CHECK(parseIpv4("192.0.2.10", b) && b[0] == 192 && b[1] == 0 && b[2] == 2 && b[3] == 10, "a dotted address is read");
    CHECK(parseIpv4("0.0.0.0", nullptr) && parseIpv4("255.255.255.255", nullptr) && parseIpv4("1.2.3.4", nullptr), "the extremes are valid addresses");
    const char* bad[] = {"", " ", "192.0.2", "192.0.2.10.5", "192.0.2.256", "192.0.2.1000", "192.0.2.", ".0.2.10", "192..2.10", "192.0.2.010", "01.2.3.4", " 192.0.2.10", "192.0.2.10 ",
                         "192.0.2.10:80", "wled.local", "192.0.2.a", "-1.2.3.4", "1.2.3.4/24", "1,2,3,4"};
    bool allBad = true;
    for (const char* s : bad) if (parseIpv4(s, nullptr)) { allBad = false; printf("  accepted: '%s'\n", s); }
    CHECK(allBad && !parseIpv4(nullptr, nullptr), "names, extra text, spaces, missing or extra parts, numbers over 255 and leading zeros are refused");
    CHECK(validSenderAddress("192.0.2.10") && validSenderAddress("192.168.1.10") && validSenderAddress("223.1.1.1"), "an ordinary device address can be locked to");
    CHECK(!validSenderAddress("0.0.0.0") && !validSenderAddress("127.0.0.1") && !validSenderAddress("224.0.0.1") && !validSenderAddress("255.255.255.255") && !validSenderAddress("wled.local") && !validSenderAddress(""),
          "0.0.0.0 (which means anyone), loopback, multicast, broadcast, names and nothing are not sender addresses");
    CHECK(strcmp(resetReasonName(1), "power on") == 0 && strcmp(resetReasonName(3), "software restart") == 0 && strcmp(resetReasonName(4), "crash (panic)") == 0
          && strcmp(resetReasonName(9), "brownout (the power supply dipped)") == 0 && strcmp(resetReasonName(0), "unknown") == 0 && strcmp(resetReasonName(99), "unknown") == 0 && strcmp(resetReasonName(-1), "unknown") == 0,
          "reset reasons have names, and an unknown code is 'unknown'");
    CHECK(!resetReasonUnexpected(1) && !resetReasonUnexpected(2) && !resetReasonUnexpected(3) && !resetReasonUnexpected(0) && resetReasonUnexpected(4) && resetReasonUnexpected(5) && resetReasonUnexpected(6) && resetReasonUnexpected(7) && resetReasonUnexpected(9),
          "a normal start or restart is not flagged; a crash, a watchdog or a brownout is"); }
  { Entry t[10]; int bad[4] = {1, 1, 1, 1};
    CHECK(!tableLoop(t, 10, 0, false, bad, 0, 16, 9), "loop rejects sides that do not sum to count");
    CHECK(!tableLoop(t, 10, 7, false, nullptr, 0, 16, 9), "loop rejects bad corner"); }

  { // bulk input: the parsers that read data from outside must neither crash nor read past the end, whatever they are given.
    // Every input goes in a buffer of exactly its size, so a sanitizer (address, undefined: see test/run_core_tests.py) sees any overread.
    uint32_t rng = 0x9E3779B9u;
    auto rnd = [&rng]() { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    // a valid frame the way the box builds it, key length q = 3..10 (size = 83 + q)
    auto valid = [&](uint8_t* out, int q) -> size_t {
      const size_t n = 83 + q;
      out[0] = 0x55; out[1] = 0xAA; out[2] = 0x5A; out[3] = (uint8_t)(n - 5); out[4] = 0; out[6] = 0;
      for (int i = 0; i < q; i++) out[8 + i] = (uint8_t)rnd();
      out[7] = out[8 + q - 1];
      for (int i = 0; i < 75; i++) {
        const uint8_t pt = i == 0 ? 0x06 : (i == 1 ? 0x04 : (i == 2 ? 0x06 : (uint8_t)rnd()));
        out[8 + q + i] = pt ^ out[8 + (i % q)];
      }
      unsigned sum = 167;
      for (size_t i = 0; i < n; i++) if (i != 3 && i != 5) sum += out[i];
      out[5] = (uint8_t)sum;
      return n;
    };
    int validOk = 0, mutated = 0, randomOk = 0, inconsistent = 0, total = 0;
    for (int q = 3; q <= 10; q++) {                            // valid frames of every key length decode, in an exactly sized buffer
      uint8_t tmp[100];
      const size_t n = valid(tmp, q);
      uint8_t* fr = (uint8_t*)malloc(n);
      memcpy(fr, tmp, n);
      uint8_t z[kZones][3];
      FrameInfo info;
      if (classifyFrame(fr, n, z, &info) == kFrameOk && info.checksumOk && frameChecksumOk(fr, n) && decodeFrame(fr, n, z)) validOk++;
      free(fr);
    }
    CHECK(validOk == 8, "frames built with every key length 3..10 classify as readable with a good checksum (%d of 8)", validOk);
    for (int it = 0; it < 30000; it++) {
      uint8_t tmp[300];
      size_t n;
      const unsigned mode = rnd() % 4;
      if (mode == 0) {                                         // pure noise of any length
        n = rnd() % 260;
        for (size_t i = 0; i < n; i++) tmp[i] = (uint8_t)rnd();
      } else if (mode == 1) {                                  // the right framing, noise inside
        n = 12 + rnd() % 120;
        for (size_t i = 0; i < n; i++) tmp[i] = (uint8_t)rnd();
        tmp[0] = 0x55; tmp[1] = 0xAA; tmp[2] = 0x5A; tmp[3] = (uint8_t)(n - 5);
      } else {                                                 // a valid frame with a few bytes changed, cut short or stretched
        n = valid(tmp, 3 + (int)(rnd() % 8));
        const int flips = 1 + (int)(rnd() % 4);
        for (int k = 0; k < flips; k++) tmp[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8));
        if (mode == 3) {
          const int d = (int)(rnd() % 13) - 6;                 // 6 shorter .. 6 longer
          if (d < 0 && (size_t)(-d) < n) n -= (size_t)(-d);
          else if (d > 0) { for (int k = 0; k < d; k++) tmp[n + (size_t)k] = (uint8_t)rnd(); n += (size_t)d; }
        }
        mutated++;
      }
      uint8_t* fr = (uint8_t*)malloc(n ? n : 1);
      if (n) memcpy(fr, tmp, n);
      uint8_t z1[kZones][3], z2[kZones][3];
      memset(z1, 0, sizeof z1); memset(z2, 0, sizeof z2);
      FrameInfo info;
      const FrameClass c = classifyFrame(fr, n, z1, &info);
      const bool d = decodeFrame(fr, n, z2);
      (void)frameChecksumOk(fr, n);
      if ((c == kFrameOk) != d) inconsistent++;                // the two must agree about what is readable
      if (c == kFrameOk && memcmp(z1, z2, sizeof z1) != 0) inconsistent++;
      if (c == kFrameOk) randomOk++;
      free(fr);
      total++;
    }
    CHECK(inconsistent == 0, "classifyFrame and decodeFrame always agree (%d disagreements in %d frames)", inconsistent, total);
    CHECK(randomOk < total / 4, "noise and damaged frames are not mostly accepted (%d readable of %d, %d of them mutated valid frames)", randomOk, total, mutated);
    { uint8_t one = 0; uint8_t z[kZones][3]; FrameInfo info;                                      // no data at all
      CHECK(classifyFrame(&one, 0, z, &info) == kFrameUnreadable && !decodeFrame(&one, 0, z), "an empty datagram is unreadable"); }
    // host names typed into the setup page: any text, any buffer size
    int hostBad = 0;
    for (int it = 0; it < 20000; it++) {
      const size_t n = rnd() % 90;
      char* in = (char*)malloc(n + 1);
      static const char alphabet[] = "abcXYZ019 .-_:/?#@%\t\xC3\xA9 http://https://";
      for (size_t i = 0; i < n; i++) in[i] = (rnd() % 3 == 0) ? (char)rnd() : alphabet[rnd() % (sizeof alphabet - 1)];
      in[n] = 0;
      const int cap = 1 + (int)(rnd() % 70);
      char* out = (char*)malloc((size_t)cap);
      const int r = normalizeHost(in, out, cap);
      if (r >= cap) hostBad++;
      else if (r >= 0 && (out[r] != 0 || (int)strlen(out) != r)) hostBad++;
      free(out); free(in);
    }
    CHECK(hostBad == 0, "a host name is cut to its buffer and terminated, whatever was typed (%d problems)", hostBad);
    CHECK(normalizeHost(nullptr, (char*)&inconsistent, 4) == -1, "no input text: refused");
    // firmware images: the header check must cope with any file
    int imgBad = 0;
    for (int it = 0; it < 20000; it++) {
      const size_t n = rnd() % 200;
      uint8_t* h = (uint8_t*)malloc(n ? n : 1);
      for (size_t i = 0; i < n; i++) h[i] = (uint8_t)rnd();
      if (n > 40 && rnd() % 2) { h[0] = 0xE9; h[12] = kChipEsp32S3; h[13] = 0; h[32] = 0x32; h[33] = 0x54; h[34] = 0xCD; h[35] = 0xAB; }   // close to a real header
      const ImageCheck r = checkAppImage(h, n, kChipEsp32S3);
      if ((int)r < 0 || (int)r > (int)kImageNoDescriptor) imgBad++;
      if (n < kImageHeadBytes && r == kImageOk) imgBad++;      // too short to be checked: never accepted
      free(h);
    }
    CHECK(imgBad == 0, "any file is judged by its header without reading past it (%d problems)", imgBad);
    // the update marker is found wherever it sits and however the upload is cut into chunks
    int markBad = 0;
    for (int it = 0; it < 3000; it++) {
      const size_t mlen = strlen(kImageMark), n = mlen + rnd() % 400, at = rnd() % (n - mlen + 1);
      uint8_t* data = (uint8_t*)malloc(n);
      for (size_t i = 0; i < n; i++) data[i] = (uint8_t)rnd();
      memcpy(data + at, kImageMark, mlen);
      MarkScanner s;
      size_t pos = 0;
      while (pos < n) { size_t c = 1 + rnd() % 70; if (c > n - pos) c = n - pos; markScan(s, data + pos, c); pos += c; }
      if (!s.found) markBad++;
      MarkScanner none;
      uint8_t* junk = (uint8_t*)malloc(n);
      for (size_t i = 0; i < n; i++) junk[i] = (uint8_t)rnd();
      markScan(none, junk, n);
      if (none.found) markBad++;                               // random bytes do not contain the 23 byte marker
      free(junk); free(data);
    }
    // addresses typed or imported: the strict parser never reads past its text
    int ipBad = 0;
    for (int it = 0; it < 20000; it++) {
      const size_t n = rnd() % 24;
      char* s = (char*)malloc(n + 1);
      static const char alphabet[] = "0123456789....  -:abc/";
      for (size_t i = 0; i < n; i++) s[i] = alphabet[rnd() % (sizeof alphabet - 1)];
      s[n] = 0;
      uint8_t b[4] = {9, 9, 9, 9};
      const bool ok = parseIpv4(s, b);
      int dots = 0;
      for (size_t i = 0; i < n; i++) if (s[i] == '.') dots++;
      if (ok && (dots != 3 || n < 7 || n > 15)) ipBad++;           // four numbers need three dots and 7 to 15 characters
      if (!ok && (b[0] != 9 || b[3] != 9)) ipBad++;                // nothing is written for a refused address
      free(s);
    }
    CHECK(ipBad == 0, "random text is never accepted as an address by mistake, and nothing is written for it (%d problems)", ipBad);
    CHECK(markBad == 0, "the update marker is always found, in any chunking, and never in noise (%d problems)", markBad);
  }

  printf("decode %d, classify %d, usage %d, uv %d, linear %d, loop %d, apply %d, ddp %d, govee %d, sharp %d, vivid %d, motion %d, blur %d vectors; %d checks passed, %d failed\n",
         nDecode, nClassify, nUsage, nUv, nLin, nLoop, nApply, nDdp, nGovee, nSharp, nVivid, nMotion, nBlur, g_pass, g_fail);
  printf(g_fail ? "SOME FAILED\n" : "ALL PASSED\n");
  return g_fail ? 1 : 0;
}
