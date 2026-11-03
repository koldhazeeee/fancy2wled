#include "core.h"

#include <math.h>

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
  for (size_t i = 0; i < n; i++) pt[i] = body[i] ^ key[i % q];
  if (pt[0] != 0x06 || pt[1] != 0x04 || pt[2] != 0x06) return false;  // built-in integrity check
  for (int z = 0; z < kZones; z++)
    for (int c = 0; c < 3; c++) zones[z][c] = pt[3 + z * 3 + c];
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

// Zone j sits at t = (j + 0.5) / k along its edge. d is filled in by the caller.
static Entry interp(Edge e, float t) {
  int k;
  const uint8_t* z = edgeZones(e, &k);
  float pos = t * k - 0.5f;
  if (pos < 0.f) pos = 0.f;
  if (pos > k - 1.f) pos = k - 1.f;
  int j0 = (int)floorf(pos);
  int j1 = j0 + 1 < k ? j0 + 1 : k - 1;
  int f = roundHalfUp((pos - j0) * 255.f);
  if (f < 0) f = 0;
  if (f > 255) f = 255;
  return Entry{z[j0], z[j1], (uint8_t)f, 0};
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

void applyTable(const Entry* t, int count, const float zones[kZones][3], float* out, const SmoothInfo& sm) {
  float avgS[3] = {0, 0, 0}, avgE[3] = {0, 0, 0};
  if (sm.lenStart > 0) averageOf(zones, sm.maskStart, avgS);
  if (sm.lenEnd > 0) averageOf(zones, sm.maskEnd, avgE);
  for (int i = 0; i < count; i++) {
    const float w = t[i].f / 255.f;
    const float* za = zones[t[i].a];
    const float* zb = zones[t[i].b];
    const bool before = i < sm.firstZero;
    const int len = before ? sm.lenStart : (i > sm.lastZero ? sm.lenEnd : 0);
    const float* avg = before ? avgS : avgE;
    const float g = (len > 0 && t[i].d > 0) ? smoothstep01(t[i].d / (float)len) : 0.f;
    for (int c = 0; c < 3; c++) {
      float col = za[c] + (zb[c] - za[c]) * w;
      out[i * 3 + c] = col + (avg[c] - col) * g;
    }
  }
}
// ---------------------------------------------------------------------------
void zoneUsage(const Entry* t, int count, float out[kZones]) {
  for (int z = 0; z < kZones; z++) out[z] = 0.f;
  for (int i = 0; i < count; i++) {
    const float w = t[i].f / 255.f;
    out[t[i].a] += 1.f - w;
    out[t[i].b] += w;
  }
}

float leadBoost(float y, float yPrev, float boost) {
  const float b = boost < 0.f ? 0.f : (boost > 1.f ? 1.f : boost);
  const float v = y + b * kBoxLead * (y - yPrev);
  return v < 0.f ? 0.f : (v > 255.f ? 255.f : v);
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
