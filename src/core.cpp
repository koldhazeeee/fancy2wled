#include "core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// This file is the colour maths (zone table lookup, blur, saturation, motion smoothing): build it for speed, the rest of the firmware for size (the default -Os).
// Measured on the AtomS3: the table lookup 37 % faster (239 -> 150 microseconds for 266 LEDs), smoothing and packing about 40-50 %, for about 5 KB of flash.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O3")
#endif

namespace fwb {

// ---------------------------------------------------------------------------
ImageCheck checkAppImage(const uint8_t* h, size_t len, uint16_t chip) {
  if (!h || len < kImageHeadBytes) return kImageTooShort;
  if (h[0] != 0xE9) return kImageBadMagic;
  if ((uint16_t)(h[12] | (h[13] << 8)) != chip) return kImageWrongChip;
  const uint32_t desc = (uint32_t)h[32] | ((uint32_t)h[33] << 8) | ((uint32_t)h[34] << 16) | ((uint32_t)h[35] << 24);
  if (desc != 0xABCD5432u) return kImageNoDescriptor;
  return kImageOk;
}

const char* imageCheckMessage(ImageCheck c) {
  switch (c) {
    case kImageOk: return "";
    case kImageTooShort: return "That file is too small to be a firmware image.";
    case kImageBadMagic: return "That is not a firmware image (use firmware.bin from .pio/build/atoms3, not a merged or bootloader file).";
    case kImageWrongChip: return "That firmware is for a different chip (this board has an ESP32-S3).";
    case kImageNoDescriptor: return "That is not an application image (use firmware.bin, not the bootloader or the partition table).";
  }
  return "That file cannot be used.";
}

const char kImageMark[] = "FANCY2WLED-IMAGE-MARK-1";

void markScan(MarkScanner& s, const uint8_t* d, size_t n) {
  // The mark starts with a character that occurs nowhere else in it, so a failed match only has to retry the same byte as a new start.
  const size_t len = sizeof kImageMark - 1;
  for (size_t i = 0; i < n && !s.found; i++) {
    if (d[i] == (uint8_t)kImageMark[s.matched]) {
      if (++s.matched == len) s.found = true;
    } else {
      s.matched = (d[i] == (uint8_t)kImageMark[0]) ? 1 : 0;
    }
  }
}

bool parseIpv4(const char* s, uint8_t out[4]) {
  if (!s) return false;
  uint8_t b[4];
  const char* p = s;
  for (int part = 0; part < 4; part++) {
    if (*p < '0' || *p > '9') return false;
    if (*p == '0' && p[1] >= '0' && p[1] <= '9') return false;       // no leading zeros
    int v = 0, digits = 0;
    while (*p >= '0' && *p <= '9') {
      v = v * 10 + (*p - '0');
      if (++digits > 3 || v > 255) return false;
      p++;
    }
    b[part] = (uint8_t)v;
    if (part < 3) {
      if (*p != '.') return false;
      p++;
    }
  }
  if (*p != 0) return false;
  if (out) memcpy(out, b, 4);
  return true;
}

bool validSenderAddress(const char* s) {
  uint8_t b[4];
  if (!parseIpv4(s, b)) return false;
  return b[0] != 0 && b[0] != 127 && b[0] < 224;
}

const char* resetReasonName(int code) {
  switch (code) {                                     // ESP-IDF's esp_reset_reason_t
    case 1: return "power on";
    case 2: return "reset button";
    case 3: return "software restart";
    case 4: return "crash (panic)";
    case 5: return "watchdog (interrupt)";
    case 6: return "watchdog (a task stopped responding)";
    case 7: return "watchdog";
    case 8: return "deep sleep";
    case 9: return "brownout (the power supply dipped)";
    case 10: return "SDIO";
    default: return "unknown";
  }
}

bool resetReasonUnexpected(int code) { return code == 4 || code == 5 || code == 6 || code == 7 || code == 9; }

bool validUpdatePassword(const char* pw) {
  if (!pw) return false;
  size_t n = 0;
  for (; pw[n]; n++)
    if ((unsigned char)pw[n] < 0x20 || (unsigned char)pw[n] > 0x7E) return false;
  return n >= 8 && n <= 64;
}

bool secureEquals(const char* a, const char* b) {
  if (!a || !b) return false;
  size_t la = 0, lb = 0;
  while (a[la]) la++;
  while (b[lb]) lb++;
  unsigned diff = (unsigned)(la ^ lb);
  const size_t n = la < lb ? la : lb;
  for (size_t i = 0; i < n; i++) diff |= (unsigned)((unsigned char)a[i] ^ (unsigned char)b[i]);
  return diff == 0;
}

// ---------------------------------------------------------------------------
int normalizeHost(const char* in, char* out, int cap) {
  if (!in || !out || cap < 1) return -1;
  auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  auto lower = [](char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; };
  auto starts = [&](const char* s, const char* p) {
    for (; *p; s++, p++) if (lower(*s) != *p) return false;
    return true;
  };
  while (space(*in)) in++;
  if (starts(in, "http://")) in += 7;
  else if (starts(in, "https://")) in += 8;
  int n = 0;
  for (; *in && *in != '/' && *in != '?' && *in != '#'; in++) {
    if (n + 1 >= cap) return -1;
    out[n++] = lower(*in);
  }
  while (n > 0 && (space(out[n - 1]) || out[n - 1] == '.')) n--;
  if (n >= 3 && out[n - 3] == ':' && out[n - 2] == '8' && out[n - 1] == '0') n -= 3;     // the board always talks to port 80
  while (n > 0 && (space(out[n - 1]) || out[n - 1] == '.')) n--;
  out[n] = 0;
  return n;
}

// ---------------------------------------------------------------------------
size_t base64Encode(const uint8_t* in, size_t n, char* out, size_t cap) {
  static const char* kB64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const size_t need = ((n + 2) / 3) * 4;
  if (!out || cap < need + 1) return 0;
  size_t o = 0;
  for (size_t i = 0; i < n; i += 3) {
    const uint32_t v = ((uint32_t)in[i] << 16) | ((i + 1 < n ? (uint32_t)in[i + 1] : 0u) << 8) | (i + 2 < n ? (uint32_t)in[i + 2] : 0u);
    out[o++] = kB64[(v >> 18) & 63];
    out[o++] = kB64[(v >> 12) & 63];
    out[o++] = i + 1 < n ? kB64[(v >> 6) & 63] : '=';
    out[o++] = i + 2 < n ? kB64[v & 63] : '=';
  }
  out[o] = 0;
  return o;
}

// {"msg":{"cmd":"razer","data":{"pt":"<base64>"}}}
static size_t goveeWrap(const uint8_t* packet, size_t n, char* out, size_t cap) {
  static const char kHead[] = "{\"msg\":{\"cmd\":\"razer\",\"data\":{\"pt\":\"";
  static const char kTail[] = "\"}}}";
  const size_t h = sizeof kHead - 1, t = sizeof kTail - 1;
  if (!out || cap < h + t + 1) return 0;
  const size_t b = base64Encode(packet, n, out + h, cap - h - t);
  if (b == 0) return 0;
  for (size_t i = 0; i < t; i++) out[h + b + i] = kTail[i];
  out[h + b + t] = 0;
  for (size_t i = 0; i < h; i++) out[i] = kHead[i];
  return h + b + t;
}

size_t goveeEnableJson(bool on, char* out, size_t cap) {
  uint8_t p[6] = {0xBB, 0x00, 0x01, 0xB1, (uint8_t)(on ? 1 : 0), 0};
  for (int i = 0; i < 5; i++) p[5] ^= p[i];
  return goveeWrap(p, sizeof p, out, cap);
}

size_t goveeFrameJson(const uint8_t* rgb, int n, char* out, size_t cap) {
  if (!rgb || n < 1 || n > kGoveeMaxSegments) return 0;
  uint8_t p[7 + 3 * kGoveeMaxSegments];
  const int len = 2 + 3 * n;
  p[0] = 0xBB; p[1] = (uint8_t)(len >> 8); p[2] = (uint8_t)(len & 255); p[3] = 0xB0; p[4] = 0x00; p[5] = (uint8_t)n;
  for (int i = 0; i < 3 * n; i++) p[6 + i] = rgb[i];
  const size_t total = (size_t)(6 + 3 * n);
  uint8_t x = 0;
  for (size_t i = 0; i < total; i++) x ^= p[i];
  p[total] = x;
  return goveeWrap(p, total + 1, out, cap);
}

// ---------------------------------------------------------------------------
bool isBlackPicture(const uint8_t zones[kZones][3]) {
  for (int z = 0; z < kZones; z++)
    if (zones[z][0] | zones[z][1] | zones[z][2]) return false;
  return true;
}

void blackFrame(BlackTracker& t, const uint8_t zones[kZones][3], uint32_t nowMs) {
  if (!isBlackPicture(zones)) { t.black = false; return; }                          // any colour at all: the TV is on
  if (!t.black) { t.black = true; t.since = nowMs; }
}

void blackReset(BlackTracker& t) { t.black = false; }

uint32_t blackForMs(const BlackTracker& t, uint32_t nowMs) { return t.black ? nowMs - t.since : 0; }

bool tvIsOff(const BlackTracker& t, uint32_t nowMs, uint32_t delayMs) { return t.black && nowMs - t.since >= delayMs; }

// ---------------------------------------------------------------------------
bool decodeFrame(const uint8_t* d, size_t len, uint8_t zones[kZones][3]) {
  if (len < 86 || len > 93) return false;
  if (d[0] != 0x55 || d[1] != 0xAA || d[2] != 0x5A || d[3] != len - 5) return false;
  const size_t q = len - 83;  // key length 3..10
  const uint8_t* key = d + 8;
  const uint8_t* body = d + 8 + q;
  const size_t n = len - 8 - q;  // 75
  if (n != 75) return false;
  uint8_t pt[75];
  size_t ki = 0;
  for (size_t i = 0; i < n; i++) {
    pt[i] = body[i] ^ key[ki];
    if (++ki == q) ki = 0;                                   // (key[i % q] without a division per byte)
  }
  if (pt[0] != 0x06 || pt[1] != 0x04 || pt[2] != 0x06) return false;  // built-in integrity check
  memcpy(zones, pt + 3, (size_t)kZones * 3);
  return true;
}

bool frameChecksumOk(const uint8_t* d, size_t len) {
  if (len < 8) return false;
  uint32_t sum = 167;
  for (size_t i = 0; i < len; i++)
    if (i != 3 && i != 5) sum += d[i];
  return (uint8_t)(sum & 255) == d[5];
}

FrameClass classifyFrame(const uint8_t* d, size_t len, uint8_t zones[kZones][3], FrameInfo* info) {
  FrameInfo local;
  FrameInfo& fi = info ? *info : local;
  fi = FrameInfo();
  if (decodeFrame(d, len, zones)) {
    fi.checksumOk = frameChecksumOk(d, len);
    return kFrameOk;
  }
  if (len >= 12 && len <= 260 && d[0] == 0x55 && d[1] == 0xAA && d[2] == 0x5A && d[3] == len - 5) {
    fi.checksumOk = frameChecksumOk(d, len);
    if (len >= 86 && len <= 93) {                       // the key length is known, so the first payload bytes can be shown
      const size_t q = len - 83;
      for (int i = 0; i < 3; i++) fi.head[i] = d[8 + q + i] ^ d[8 + (i % q)];
      fi.headKnown = true;
    }
    return kFrameNewLayout;
  }
  return kFrameUnreadable;
}

// ---------------------------------------------------------------------------
// Border zones in screen order along each edge (corners appear on two edges).
static const uint8_t kTop[6] = {0, 1, 2, 3, 4, 5};        // left -> right
static const uint8_t kRight[4] = {5, 11, 17, 23};         // top -> bottom
static const uint8_t kBottom[6] = {18, 19, 20, 21, 22, 23};  // left -> right
static const uint8_t kLeft[4] = {0, 6, 12, 18};           // top -> bottom

enum Edge { TOP = 0, RIGHT = 1, BOTTOM = 2, LEFT = 3 };

static const uint8_t* edgeZones(Edge e, int* k) {
  switch (e) {
    case TOP: *k = 6; return kTop;
    case RIGHT: *k = 4; return kRight;
    case BOTTOM: *k = 6; return kBottom;
    default: *k = 4; return kLeft;
  }
}

static inline int roundHalfUp(float x) { return (int)floorf(x + 0.5f); }

// Where along its edge each zone looks, as a fraction of the picture width (top, bottom) or height (left, right): the middle of the window the box averages
// for that zone. The Sync Box does not cut the picture into an even 6 x 4 grid: measured with tools/sync_probe on 2026-11-04 (a white rectangle growing across a known picture): each zone is an exact average over a window and the windows do not tile the picture evenly. Across, six
// windows 300 px wide of 1920 (x 0-300, 300-600, 600-900, 960-1260, 1260-1560, 1560-1860); down, the top and bottom zones average a thin band at the screen
// edge (60 px of 1080), the middle rows y 240-480 and 540-780, and the left / right zones a 60 px strip. See docs/PROTOCOL.md.
static const float kPosAcross[6] = {0.0797f, 0.2359f, 0.3922f, 0.5797f, 0.7359f, 0.8922f};   // top and bottom edge, left -> right
static const float kPosDown[4] = {0.0287f, 0.3356f, 0.6134f, 0.9736f};                        // left and right edge, top -> bottom

// A LED at t along its edge blends the two zones either side of it (before the first / after the last zone it holds that zone). d is filled in by the caller.
static Entry interp(Edge e, float t) {
  int k;
  const uint8_t* z = edgeZones(e, &k);
  const float* p = (e == TOP || e == BOTTOM) ? kPosAcross : kPosDown;
  if (t <= p[0]) return Entry{z[0], z[1], 0, 0};
  if (t >= p[k - 1]) return Entry{z[k - 1], z[k - 1], 0, 0};
  int j0 = 0;
  while (j0 + 2 < k && t >= p[j0 + 1]) j0++;
  int f = roundHalfUp((t - p[j0]) / (p[j0 + 1] - p[j0]) * 255.f);
  if (f < 0) f = 0;
  if (f > 255) f = 255;
  return Entry{z[j0], z[j0 + 1], (uint8_t)f, 0};
}

static inline float clamp01(float x) { return x < 0.f ? 0.f : (x > 1.f ? 1.f : x); }
static inline float maxf(float a, float b) { return a > b ? a : b; }

static uint8_t distanceByte(float over) {
  if (over > kOutMax) over = kOutMax;
  int d = roundHalfUp(over / kOutMax * 255.f);
  return (uint8_t)(d < 0 ? 0 : (d > 255 ? 255 : d));
}

Entry uvToEntry(float u, float v, float aspect, int edgeHint) {
  const float cu = clamp01(u), cv = clamp01(v);
  Edge e;
  if (edgeHint >= 0 && edgeHint <= 3) {
    e = (Edge)edgeHint;
  } else if (u > 0.f && u < 1.f && v > 0.f && v < 1.f) {  // inside the picture: nearest edge
    const float dTop = v, dBottom = 1.f - v, dLeft = u, dRight = 1.f - u;
    e = TOP;
    float best = dTop;
    if (dBottom < best) { best = dBottom; e = BOTTOM; }
    if (dLeft < best) { best = dLeft; e = LEFT; }
    if (dRight < best) { best = dRight; e = RIGHT; }
  } else if (cv <= 0.f) {
    e = TOP;
  } else if (cv >= 1.f) {
    e = BOTTOM;
  } else if (cu <= 0.f) {
    e = LEFT;
  } else {
    e = RIGHT;
  }
  const bool horizontal = (e == TOP || e == BOTTOM);
  Entry r = interp(e, horizontal ? cu : cv);
  // how far past the end of the picture's edge (in picture widths) -- the perpendicular offset does not count
  const float over = horizontal ? maxf(0.f, maxf(-u, u - 1.f)) : maxf(0.f, maxf(-v, v - 1.f)) * aspect;
  r.d = distanceByte(maxf(0.f, over - kDeadZone));
  return r;
}

void tableLinear(Entry* out, int count, bool led0Right, int spanFirst, int spanLast, int edge) {
  int first = 0, last = count - 1;
  if (spanFirst >= 0) { first = spanFirst; last = spanLast; }
  int width = last - first + 1;
  if (width < 1) width = 1;
  if (edge < 0 || edge > 3) edge = 0;
  for (int i = 0; i < count; i++) {
    float t = (i - first + 0.5f) / width;
    if (led0Right) t = 1.f - t;
    float u, v;
    switch (edge) {
      case BOTTOM: u = t; v = 1.f; break;
      case LEFT: u = 0.f; v = t; break;
      case RIGHT: u = 1.f; v = t; break;
      default: u = t; v = 0.f; break;
    }
    out[i] = uvToEntry(u, v, kDefaultAspect, edge);
  }
}
static void autoSides(int count, int aw, int ah, int s[4]) {
  const float tot = 2.f * (aw + ah);
  s[0] = roundHalfUp(count * aw / tot);
  s[1] = roundHalfUp(count * ah / tot);
  s[2] = s[0];
  s[3] = count - 2 * s[0] - s[1];
}

// Entry for the idx-th LED along the clockwise perimeter (from the top-left corner).
static Entry perimeterEntry(int idx, const int s[4]) {
  int edge = 0;
  while (edge < 3 && idx >= s[edge]) { idx -= s[edge]; edge++; }
  float t = (idx + 0.5f) / s[edge];
  if (edge == BOTTOM || edge == LEFT) t = 1.f - t;  // walked right->left / bottom->top
  return interp((Edge)edge, t);
}

bool tableLoop(Entry* out, int count, int startCorner, bool ccw, const int* sides, int shift, int aw, int ah) {
  int s[4];
  if (sides) {
    if (sides[0] + sides[1] + sides[2] + sides[3] != count) return false;
    for (int i = 0; i < 4; i++) s[i] = sides[i];
  } else {
    autoSides(count, aw, ah, s);
  }
  for (int i = 0; i < 4; i++)
    if (s[i] <= 0) return false;
  int offsets[4] = {0, s[0], s[0] + s[1], s[0] + s[1] + s[2]};
  if (startCorner < 0 || startCorner > 3) return false;
  const int off = offsets[startCorner] + shift;
  for (int i = 0; i < count; i++) {
    int idx = ccw ? (off - 1 - i) : (off + i);
    idx %= count;
    if (idx < 0) idx += count;
    out[i] = perimeterEntry(idx, s);
  }
  return true;
}

static int tailLength(const Entry* t, int lo, int hi, int percent) {
  int dmax = 0;
  for (int i = lo; i < hi; i++)
    if (t[i].d > dmax) dmax = t[i].d;
  if (percent <= 0 || dmax == 0) return 0;
  int len = roundHalfUp(dmax * (float)percent / 100.f);
  return len < kMinOutQ ? kMinOutQ : len;
}

// The zone a tail eases toward: the zone the strip's outermost LED shows (the picture's corner zone when the strip runs along
// an edge). Only that zone, so colours anywhere else on screen never reach the LEDs beyond the TV.
static uint32_t tailTarget(const Entry& outer) {
  return 1u << (outer.f < 128 ? outer.a : outer.b);
}

SmoothInfo smoothInfo(const Entry* t, int count, int percent) {
  SmoothInfo s{count, -1, 0, 0, 0u, 0u};
  for (int i = 0; i < count; i++)
    if (t[i].d == 0) { if (s.firstZero == count) s.firstZero = i; s.lastZero = i; }
  s.lenStart = tailLength(t, 0, s.firstZero, percent);
  s.lenEnd = tailLength(t, s.lastZero + 1, count, percent);
  if (s.firstZero > 0 && s.firstZero < count) s.maskStart = tailTarget(t[0]);
  if (s.lastZero >= 0 && s.lastZero < count - 1) s.maskEnd = tailTarget(t[count - 1]);
  return s;
}
// f / 255 for every f (the same division, done by the compiler), so applyTable reads a table instead of dividing for every LED. (Built with a template, not a loop,
// because the host tests are compiled as C++11.)
template <int... I> struct IntSeq {};
template <int N, int... I> struct MakeSeq : MakeSeq<N - 1, N - 1, I...> {};
template <int... I> struct MakeSeq<0, I...> { typedef IntSeq<I...> type; };
struct Weights255 { float v[256]; };
template <int... I> constexpr Weights255 makeWeights255(IntSeq<I...>) { return Weights255{{((float)I / 255.f)...}}; }
static constexpr Weights255 kWeights255 = makeWeights255(MakeSeq<256>::type());

static inline float smoothstep01(float x) {
  x = x < 0.f ? 0.f : (x > 1.f ? 1.f : x);
  return x * x * (3.f - 2.f * x);
}

// Average colour of the zones in `mask` (bit z = zone z); an empty mask means all 24 zones.
static void averageOf(const float zones[kZones][3], uint32_t mask, float avg[3]) {
  avg[0] = avg[1] = avg[2] = 0.f;
  int n = 0;
  for (int z = 0; z < kZones; z++) {
    if (mask && !(mask & (1u << z))) continue;
    for (int c = 0; c < 3; c++) avg[c] += zones[z][c];
    n++;
  }
  for (int c = 0; c < 3; c++) avg[c] /= n;
}

// ---- vivid blending: through the colour wheel (see core.h; tools/reference/layout.py blend_vivid is the independent version)
static void toHsv(const float c[3], float* h, float* s, float* v) {
  const float r = c[0], g = c[1], b = c[2];
  const float mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b), d = mx - mn;     // (fmaxf / fminf are library calls on the ESP32)
  *v = mx / 255.f;
  *s = mx > 0.f ? d / mx : 0.f;
  if (d <= 0.f) { *h = 0.f; return; }
  float hh;
  if (mx == r) { hh = (g - b) / d; if (hh < 0.f) hh += 6.f; hh *= 60.f; }       // (the value is in [-1, 1]: one conditional step is the same as the floorf wrap)
  else if (mx == g) hh = 60.f * ((b - r) / d + 2.f);
  else hh = 60.f * ((r - g) / d + 4.f);
  *h = hh;
}
static void fromHsv(float h, float s, float v, float out[3]) {
  if (h >= 360.f || h < 0.f) {                       // a blend of two hues lands in (-180, 540): one step wraps it; anything else falls back to the general wrap
    if (h >= 360.f && h < 720.f) h -= 360.f;
    else if (h < 0.f && h >= -360.f) h += 360.f;
    else h -= 360.f * floorf(h / 360.f);
  }
  float hs = h / 60.f;
  int sec = (int)hs;                                 // 0..5 (6 only for h == 360, which is the same as 0)
  if (sec > 5) { sec = 0; hs = 0.f; }
  const float c = v * s, m = v - c;
  const float x = c * (1.f - fabsf((hs - (float)(sec & ~1)) - 1.f));    // the same as c * (1 - |fmod(hs, 2) - 1|)
  float r, g, b;
  switch (sec) {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
  }
  out[0] = (r + m) * 255.f; out[1] = (g + m) * 255.f; out[2] = (b + m) * 255.f;
}
struct Hsv { float h, s, v; };
// The blend of two colours given as hue / saturation / value (the colours converted once, see applyTable: a table has hundreds of LEDs but only 24 zones).
static void vividBlendHsv(const Hsv& a, const Hsv& b, float w, float out[3]) {
  float ha = a.h, sa = a.s, va = a.v, hb = b.h, sb = b.s, vb = b.v;
  if (va < 0.02f && vb >= 0.02f) sa = sb;                                           // black has no saturation of its own either: green from black is a darker green, not a paler one
  else if (vb < 0.02f && va >= 0.02f) sb = sa;
  const bool da = sa >= 0.04f && va >= 0.02f, db = sb >= 0.04f && vb >= 0.02f;     // a grey or a black has no hue worth keeping
  float h;
  if (da && db) {
    float dh = hb - ha;
    if (dh > 180.f) dh -= 360.f;
    else if (dh <= -180.f) dh += 360.f;
    h = ha + dh * w;
  } else if (da) h = ha;
  else if (db) h = hb;
  else h = 0.f;
  fromHsv(h, sa + (sb - sa) * w, va + (vb - va) * w, out);
}
void vividBlend(const float a[3], const float b[3], float w, float out[3]) {
  Hsv A, B;
  toHsv(a, &A.h, &A.s, &A.v);
  toHsv(b, &B.h, &B.s, &B.v);
  vividBlendHsv(A, B, w, out);
}

// ---------------------------------------------------------------------------
namespace {
// One LED's colour from the table entry: the blend of its two zones (or, vivid, through the colour wheel).
template <bool kVivid>
inline void ledColour(const Entry& e, const float zones[kZones][3], const Weights255& kW, Hsv* zh, bool* zhHave, float* o) {
  const float* za = zones[e.a];
  const float* zb = zones[e.b];
  if (kVivid && e.a != e.b && e.f != 0 && e.f != 255) {
    const int ia = e.a, ib = e.b;
    if (!zhHave[ia]) { toHsv(za, &zh[ia].h, &zh[ia].s, &zh[ia].v); zhHave[ia] = true; }
    if (!zhHave[ib]) { toHsv(zb, &zh[ib].h, &zh[ib].s, &zh[ib].v); zhHave[ib] = true; }
    vividBlendHsv(zh[ia], zh[ib], kW.v[e.f], o);
  } else if (e.f == 0 || e.a == e.b) {                  // (the blend would add exactly nothing)
    o[0] = za[0]; o[1] = za[1]; o[2] = za[2];
  } else {
    const float w = kW.v[e.f];
    o[0] = za[0] + (zb[0] - za[0]) * w; o[1] = za[1] + (zb[1] - za[1]) * w; o[2] = za[2] + (zb[2] - za[2]) * w;
  }
}

// LEDs lo..hi-1 of a table. tail: the LEDs are beyond the TV (a run of `len` LEDs settles toward avg); otherwise they are along it and nothing is added.
template <bool kVivid>
void applyRange(const Entry* t, int lo, int hi, const float zones[kZones][3], float* out, bool tail, int len, const float* avg, Hsv* zh, bool* zhHave) {
  const Weights255& kW = kWeights255;
  if (!tail || len <= 0) {
    for (int i = lo; i < hi; i++) ledColour<kVivid>(t[i], zones, kW, zh, zhHave, out + i * 3);
    return;
  }
  for (int i = lo; i < hi; i++) {
    float* o = out + i * 3;
    ledColour<kVivid>(t[i], zones, kW, zh, zhHave, o);
    if (t[i].d > 0) {                                    // beyond the TV: settle toward the end colour
      const float g = smoothstep01(t[i].d / (float)len);
      for (int c = 0; c < 3; c++) o[c] = o[c] + (avg[c] - o[c]) * g;
    }
  }
}
}  // namespace

void applyTable(const Entry* t, int count, const float zones[kZones][3], float* out, const SmoothInfo& sm, bool vivid) {
  float avgS[3] = {0, 0, 0}, avgE[3] = {0, 0, 0};
  if (sm.lenStart > 0) averageOf(zones, sm.maskStart, avgS);
  if (sm.lenEnd > 0) averageOf(zones, sm.maskEnd, avgE);
  Hsv zh[kZones];                                         // vivid: each zone's hue / saturation / value, converted the first time it is needed
  bool zhHave[kZones] = {};
  // the strip in three parts: before the first LED that is lined up with the TV, along it, and after the last one
  int a = sm.firstZero < count ? sm.firstZero : count;
  int b = sm.lastZero + 1;
  if (b < a) b = a;
  if (b > count) b = count;
  if (vivid) {
    applyRange<true>(t, 0, a, zones, out, true, sm.lenStart, avgS, zh, zhHave);
    applyRange<true>(t, a, b, zones, out, false, 0, avgS, zh, zhHave);
    applyRange<true>(t, b, count, zones, out, true, sm.lenEnd, avgE, zh, zhHave);
  } else {
    applyRange<false>(t, 0, a, zones, out, true, sm.lenStart, avgS, zh, zhHave);
    applyRange<false>(t, a, b, zones, out, false, 0, avgS, zh, zhHave);
    applyRange<false>(t, b, count, zones, out, true, sm.lenEnd, avgE, zh, zhHave);
  }
}
// ---------------------------------------------------------------------------
void sharpenTable(Entry* t, int count, int percent) {
  if (percent <= 0 || !t) return;
  const float s = percent >= 100 ? 1.f : percent / 100.f;
  const float k = s >= 1.f ? 0.f : 1.f / (1.f - s);
  for (int i = 0; i < count; i++) {
    Entry& e = t[i];
    if (e.a == e.b) continue;
    float x;
    if (s >= 1.f) x = e.f < 127.5f ? 0.f : 255.f;
    else {
      x = ((float)e.f - 127.5f) * k + 127.5f;
      x = x < 0.f ? 0.f : (x > 255.f ? 255.f : x);
    }
    e.f = (uint8_t)(x + 0.5f);
  }
}

void saturateColours(float* rgb, int n, float sat) {
  if (sat == 1.f || !rgb) return;
  int i = 0;
  for (; i + 1 < n; i += 2) {                          // two LEDs at a time: their sums do not depend on each other, so the FPU works on both at once (the same arithmetic as one at a time)
    float* a = rgb + i * 3;
    float* b = a + 3;
    const float ya = 0.299f * a[0] + 0.587f * a[1] + 0.114f * a[2], yb = 0.299f * b[0] + 0.587f * b[1] + 0.114f * b[2];
    const float a0 = ya + (a[0] - ya) * sat, a1 = ya + (a[1] - ya) * sat, a2 = ya + (a[2] - ya) * sat;
    const float b0 = yb + (b[0] - yb) * sat, b1 = yb + (b[1] - yb) * sat, b2 = yb + (b[2] - yb) * sat;
    a[0] = a0 < 0.f ? 0.f : (a0 > 255.f ? 255.f : a0); a[1] = a1 < 0.f ? 0.f : (a1 > 255.f ? 255.f : a1); a[2] = a2 < 0.f ? 0.f : (a2 > 255.f ? 255.f : a2);
    b[0] = b0 < 0.f ? 0.f : (b0 > 255.f ? 255.f : b0); b[1] = b1 < 0.f ? 0.f : (b1 > 255.f ? 255.f : b1); b[2] = b2 < 0.f ? 0.f : (b2 > 255.f ? 255.f : b2);
  }
  for (; i < n; i++) {
    float* c = rgb + i * 3;
    const float y = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
    for (int k = 0; k < 3; k++) {
      const float v = y + (c[k] - y) * sat;
      c[k] = v < 0.f ? 0.f : (v > 255.f ? 255.f : v);
    }
  }
}

int blurHalfWidth(int blurLeds) {
  if (blurLeds <= 0) return 0;
  const float sigma = blurLeds * 0.5f;
  const int h = (int)floorf((sqrtf(4.f * sigma * sigma + 1.f) - 1.f) * 0.5f + 0.5f);   // three box averages of half width h have sigma^2 = ((2h+1)^2 - 1) / 4
  return h < 1 ? 1 : h;
}

// One box average (half width h, 2h+1 LEDs) over m colours, with the first and last colour repeated beyond the ends; a running sum, so it costs the same for any width.
static void boxPassColours(const float* in, float* out, int m, int h) {
  const float inv = 1.f / (float)(2 * h + 1);
  float s0 = 0.f, s1 = 0.f, s2 = 0.f;
  for (int d = -h; d <= h; d++) {
    const float* p = in + (d < 0 ? 0 : (d >= m ? m - 1 : d)) * 3;
    s0 += p[0]; s1 += p[1]; s2 += p[2];
  }
  int i = 0;
  for (; i < m && i - h < 0; i++) {                    // the first LEDs: the left end of the window is clamped to LED 0
    out[i * 3] = s0 * inv; out[i * 3 + 1] = s1 * inv; out[i * 3 + 2] = s2 * inv;
    const int add = i + h + 1 >= m ? m - 1 : i + h + 1;
    const float* pa = in + add * 3;
    s0 += pa[0] - in[0]; s1 += pa[1] - in[1]; s2 += pa[2] - in[2];
  }
  for (; i < m && i + h + 1 < m; i++) {                // the middle: nothing is clamped
    out[i * 3] = s0 * inv; out[i * 3 + 1] = s1 * inv; out[i * 3 + 2] = s2 * inv;
    const float* pa = in + (i + h + 1) * 3;
    const float* ps = in + (i - h) * 3;
    s0 += pa[0] - ps[0]; s1 += pa[1] - ps[1]; s2 += pa[2] - ps[2];
  }
  for (; i < m; i++) {                                 // the last LEDs: the right end is clamped to the last LED
    out[i * 3] = s0 * inv; out[i * 3 + 1] = s1 * inv; out[i * 3 + 2] = s2 * inv;
    const int add = i + h + 1 >= m ? m - 1 : i + h + 1;
    const int sub = i - h < 0 ? 0 : i - h;
    const float* pa = in + add * 3;
    const float* ps = in + sub * 3;
    s0 += pa[0] - ps[0]; s1 += pa[1] - ps[1]; s2 += pa[2] - ps[2];
  }
}

void blurColours(float* rgb, int n, int first, int last, int blurLeds, float* scratch) {
  const int h = blurHalfWidth(blurLeds);
  if (!rgb || !scratch || n < 2 || h < 1) return;
  int a = 0, b = n - 1;
  if (first >= 0) {
    a = first > n - 1 ? n - 1 : first;
    if (last >= first) b = last > n - 1 ? n - 1 : last;
  }
  const int m = b - a + 1;
  if (m < 2) return;
  float* A = scratch;
  float* B = scratch + (size_t)m * 3;
  memcpy(A, rgb + (size_t)a * 3, (size_t)m * 3 * sizeof(float));
  boxPassColours(A, B, m, h);
  boxPassColours(B, A, m, h);
  boxPassColours(A, B, m, h);
  memcpy(rgb + (size_t)a * 3, B, (size_t)m * 3 * sizeof(float));
}

void motionReset(Motion& m) {
  m.n = 0;
  m.lastRawMs = 0;
  m.periodMs = 100.f;
}

void motionPush(Motion& m, uint32_t nowMs, const float zones[kZones][3]) {
  if (m.n > 0 && (uint32_t)(nowMs - m.lastRawMs) < 25u) {          // a burst: the newer picture replaces the one that came just before it
    memcpy(m.k[m.n - 1].z, zones, sizeof m.k[0].z);
    m.lastRawMs = nowMs;
    return;
  }
  uint32_t t;
  if (m.n == 0 || (uint32_t)(nowMs - m.lastRawMs) > 1500u) {       // the first picture, or the box was silent: start over
    m.n = 0;
    m.periodMs = 100.f;
    t = nowMs;
  } else {
    const float gap = (float)(uint32_t)(nowMs - m.lastRawMs);
    if (gap > 40.f && gap < 160.f) m.periodMs = 0.95f * m.periodMs + 0.05f * gap;   // a slow estimate of the real period, ignoring drops and bursts
    const uint32_t prevT = m.k[m.n - 1].ms;
    const float rawRel = (float)(int32_t)(nowMs - prevT);          // when it really arrived, relative to the previous picture's place on the clock
    float rel = m.periodMs + 0.15f * (rawRel - m.periodMs);        // the clock says one period later; it is pulled gently toward the real arrival
    if (rel < 1.f) rel = 1.f;
    t = prevT + (uint32_t)floorf(rel + 0.5f);
  }
  if (m.n == kMotionKnots) {
    for (int i = 1; i < kMotionKnots; i++) m.k[i - 1] = m.k[i];
    m.n--;
  }
  m.k[m.n].ms = t;
  memcpy(m.k[m.n].z, zones, sizeof m.k[0].z);
  m.n++;
  m.lastRawMs = nowMs;
}

// True when the pictures from knot `from` to the newest are all the same: showing any of them is the same, so nothing will change until a different one arrives.
static bool knotsSameFrom(const Motion& m, int from) {
  for (int j = from; j + 1 < m.n; j++)
    if (memcmp(m.k[j].z, m.k[j + 1].z, sizeof m.k[0].z) != 0) return false;
  return true;
}

bool motionAt(const Motion& m, uint32_t timeMs, float out[kZones][3]) {
  if (m.n == 0) { memset(out, 0, sizeof(float) * kZones * 3); return true; }
  const MotionKnot& last = m.k[m.n - 1];
  if ((int32_t)(timeMs - last.ms) >= 0) { memcpy(out, last.z, sizeof last.z); return true; }
  if ((int32_t)(timeMs - m.k[0].ms) <= 0) { memcpy(out, m.k[0].z, sizeof m.k[0].z); return knotsSameFrom(m, 0); }
  for (int i = 0; i + 1 < m.n; i++) {
    const int32_t a = (int32_t)(timeMs - m.k[i].ms);
    const int32_t span = (int32_t)(m.k[i + 1].ms - m.k[i].ms);
    if (a >= 0 && a < span) {
      if (knotsSameFrom(m, i)) { memcpy(out, m.k[i].z, sizeof m.k[i].z); return true; }   // a picture that does not change: settled, so the output can stop ticking
      const float f = (float)a / (float)span;
      for (int z = 0; z < kZones; z++)
        for (int c = 0; c < 3; c++) out[z][c] = m.k[i].z[z][c] + (m.k[i + 1].z[z][c] - m.k[i].z[z][c]) * f;
      return false;
    }
  }
  memcpy(out, last.z, sizeof last.z);                               // (not reached: the knots are in strictly increasing order)
  return true;
}

float motionDelayMs(const Motion& m, int percent) {
  const float p = m.periodMs < 50.f ? 50.f : (m.periodMs > 160.f ? 160.f : m.periodMs);
  const int pc = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  return p * (float)pc / 100.f;
}

void rttAdd(RttRing& r, float ms) {
  r.ms[r.head] = ms < 0.f ? -1.f : ms;
  r.head = (r.head + 1) % kRttSamples;
  if (r.count < kRttSamples) r.count++;
}

RttSummary rttSummary(const RttRing& r) {
  RttSummary s;
  s.samples = r.count;
  float sum = 0.f;
  int ok = 0;
  for (int i = 0; i < r.count; i++) {
    const float v = r.ms[i];                         // (while the ring is not full the used slots are 0..count-1)
    if (v < 0.f) { s.lost++; continue; }
    sum += v;
    if (ok == 0 || v < s.minMs) s.minMs = v;
    if (ok == 0 || v > s.maxMs) s.maxMs = v;
    ok++;
  }
  if (ok) s.avgMs = sum / ok;
  return s;
}

float wledShowMs(int totalLeds, float fps) {
  if (totalLeds < 1) return -1.f;
  const float clock = totalLeds * 0.030f;            // WS281x: 30 microseconds per LED
  const float wait = fps > 1.f ? 500.f / fps : 0.f;  // on average half a refresh until the received frame is picked up
  return clock + wait;
}

void darkOutside(float* rgb, int n, int first, int last) {
  if (!rgb || first < 0 || last < first) return;
  const int a = first < n ? first : n;
  for (int i = 0; i < a * 3; i++) rgb[i] = 0.f;
  for (int i = (last + 1 < 0 ? 0 : last + 1) * 3; i < n * 3; i++) rgb[i] = 0.f;
}

int zoneRuns(const Entry* t, int count, int zone, int out[][2], int maxRuns) {
  int runs = 0;
  bool in = false;
  for (int i = 0; i < count; i++) {
    const bool mine = (t[i].f < 128 ? t[i].a : t[i].b) == zone;
    if (mine && in) {
      out[runs - 1][1]++;
    } else if (mine) {
      if (runs >= maxRuns) break;
      out[runs][0] = i;
      out[runs][1] = 1;
      runs++;
    }
    in = mine;
  }
  return runs;
}

void zoneUsage(const Entry* t, int count, float out[kZones]) {
  for (int z = 0; z < kZones; z++) out[z] = 0.f;
  for (int i = 0; i < count; i++) {
    const float w = t[i].f / 255.f;
    out[t[i].a] += 1.f - w;
    out[t[i].b] += w;
  }
}

float leadFactor(float boost, int speedPercent, int steps) {
  const float b = boost < 0.f ? 0.f : (boost > 1.f ? 1.f : boost);
  float k = 0.f;                                          // nothing to undo at 100 % or when the packets did not follow each other
  if (speedPercent < 100 && steps > 0) {
    const float q = 1.f - (speedPercent < 10 ? 10 : speedPercent) / 100.f;
    float qm = 1.f;
    for (int i = 0; i < (steps > 8 ? 8 : steps); i++) qm *= q;
    k = qm / (1.f - qm);
  }
  return b * k;
}

float leadBoost(float y, float yPrev, float boost, int speedPercent, int steps) {
  return leadApply(y, yPrev, leadFactor(boost, speedPercent, steps));
}

int boxStepsForGap(uint32_t gapMs) {
  if (gapMs > 600) return 0;
  const int n = (int)((float)gapMs / 73.f + 0.35f);
  return n < 1 ? 1 : n;
}

void adjustZones(float out[kZones][3], const float raw[kZones][3], float br, float gm, float sat) {
  const float* in = &raw[0][0];
  float* o = &out[0][0];
  if (sat == 1.f && gm == 1.f) {                         // the usual case: only brightness
    const float k = br;
    for (int i = 0; i < kZones * 3; i++) {
      const float v = in[i] * k;
      o[i] = v < 0.f ? 0.f : (v > 255.f ? 255.f : v);
    }
    return;
  }
  const float inv255 = 1.f / 255.f, scale = br * 255.f;
  for (int z = 0; z < kZones; z++) {
    float c0 = in[z * 3] * inv255, c1 = in[z * 3 + 1] * inv255, c2 = in[z * 3 + 2] * inv255;
    if (sat != 1.f) {
      const float y = 0.299f * c0 + 0.587f * c1 + 0.114f * c2;
      c0 = y + (c0 - y) * sat; c1 = y + (c1 - y) * sat; c2 = y + (c2 - y) * sat;
      c0 = c0 < 0.f ? 0.f : (c0 > 1.f ? 1.f : c0);
      c1 = c1 < 0.f ? 0.f : (c1 > 1.f ? 1.f : c1);
      c2 = c2 < 0.f ? 0.f : (c2 > 1.f ? 1.f : c2);
    }
    if (gm != 1.f) { c0 = powf(c0, gm); c1 = powf(c1, gm); c2 = powf(c2, gm); }
    c0 *= scale; c1 *= scale; c2 *= scale;
    o[z * 3] = c0 < 0.f ? 0.f : (c0 > 255.f ? 255.f : c0);
    o[z * 3 + 1] = c1 < 0.f ? 0.f : (c1 > 255.f ? 255.f : c1);
    o[z * 3 + 2] = c2 < 0.f ? 0.f : (c2 > 255.f ? 255.f : c2);
  }
}

float easeColours(float* cur, const float* tgt, int n, float trim, float decay) {
  const float a = 1.f - decay;
  float w0 = 0.f, w1 = 0.f;                            // the largest |distance| (two running maxima, so the compares do not wait for each other); what is left after the step is that times `decay`
  const int total = n * 3;
  int k = 0;
  for (; k + 1 < total; k += 2) {
    const float d0 = tgt[k] * trim - cur[k], d1 = tgt[k + 1] * trim - cur[k + 1];
    cur[k] += d0 * a;
    cur[k + 1] += d1 * a;
    const float a0 = fabsf(d0), a1 = fabsf(d1);
    w0 = a0 > w0 ? a0 : w0;
    w1 = a1 > w1 ? a1 : w1;
  }
  for (; k < total; k++) {
    const float d = tgt[k] * trim - cur[k];
    cur[k] += d * a;
    const float ad = fabsf(d);
    w0 = ad > w0 ? ad : w0;
  }
  return (w0 > w1 ? w0 : w1) * decay;
}

void scaleColours(float* cur, const float* tgt, int n, float trim) {
  for (int k = 0; k < n * 3; k++) cur[k] = tgt[k] * trim;
}

size_t packColours(const float* cur, int n, bool whiteMin, uint8_t* out) {
  size_t o = 0;
  for (int i = 0; i < n; i++) {
    int r = (int)(cur[i * 3] + 0.5f), g = (int)(cur[i * 3 + 1] + 0.5f), b = (int)(cur[i * 3 + 2] + 0.5f);
    r = r < 0 ? 0 : (r > 255 ? 255 : r); g = g < 0 ? 0 : (g > 255 ? 255 : g); b = b < 0 ? 0 : (b > 255 ? 255 : b);
    if (whiteMin) {
      int w = r < g ? (r < b ? r : b) : (g < b ? g : b);
      out[o++] = (uint8_t)(r - w); out[o++] = (uint8_t)(g - w); out[o++] = (uint8_t)(b - w); out[o++] = (uint8_t)w;
    } else {
      out[o++] = (uint8_t)r; out[o++] = (uint8_t)g; out[o++] = (uint8_t)b;
    }
  }
  return o;
}

namespace {
template <bool kBand, bool kDither, bool kCut>
void finishRange(const float* cur, int n, float* held, float* resid, uint8_t* dark, float band, float cut, bool fresh, float* out) {
  for (int i = 0; i < n; i++) {
    float x0 = cur[i * 3], x1 = cur[i * 3 + 1], x2 = cur[i * 3 + 2];
    x0 = x0 < 0.f ? 0.f : (x0 > 255.f ? 255.f : x0);
    x1 = x1 < 0.f ? 0.f : (x1 > 255.f ? 255.f : x1);
    x2 = x2 < 0.f ? 0.f : (x2 > 255.f ? 255.f : x2);
    const bool wasDark = dark[i] != 0;
    if (kCut) {
      const float m = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
      if (fresh) dark[i] = m < cut;
      else if (wasDark) { if (m >= cut + 1.f) dark[i] = 0; }
      else if (m < cut) dark[i] = 1;
    } else {
      dark[i] = 0;
    }
    float* h = held + i * 3;
    float* e = resid + i * 3;
    float* o = out + i * 3;
    if (kCut && dark[i]) {
      h[0] = h[1] = h[2] = 0.f; e[0] = e[1] = e[2] = 0.f; o[0] = o[1] = o[2] = 0.f;
      continue;
    }
    const bool restart = fresh || wasDark;                                              // coming back from dark: take the colour as it is
    const float x[3] = {x0, x1, x2};
    for (int c = 0; c < 3; c++) {
      float hc = h[c];
      if (restart) { hc = x[c]; e[c] = 0.f; }
      else if (kBand && x[c] >= 1.f && hc >= 1.f && fabsf(x[c] - hc) <= band) { /* inside the deadband: keep what was shown */ }
      else hc = x[c];
      h[c] = hc;
      float v;
      if (kDither) {
        const float t = hc + e[c];
        v = (float)(int)(t + 0.5f);                    // t + 0.5 is never negative here (hc >= 0 and the carried error is >= -0.5), so truncating is the floor
        e[c] = t - v;
      } else {
        v = (float)(int)(hc + 0.5f);
        e[c] = 0.f;
      }
      o[c] = v < 0.f ? 0.f : (v > 255.f ? 255.f : v);
    }
  }
}
}  // namespace

void finishColours(const float* cur, int n, float* held, float* resid, uint8_t* dark, const FinishParams& p, bool moving, bool fresh, float* out) {
  const float cut = (float)p.blackCut;
  const bool band = p.deadband > 0.f, dith = p.dither && moving, useCut = p.blackCut > 0;
#define FWB_FINISH(B, D, C) finishRange<B, D, C>(cur, n, held, resid, dark, p.deadband, cut, fresh, out)
  if (band) { if (dith) { if (useCut) FWB_FINISH(true, true, true); else FWB_FINISH(true, true, false); } else { if (useCut) FWB_FINISH(true, false, true); else FWB_FINISH(true, false, false); } }
  else      { if (dith) { if (useCut) FWB_FINISH(false, true, true); else FWB_FINISH(false, true, false); } else { if (useCut) FWB_FINISH(false, false, true); else FWB_FINISH(false, false, false); } }
#undef FWB_FINISH
}

void speedReset(SpeedDetect& d) { d = SpeedDetect(); }

void speedPush(SpeedDetect& d, const uint8_t zones[kZones][3], int steps) {
  memmove(d.f[0], d.f[1], sizeof d.f[0] * 3);
  memmove(&d.steps[0], &d.steps[1], 3);
  memcpy(d.f[3], zones, sizeof d.f[3]);
  d.steps[3] = (uint8_t)(steps < 0 ? 0 : (steps > 255 ? 255 : steps));
  if (d.have < 4) d.have++;
  if (d.have < 4 || d.steps[1] != 1 || d.steps[2] != 1 || d.steps[3] != 1) return;      // a lost packet in between would hide steps
  int32_t num = 0, den = 0;
  int used = 0;
  for (int z = 0; z < kZones; z++)
    for (int c = 0; c < 3; c++) {
      const int y0 = d.f[1][z][c], y1 = d.f[2][z][c], y2 = d.f[3][z][c];
      if (abs((int)d.f[0][z][c] - y0) > 2) continue;                                    // it was already moving: not the start of a change
      const int d1 = y1 - y0, d2 = y2 - y1;
      if (abs(d1) < 24 || (y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2)) < 30) continue;   // too small to measure, or near the box's black cut-off
      num += d1 * d2;
      den += d1 * d1;
      used++;
    }
  if (used < kSpeedMinChannels || den <= 0) return;
  d.events++;
  d.q[d.head] = (float)num / (float)den;
  d.head = (d.head + 1) % kSpeedRing;
  if (d.nq < kSpeedRing) d.nq++;
  const SpeedResult r = speedResult(d);                                                 // hold the answer steady for the page
  if (!r.sure) { d.run = 0; return; }
  if (r.percent == d.cand) d.run++; else { d.cand = r.percent; d.run = 1; }
  if (d.run >= (d.shown < 0 || d.cand == d.shown ? kSpeedShowRun : kSpeedChangeRun)) d.shown = d.cand;
}

SpeedResult speedResult(const SpeedDetect& d) {
  SpeedResult r;
  r.estimates = d.nq;
  if (d.nq < kSpeedMinEvents) return r;
  int votes[19] = {};                                                                   // bins of 0.05: 0 = speed 100 % ... 18 = speed 10 %
  for (int i = 0; i < d.nq; i++) {
    const int bin = (int)floorf(d.q[i] * 20.f + 0.5f);
    if (bin >= 0 && bin <= 18) votes[bin]++;                                            // outside that (a negative estimate is plainly polluted) votes for nobody
  }
  int best = 0;
  for (int k = 1; k < 19; k++) if (votes[k] > votes[best]) best = k;                    // a tie goes to the lower bin (the faster speed)
  if (votes[best] == 0) return r;                                                       // nobody voted
  int second = 0;
  for (int k = 0; k < 19; k++) if (k != best && votes[k] > second) second = votes[k];
  r.peak = votes[best];
  r.second = second;
  r.percent = 100 - 5 * best;
  r.sure = r.peak >= kSpeedMinPeak && r.peak * 4 >= d.nq && r.peak >= 2 * second;
  return r;
}

float smoothDecay(float smoothing, float dtMs, float refMs) {
  const float s = smoothing < 0.f ? 0.f : (smoothing > 0.95f ? 0.95f : smoothing);
  if (dtMs <= 0.f) return 1.f;          // no time has passed: nothing moves
  if (s <= 0.f || refMs <= 0.f) return 0.f;   // no smoothing: jump straight to the target
  return powf(s, dtMs / refMs);
}

void ddpHeader(uint8_t* o, uint8_t seq, uint8_t dataType, uint32_t byteOffset, uint16_t length, bool push) {
  o[0] = 0x40 | (push ? 0x01 : 0x00);  // version 1, PUSH flag
  o[1] = seq;
  o[2] = dataType;
  o[3] = 0x01;  // destination id: default output device
  o[4] = (uint8_t)(byteOffset >> 24);
  o[5] = (uint8_t)(byteOffset >> 16);
  o[6] = (uint8_t)(byteOffset >> 8);
  o[7] = (uint8_t)byteOffset;
  o[8] = (uint8_t)(length >> 8);
  o[9] = (uint8_t)length;
}

}  // namespace fwb
