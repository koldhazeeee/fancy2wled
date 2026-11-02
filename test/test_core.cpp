// Host-side test of the firmware core against golden vectors from the Python reference.
//   build + run:  python test/run_core_tests.py
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  int nDecode = 0, nUv = 0, nLin = 0, nLoop = 0, nDdp = 0, nApply = 0;
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
    } else if (k == "APPLY") {
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
      applyTable(tbl.data(), count, zones, out.data(), si);
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
    } else if (k == "DDP") {
      int seq, dt, ln, push; unsigned off2; char hex[64];
      sscanf(rest, "%d %d %u %d %d %63s", &seq, &dt, &off2, &ln, &push, hex);
      uint8_t h[10];
      ddpHeader(h, (uint8_t)seq, (uint8_t)dt, off2, (uint16_t)ln, push != 0);
      auto want = fromHex(hex);
      CHECK(want.size() == 10 && memcmp(h, want.data(), 10) == 0, "ddp header mismatch (seq %d)", seq);
      nDdp++;
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
    const float avgL = (z[0][0] + z[1][0] + z[2][0]) / 3.f, avgRt = (z[3][0] + z[4][0] + z[5][0]) / 3.f;   // halves of the top row
    const SmoothInfo si = smoothInfo(t, 210, 100);
    float out[210 * 3]; applyTable(t, 210, z, out, si);
    CHECK(si.firstZero > 0 && si.lastZero < 209 && si.lenStart > si.lenEnd && si.lenEnd >= kMinOutQ, "each tail gets its own length (%d, %d)", si.lenStart, si.lenEnd);
    CHECK(fabsf(out[91 * 3] - 255.f) < 1.f, "over the TV the edge zone colour is exact (%.1f)", out[91 * 3]);
    CHECK(fabsf(out[0] - avgL) < 2.f, "the far end of the strip reaches the average of its half of the top row (%.1f vs %.1f)", out[0], avgL);
    CHECK(fabsf(out[209 * 3] - avgRt) < 2.f, "the SHORTER tail also reaches its target at its own end (%.1f vs %.1f)", out[209 * 3], avgRt);
    CHECK(out[50 * 3] < out[91 * 3] && out[50 * 3] > out[0] - 1.f, "beyond the TV the colour eases smoothly from the edge to the average");
    float prev = out[91 * 3]; bool mono = true;
    for (int i = 90; i >= 0; i--) { if (out[i * 3] > prev + 0.01f) mono = false; prev = out[i * 3]; }
    CHECK(mono, "the ease is monotonic (no bumps)");
    applyTable(t, 210, z, out, smoothInfo(t, 210, 0));
    CHECK(fabsf(out[0] - 255.f) < 1.f, "smoothing off: the strip beyond the TV repeats the edge colour"); }
  { // a strip on the ceiling (top edge) must only ever see the top row: a green dot in the bottom-left corner of the picture
    // (zone 18) must not tint the LEDs beyond the TV; they ease toward the average of the TOP ROW
    Entry t[210]; tableLinear(t, 210, false, 82, 172, 0);
    const SmoothInfo si = smoothInfo(t, 210, 100);
    CHECK(si.maskStart == 0x07u && si.maskEnd == 0x38u, "a top strip: left tail -> left half of the top row (zones 0-2), right tail -> right half (3-5): %x %x", si.maskStart, si.maskEnd);
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
    for (int i = 0; i < kZones; i++) { z[i][0] = z[i][1] = z[i][2] = (i < 3) ? 100.f : (i < 6 ? 40.f : 255.f); }   // top row 100|40, rest 255
    applyTable(t, 210, z, out, si);
    CHECK(fabsf(out[0] - 100.f) < 1.f && fabsf(out[209 * 3] - 40.f) < 1.f,
          "each far end reaches the average of its own half of the top row (%.1f, %.1f)", out[0], out[209 * 3]);
    Entry b[210]; tableLinear(b, 210, false, 82, 172, 2);                         // strip under the TV: bottom row
    const SmoothInfo sb = smoothInfo(b, 210, 100);
    CHECK(sb.maskStart == (7u << 18) && sb.maskEnd == (7u << 21), "a bottom strip uses halves of the bottom row (zones 18-20 / 21-23): %x %x", sb.maskStart, sb.maskEnd);
    Entry r2[210]; tableLinear(r2, 210, true, 82, 172, 0);                        // LED 0 at the right end: the tails swap sides
    const SmoothInfo sr = smoothInfo(r2, 210, 100);
    CHECK(sr.maskStart == 0x38u && sr.maskEnd == 0x07u, "LED 0 on the right: first tail uses the right half (%x %x)", sr.maskStart, sr.maskEnd);
    Entry l[180]; tableLinear(l, 180, false, 50, 130, 3);
    const SmoothInfo sl = smoothInfo(l, 180, 100);
    CHECK(sl.maskStart == ((1u << 0) | (1u << 6)) && sl.maskEnd == ((1u << 12) | (1u << 18)), "a left strip uses halves of the left column: %x %x", sl.maskStart, sl.maskEnd); }
  { Entry t[10]; int bad[4] = {1, 1, 1, 1};
    CHECK(!tableLoop(t, 10, 0, false, bad, 0, 16, 9), "loop rejects sides that do not sum to count");
    CHECK(!tableLoop(t, 10, 7, false, nullptr, 0, 16, 9), "loop rejects bad corner"); }

  printf("decode %d, uv %d, linear %d, loop %d, apply %d, ddp %d vectors; %d checks passed, %d failed\n",
         nDecode, nUv, nLin, nLoop, nApply, nDdp, g_pass, g_fail);
  printf(g_fail ? "SOME FAILED\n" : "ALL PASSED\n");
  return g_fail ? 1 : 0;
}
