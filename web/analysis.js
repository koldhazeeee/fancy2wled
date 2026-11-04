/* Image analysis + layout maths for Fancy2WLED (Sync Box -> WLED).
 *
 * Runs in the phone's browser (calibration page) and in Node (tests). No dependencies.
 * Images are {w, h, d: Float32Array} grey-scale (0..255), row-major.
 *
 * Layout rule (same as core.cpp): a LED at picture position (u, v) -- (0,0) = top-left of the picture,
 * (1,1) = bottom-right, values outside are allowed -- takes its colour from the border zones along the
 * nearest picture edge. A "table" is one [zoneA, zoneB, f, d] entry per LED: f = 0..255 blends zone A -> B,
 * d = 0..255 is how far the LED is beyond the TV along its edge (0..2 picture widths); LEDs beyond the TV ease
 * toward the colour of their end zone on the device.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory();
  else root.FWB = factory();
})(typeof self !== 'undefined' ? self : this, function () {
  'use strict';

  var OUT_MAX = 2.0, MIN_OUT_Q = 19, DEAD = 0.02, DEFAULT_ASPECT = 9 / 16;
  // The calibration screen shows 9 squares at these picture positions (centres, picture coords; side = fraction of the
  // picture WIDTH). web/screen.html draws the same ones: keep them in sync (test/test_analysis.js checks).
  var SQUARES = { us: [0.08, 0.5, 0.92], vs: [0.12, 0.5, 0.88], side: 0.06 };

  /* ------------------------------------------------------------------ images */
  function newImg(w, h, fill) {
    var d = new Float32Array(w * h);
    if (fill) d.fill(fill);
    return { w: w, h: h, d: d };
  }
  function meanImgs(list) {
    var o = newImg(list[0].w, list[0].h), i;
    for (var k = 0; k < list.length; k++) for (i = 0; i < o.d.length; i++) o.d[i] += list[k].d[i];
    for (i = 0; i < o.d.length; i++) o.d[i] /= list.length;
    return o;
  }
  function subImg(a, b) {
    var o = newImg(a.w, a.h);
    for (var i = 0; i < o.d.length; i++) o.d[i] = a.d[i] - b.d[i];
    return o;
  }
  function lerpImg(a, b, t) {
    var o = newImg(a.w, a.h);
    for (var i = 0; i < o.d.length; i++) o.d[i] = a.d[i] + (b.d[i] - a.d[i]) * t;
    return o;
  }
  function meanOf(img) { var s = 0; for (var i = 0; i < img.d.length; i++) s += img.d[i]; return s / img.d.length; }
  function sorted(arr) { var c = Float32Array.from(arr); c.sort(); return c; }
  function median(arr) { return sorted(arr)[arr.length >> 1]; }
  function percentile(arr, p) { var c = sorted(arr); return c[Math.min(c.length - 1, Math.floor(p * (c.length - 1)))]; }

  // Separable box blur with edge clamping (window shrinks at the borders).
  function boxBlur(img, r) {
    var w = img.w, h = img.h, tmp = new Float32Array(w * h), out = newImg(w, h), x, y, a, b;
    var P = new Float64Array(Math.max(w, h) + 1);
    for (y = 0; y < h; y++) {
      P[0] = 0;
      for (x = 0; x < w; x++) P[x + 1] = P[x] + img.d[y * w + x];
      for (x = 0; x < w; x++) {
        a = Math.max(0, x - r); b = Math.min(w, x + r + 1);
        tmp[y * w + x] = (P[b] - P[a]) / (b - a);
      }
    }
    for (x = 0; x < w; x++) {
      P[0] = 0;
      for (y = 0; y < h; y++) P[y + 1] = P[y] + tmp[y * w + x];
      for (y = 0; y < h; y++) {
        a = Math.max(0, y - r); b = Math.min(h, y + r + 1);
        out.d[y * w + x] = (P[b] - P[a]) / (b - a);
      }
    }
    return out;
  }

  function otsu(values, lo, hi) {
    var bins = 64, hist = new Float64Array(bins), n = 0, span = Math.max(1e-6, hi - lo), i;
    for (i = 0; i < values.length; i++) {
      var v = values[i];
      if (v < lo) continue;
      hist[Math.min(bins - 1, Math.floor(((v - lo) / span) * bins))]++;
      n++;
    }
    if (!n) return lo;
    var sum = 0;
    for (i = 0; i < bins; i++) sum += i * hist[i];
    var wB = 0, sB = 0, best = -1, bestT = 0;
    for (var t = 0; t < bins; t++) {
      wB += hist[t];
      if (!wB) continue;
      var wF = n - wB;
      if (!wF) break;
      sB += t * hist[t];
      var mB = sB / wB, mF = (sum - sB) / wF, between = wB * wF * (mB - mF) * (mB - mF);
      if (between > best) { best = between; bestT = t; }
    }
    return lo + ((bestT + 1) / bins) * span;
  }

  // 4-connected components of `mask`. components(mask,w,h,seed): the one containing pixel `seed`;
  // seed < 0: the largest. allComponents(mask,w,h,minArea): every one with at least minArea pixels.
  function floodFill(mask, w, h, label, stack, s, id) {
    var pix = [], sp = 0;
    stack[sp++] = s; label[s] = id;
    while (sp) {
      var p = stack[--sp];
      pix.push(p);
      var x = p % w, y = (p / w) | 0;
      if (x > 0 && mask[p - 1] && !label[p - 1]) { label[p - 1] = id; stack[sp++] = p - 1; }
      if (x < w - 1 && mask[p + 1] && !label[p + 1]) { label[p + 1] = id; stack[sp++] = p + 1; }
      if (y > 0 && mask[p - w] && !label[p - w]) { label[p - w] = id; stack[sp++] = p - w; }
      if (y < h - 1 && mask[p + w] && !label[p + w]) { label[p + w] = id; stack[sp++] = p + w; }
    }
    return pix;
  }
  function component(mask, w, h, seed) {
    var label = new Int32Array(w * h), stack = new Int32Array(w * h), best = null, id = 0;
    if (seed >= 0) return mask[seed] ? floodFill(mask, w, h, label, stack, seed, 1) : [];
    for (var i = 0; i < w * h; i++) {
      if (mask[i] && !label[i]) {
        var pix = floodFill(mask, w, h, label, stack, i, ++id);
        if (!best || pix.length > best.length) best = pix;
      }
    }
    return best || [];
  }
  function allComponents(mask, w, h, minArea) {
    var label = new Int32Array(w * h), stack = new Int32Array(w * h), out = [], id = 0;
    for (var i = 0; i < w * h; i++) {
      if (mask[i] && !label[i]) {
        var pix = floodFill(mask, w, h, label, stack, i, ++id);
        if (pix.length >= minArea) out.push(pix);
      }
    }
    return out;
  }

  /* ----------------------------------------------------------- homographies */
  // 3x3 row-major [h0..h8]; image (x,y) -> (u,v).
  function applyH(H, x, y) {
    var wv = H[6] * x + H[7] * y + H[8];
    return [(H[0] * x + H[1] * y + H[2]) / wv, (H[3] * x + H[4] * y + H[5]) / wv];
  }
  function mul3(A, B) {
    var C = new Array(9);
    for (var i = 0; i < 3; i++) for (var j = 0; j < 3; j++) C[i * 3 + j] = A[i * 3] * B[j] + A[i * 3 + 1] * B[3 + j] + A[i * 3 + 2] * B[6 + j];
    return C;
  }
  function invertH(H) {
    var a = H[0], b = H[1], c = H[2], d = H[3], e = H[4], f = H[5], g = H[6], h = H[7], i = H[8];
    var A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g, det = a * A + b * B + c * C;
    if (Math.abs(det) < 1e-18) return null;
    return [A / det, -(b * i - c * h) / det, (b * f - c * e) / det,
            B / det, (a * i - c * g) / det, -(a * f - c * d) / det,
            C / det, -(a * h - b * g) / det, (a * e - b * d) / det];
  }
  function solveLinear(A, bvec) {                      // Gaussian elimination with partial pivoting, A is n x n
    var n = bvec.length, M = [], r, c, k;
    for (r = 0; r < n; r++) { M.push(A[r].slice()); M[r].push(bvec[r]); }
    for (c = 0; c < n; c++) {
      var piv = c;
      for (r = c + 1; r < n; r++) if (Math.abs(M[r][c]) > Math.abs(M[piv][c])) piv = r;
      if (Math.abs(M[piv][c]) < 1e-12) return null;
      var t = M[c]; M[c] = M[piv]; M[piv] = t;
      for (r = 0; r < n; r++) {
        if (r === c) continue;
        var f = M[r][c] / M[c][c];
        for (k = c; k <= n; k++) M[r][k] -= f * M[c][k];
      }
    }
    var x = [];
    for (r = 0; r < n; r++) x.push(M[r][n] / M[r][r]);
    return x;
  }
  // Least-squares homography from point pairs [{x,y,u,v}] (4 or more). Coordinates are normalised first for stability.
  function homographyFit(pairs) {
    var n = pairs.length, i, cx = 0, cy = 0;
    if (n < 4) return null;
    for (i = 0; i < n; i++) { cx += pairs[i].x; cy += pairs[i].y; }
    cx /= n; cy /= n;
    var s = 0;
    for (i = 0; i < n; i++) s += Math.hypot(pairs[i].x - cx, pairs[i].y - cy);
    s = Math.max(1e-6, s / n);
    var AtA = [], Atb = [], r, c;
    for (r = 0; r < 8; r++) { AtA.push(new Array(8).fill(0)); Atb.push(0); }
    for (i = 0; i < n; i++) {
      var x = (pairs[i].x - cx) / s, y = (pairs[i].y - cy) / s, u = pairs[i].u, v = pairs[i].v;
      var rows = [[[x, y, 1, 0, 0, 0, -u * x, -u * y], u], [[0, 0, 0, x, y, 1, -v * x, -v * y], v]];
      for (var q = 0; q < 2; q++) {
        var row = rows[q][0], rhs = rows[q][1];
        for (r = 0; r < 8; r++) { Atb[r] += row[r] * rhs; for (c = 0; c < 8; c++) AtA[r][c] += row[r] * row[c]; }
      }
    }
    var h = solveLinear(AtA, Atb);
    if (!h) return null;
    h.push(1);
    var T = [1 / s, 0, -cx / s, 0, 1 / s, -cy / s, 0, 0, 1];
    return mul3(h, T);
  }
  // Homography mapping the four image corners [TL, TR, BR, BL] onto the unit square.
  function homography(c) {
    var dst = [[0, 0], [1, 0], [1, 1], [0, 1]], pairs = [];
    for (var i = 0; i < 4; i++) pairs.push({ x: c[i][0], y: c[i][1], u: dst[i][0], v: dst[i][1] });
    return homographyFit(pairs);
  }
  function quadFromH(H) {                             // image corners of the picture, [TL, TR, BR, BL]
    var inv = invertH(H);
    if (!inv) return null;
    return [[0, 0], [1, 0], [1, 1], [0, 1]].map(function (p) { return applyH(inv, p[0], p[1]); });
  }

  /* ---------------------------------------------------------------- TV finder */
  function polyArea(p) {
    var s = 0;
    for (var i = 0; i < p.length; i++) { var q = p[(i + 1) % p.length]; s += p[i][0] * q[1] - q[0] * p[i][1]; }
    return s / 2;
  }
  // Whole-screen white flash: a rough outline of the TV (used as a cross-check and as a fallback).
  function detectTV(white, dark) {
    var w = white.w, h = white.h, i;
    var diff = boxBlur(subImg(white, dark), 1);
    for (i = 0; i < diff.d.length; i++) if (diff.d[i] < 0) diff.d[i] = 0;
    var p99 = percentile(diff.d, 0.99);
    if (p99 < 20) return { ok: false, reason: 'The camera did not see anything change when the TV went white. Is the calibration screen showing full-screen on the TV?' };
    var thr = Math.min(0.7 * p99, Math.max(0.3 * p99, otsu(diff.d, 0.05 * p99, p99)));
    var mask = new Uint8Array(w * h);
    for (i = 0; i < mask.length; i++) mask[i] = diff.d[i] > thr ? 1 : 0;
    var pix = component(mask, w, h, -1);
    if (pix.length < 0.01 * w * h) return { ok: false, reason: 'The bright area on the TV looks too small. Move closer or zoom in so the TV fills more of the picture.' };
    var tl = null, tr = null, br = null, bl = null, a, b;
    var mTL = Infinity, mBR = -Infinity, mTR = -Infinity, mBL = Infinity;
    for (var k = 0; k < pix.length; k++) {
      var x = pix[k] % w, y = (pix[k] / w) | 0;
      a = x + y; b = x - y;
      if (a < mTL) { mTL = a; tl = [x, y]; }
      if (a > mBR) { mBR = a; br = [x + 1, y + 1]; }
      if (b > mTR) { mTR = b; tr = [x + 1, y]; }
      if (b < mBL) { mBL = b; bl = [x, y + 1]; }
    }
    var corners = [tl, tr, br, bl], area = Math.abs(polyArea(corners)), fill = pix.length / Math.max(1, area);
    var topW = Math.hypot(tr[0] - tl[0], tr[1] - tl[1]), sideH = Math.hypot(bl[0] - tl[0], bl[1] - tl[1]), aspect = topW / Math.max(1e-6, sideH);
    var res = { ok: true, corners: corners, area: area, fill: fill, aspect: aspect, threshold: thr, warnings: [] };
    if (fill < 0.8) res.warnings.push('The detected TV shape is not very rectangular (fill ' + fill.toFixed(2) + '); glare or a lamp may be confusing it.');
    if (aspect < 1.0) res.warnings.push('The TV looks taller than wide: hold the phone upright (not rotated) and keep the TV level.');
    else if (aspect < 1.3 || aspect > 2.6) res.warnings.push('Unusual TV proportions (' + aspect.toFixed(2) + ':1); stand more in front of the TV for best results.');
    return res;
  }

  // Nine squares at known picture positions: their centres are found to sub-pixel accuracy (glare/bloom around a bright
  // area is symmetric, so unlike an outline it does not move the centre) and give an exact camera -> picture mapping.
  function squareCentres() {
    var out = [];
    for (var r = 0; r < 3; r++) for (var c = 0; c < 3; c++) out.push([SQUARES.us[c], SQUARES.vs[r]]);
    return out;
  }
  function detectSquares(img, dark) {
    var w = img.w, h = img.h, i, diff = boxBlur(subImg(img, dark), 1);
    for (i = 0; i < diff.d.length; i++) if (diff.d[i] < 0) diff.d[i] = 0;
    var top = percentile(diff.d, 0.999);
    if (top < 20) return { ok: false, reason: 'The camera did not see the squares appear on the TV. Is the calibration screen showing full-screen on the TV, and is the TV in view?' };
    var thr = 0.45 * top, mask = new Uint8Array(w * h);
    for (i = 0; i < mask.length; i++) mask[i] = diff.d[i] > thr ? 1 : 0;
    var comps = allComponents(mask, w, h, 3).sort(function (a, b) { return b.length - a.length; });
    if (comps.length < 9) return { ok: false, reason: 'I could only find ' + comps.length + ' of the 9 squares. Make sure the whole TV is in view, move a little closer, and dim the room.' };
    comps = comps.slice(0, 9);
    var areas = comps.map(function (c) { return c.length; }), med = median(areas);
    if (areas.some(function (a) { return a < 0.35 * med || a > 2.8 * med; })) return { ok: false, reason: 'The squares did not look alike (glare or a lamp is in view). Dim the room and try again.' };
    var pts = comps.map(function (pix) {
      var sx = 0, sy = 0, sm = 0;
      for (var k = 0; k < pix.length; k++) {
        var p = pix[k], x = p % w, y = (p / w) | 0, v = diff.d[p] - thr * 0.5;
        if (v <= 0) continue;
        sx += (x + 0.5) * v; sy += (y + 0.5) * v; sm += v;
      }
      return { x: sx / sm, y: sy / sm, area: pix.length };
    });
    pts.sort(function (a, b) { return a.y - b.y; });                          // 3 rows of 3, top to bottom
    var rows = [pts.slice(0, 3), pts.slice(3, 6), pts.slice(6, 9)];
    var rowMid = rows.map(function (r) { return r.reduce(function (s, p) { return s + p.y; }, 0) / 3; });
    var gap = Math.min(rowMid[1] - rowMid[0], rowMid[2] - rowMid[1]);
    var spread = Math.max.apply(null, rows.map(function (r) { return Math.max(r[0].y, r[1].y, r[2].y) - Math.min(r[0].y, r[1].y, r[2].y); }));
    if (spread > 0.7 * gap) return { ok: false, reason: 'The TV looks rotated or tilted too much to tell the rows of squares apart. Stand more in front of the TV with the phone upright.' };
    var ordered = [];
    rows.forEach(function (r) { r.sort(function (a, b) { return a.x - b.x; }); ordered = ordered.concat(r); });
    var want = squareCentres(), pairs = ordered.map(function (p, i) { return { x: p.x, y: p.y, u: want[i][0], v: want[i][1] }; });
    var H = homographyFit(pairs);
    if (!H) return { ok: false, reason: 'Could not work out the shape of the TV from the squares.' };
    var se = 0;
    for (i = 0; i < pairs.length; i++) { var uv = applyH(H, pairs[i].x, pairs[i].y); se += Math.pow(uv[0] - pairs[i].u, 2) + Math.pow(uv[1] - pairs[i].v, 2); }
    var rms = Math.sqrt(se / pairs.length), quad = quadFromH(H);
    var res = { ok: true, H: H, quad: quad, points: pairs, rms: rms, warnings: [] };
    var side = Math.sqrt(med);
    if (side < 2.5) res.warnings.push('The squares are very small in the picture (about ' + side.toFixed(1) + ' px). Move closer or zoom in for a more precise result.');
    if (rms > 0.01) res.warnings.push('The squares do not line up perfectly (error ' + (rms * 100).toFixed(1) + '% of the picture width): the TV may be curved or the phone lens distorted.');
    return res;
  }

  /* -------------------------------------------------------------- LED finder */
  // Where did the lit LEDs show up? frame/base are camera images with the LEDs lit / all off.
  function locateBlob(frame, base, opts) {
    opts = opts || {};
    var w = frame.w, h = frame.h, i;
    var D = subImg(frame, base), med = median(D.d);
    for (i = 0; i < D.d.length; i++) D.d[i] -= med;                  // remove exposure drift / ambient pedestal
    var r = opts.radius || (Math.round(w / 128) + 1), B = boxBlur(D, r);
    var pk = 0, pv = -Infinity;
    for (i = 0; i < B.d.length; i++) if (B.d[i] > pv) { pv = B.d[i]; pk = i; }
    var dev = new Float32Array(B.d.length);
    for (i = 0; i < dev.length; i++) dev[i] = Math.abs(B.d[i]);
    var sigma = 1.4826 * median(dev), minPeak = opts.minPeak || 5, visible = pv >= Math.max(minPeak, 7 * sigma);
    var res = { visible: visible, peak: pv, sigma: sigma, x: 0, y: 0, mass: 0, area: 0, touches: false, bbox: null };
    if (!visible) return res;
    var thr = Math.max(0.35 * pv, 3.5 * sigma, 2), mask = new Uint8Array(w * h);
    for (i = 0; i < mask.length; i++) mask[i] = B.d[i] > thr ? 1 : 0;
    var pix = component(mask, w, h, pk), sx = 0, sy = 0, sm = 0, x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (var k = 0; k < pix.length; k++) {
      var p = pix[k], x = p % w, y = (p / w) | 0, v = B.d[p];
      sx += (x + 0.5) * v; sy += (y + 0.5) * v; sm += v;
      if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
    }
    res.x = sx / sm; res.y = sy / sm; res.mass = sm; res.area = pix.length;
    res.bbox = [x0, y0, x1 + 1, y1 + 1];
    res.touches = x0 <= 1 || y0 <= 1 || x1 >= w - 2 || y1 >= h - 2;
    return res;
  }

  /* ------------------------------------------------------- uv -> table rule */
  var EDGE = { top: [0, 1, 2, 3, 4, 5], right: [5, 11, 17, 23], bottom: [18, 19, 20, 21, 22, 23], left: [0, 6, 12, 18] };
  function clamp01(x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
  // Where along its edge each zone looks (fraction of the picture width for top / bottom, height for left / right): the middle of the window the box
  // averages for that zone. Not an even grid: measured with tools/sync_probe, see docs/PROTOCOL.md. Same numbers as core.cpp.
  var POS_ACROSS = [0.0797, 0.2359, 0.3922, 0.5797, 0.7359, 0.8922], POS_DOWN = [0.0287, 0.3356, 0.6134, 0.9736];
  var EDGE_POS = { top: POS_ACROSS, bottom: POS_ACROSS, left: POS_DOWN, right: POS_DOWN };
  function interp(edge, t) {
    var z = EDGE[edge], p = EDGE_POS[edge], k = z.length, j0 = 0;
    if (t <= p[0]) return [z[0], z[1], 0];
    if (t >= p[k - 1]) return [z[k - 1], z[k - 1], 0];
    while (j0 + 2 < k && t >= p[j0 + 1]) j0++;
    return [z[j0], z[j0 + 1], Math.min(255, Math.max(0, Math.floor((t - p[j0]) / (p[j0 + 1] - p[j0]) * 255 + 0.5)))];
  }
  function edgeOf(u, v) {
    var cu = clamp01(u), cv = clamp01(v);
    if (u > 0 && u < 1 && v > 0 && v < 1) {
      var d = [['top', v], ['bottom', 1 - v], ['left', u], ['right', 1 - u]], best = d[0];
      for (var i = 0; i < d.length; i++) if (d[i][1] < best[1]) best = d[i];
      return best[0];
    }
    if (cv <= 0) return 'top';
    if (cv >= 1) return 'bottom';
    if (cu <= 0) return 'left';
    return 'right';
  }
  var EDGE_NAMES = ['top', 'right', 'bottom', 'left'];
  // edge (optional): force the border to take the colour from, as a name or 0 top / 1 right / 2 bottom / 3 left
  function uvToEntry(u, v, aspect, edge) {
    aspect = aspect || DEFAULT_ASPECT;
    var e = (typeof edge === 'number') ? EDGE_NAMES[edge] : (edge || edgeOf(u, v)), horiz = (e === 'top' || e === 'bottom');
    var ent = interp(e, horiz ? clamp01(u) : clamp01(v));
    var over = horiz ? Math.max(0, -u, u - 1) : Math.max(0, -v, v - 1) * aspect;          // overshoot along the edge, in picture widths
    over = Math.max(0, over - DEAD);                                                       // small overshoot at a corner does not count
    ent.push(Math.min(255, Math.max(0, Math.floor(Math.min(over, OUT_MAX) / OUT_MAX * 255 + 0.5))));
    return ent;
  }

  /* --------------------------------------------------------- manual layouts */
  // One straight run along one side of the picture. edge: 'top' (ceiling: top row of the colour map), 'bottom', 'left',
  // 'right' (or 0..3). led0Right: LED 0 at the right end of a top/bottom run, or at the bottom end of a left/right run.
  function tableLinear(count, led0Right, spanFirst, spanLast, edge) {
    var first = 0, last = count - 1;
    if (spanFirst != null && spanFirst >= 0) { first = spanFirst; last = spanLast; }
    var name = (typeof edge === 'number') ? EDGE_NAMES[edge] : (edge || 'top');
    var width = Math.max(1, last - first + 1), out = [];
    for (var i = 0; i < count; i++) {
      var t = (i - first + 0.5) / width;
      if (led0Right) t = 1 - t;
      var uv = name === 'bottom' ? [t, 1] : (name === 'left' ? [0, t] : (name === 'right' ? [1, t] : [t, 0]));
      out.push(uvToEntry(uv[0], uv[1], DEFAULT_ASPECT, name));
    }
    return out;
  }
  function autoSides(count, aw, ah) {
    var tot = 2 * (aw + ah), top = Math.floor(count * aw / tot + 0.5), right = Math.floor(count * ah / tot + 0.5);
    return [top, right, top, count - 2 * top - right];
  }
  function tableLoop(count, startCorner, ccw, sides, shift, aw, ah) {
    var s = sides ? sides.slice() : autoSides(count, aw || 16, ah || 9);
    if (s[0] + s[1] + s[2] + s[3] !== count || s.some(function (x) { return x <= 0; })) return null;
    var offs = [0, s[0], s[0] + s[1], s[0] + s[1] + s[2]], off = offs[startCorner] + (shift || 0), names = ['top', 'right', 'bottom', 'left'], out = [];
    for (var i = 0; i < count; i++) {
      var idx = (ccw ? off - 1 - i : off + i) % count;
      if (idx < 0) idx += count;
      var e = 0;
      while (e < 3 && idx >= s[e]) { idx -= s[e]; e++; }
      var t = (idx + 0.5) / s[e];
      if (e === 2 || e === 3) t = 1 - t;
      var ent = interp(names[e], t);
      ent.push(0);
      out.push(ent);
    }
    return out;
  }

  /* ------------------------------------------- smoothing beyond the TV (device side) */
  // The two tails of a strip (before / after the part lined up with the TV) each ease toward the colour of the zone at their end
  // of the strip (the corner zone; no other zone counts), reaching it at the strip's own end when percent = 100.
  function smoothInfo(table, percent) {
    var n = table.length, firstZero = n, lastZero = -1, i;
    for (i = 0; i < n; i++) if (table[i][3] === 0) { if (firstZero === n) firstZero = i; lastZero = i; }
    function tail(lo, hi) {
      var dmax = 0;
      for (var k = lo; k < hi; k++) if (table[k][3] > dmax) dmax = table[k][3];
      if (percent <= 0 || dmax === 0) return 0;
      return Math.max(MIN_OUT_Q, Math.floor(dmax * percent / 100 + 0.5));
    }
    // the zone each tail eases toward: just the one the strip's outermost LED shows (the corner zone for a strip along an edge)
    function outerZone(entry) { return [entry[2] < 128 ? entry[0] : entry[1]]; }
    return { firstZero: firstZero, lastZero: lastZero, lenStart: tail(0, firstZero), lenEnd: tail(lastZero + 1, n),
             zonesStart: (firstZero > 0 && firstZero < n) ? outerZone(table[0]) : null,
             zonesEnd: (lastZero >= 0 && lastZero < n - 1) ? outerZone(table[n - 1]) : null };
  }  function smoothstep(x) { x = Math.min(Math.max(x, 0), 1); return x * x * (3 - 2 * x); }
  // Colours for every LED from 24 zone colours [[r,g,b]...] (what the device does each frame; here for tests/previews).
  function applyTable(table, zones, info) {
    function average(idx) {      // average of the given zones (null / missing: all zones)
      var zs = idx ? idx.map(function (j) { return zones[j]; }) : zones, a = [0, 0, 0], i, c;
      for (i = 0; i < zs.length; i++) for (c = 0; c < 3; c++) a[c] += zs[i][c] / zs.length;
      return a;
    }
    var avgStart = average(info.zonesStart), avgEnd = average(info.zonesEnd);
    return table.map(function (e, idx) {
      var avg = idx < info.firstZero ? avgStart : avgEnd;
      var len = idx < info.firstZero ? info.lenStart : (idx > info.lastZero ? info.lenEnd : 0);
      var w = e[2] / 255, g = (len > 0 && e[3] > 0) ? smoothstep(e[3] / len) : 0, col = [];
      for (var k = 0; k < 3; k++) { var x = zones[e[0]][k] + (zones[e[1]][k] - zones[e[0]][k]) * w; col.push(x + (avg[k] - x) * g); }
      return col;
    });
  }
  /* ------------------------------------------------- samples -> LED positions */
  // samples: [{idx, u, v}] (idx = LED index of the lit block's centre).
  function cleanSamples(s) {
    s = s.slice().sort(function (a, b) { return a.idx - b.idx; });
    if (s.length < 4) return s;
    var steps = [], i;
    for (i = 1; i < s.length; i++) steps.push(Math.hypot(s[i].u - s[i - 1].u, s[i].v - s[i - 1].v));
    var lim = Math.max(0.06, 4 * median(steps)), keep = [];
    for (i = 0; i < s.length; i++) {
      if (i === 0 || i === s.length - 1) { keep.push(s[i]); continue; }
      var mu = (s[i - 1].u + s[i + 1].u) / 2, mv = (s[i - 1].v + s[i + 1].v) / 2;
      if (Math.hypot(s[i].u - mu, s[i].v - mv) <= lim) keep.push(s[i]);
    }
    return keep;
  }
  function lineFit(pts, key) {  // least squares  value = a + b * idx
    var n = pts.length, sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (var i = 0; i < n; i++) { var p = pts[i]; sx += p.idx; sy += p[key]; sxx += p.idx * p.idx; sxy += p.idx * p[key]; }
    var den = n * sxx - sx * sx;
    if (n < 2 || Math.abs(den) < 1e-9) return [sy / n, 0];
    var b = (n * sxy - sx * sy) / den;
    return [(sy - b * sx) / n, b];
  }
  // Remove measurement noise: if a coordinate runs in a straight line along the strip, replace it by the fitted line
  // (averaging all the samples makes it more precise than any single glow centre); otherwise smooth it with a short
  // local straight-line fit that keeps corners.
  function smoothSamples(s, tol) {
    tol = tol || 0.006;
    var out = s.map(function (p) { return { idx: p.idx, u: p.u, v: p.v, x: p.x, y: p.y, peak: p.peak, touches: p.touches }; });
    ['u', 'v'].forEach(function (key) {
      if (s.length < 5) return;
      var f = lineFit(s, key), se = 0, i;
      for (i = 0; i < s.length; i++) se += Math.pow(s[i][key] - (f[0] + f[1] * s[i].idx), 2);
      if (Math.sqrt(se / s.length) <= tol * (key === 'v' ? 4 : 1)) {
        for (i = 0; i < s.length; i++) out[i][key] = f[0] + f[1] * s[i].idx;
        return;
      }
      for (i = 0; i < s.length; i++) {
        var win = s.slice(Math.max(0, i - 2), Math.min(s.length, i + 3));
        if (win.length < 3) continue;
        var g = lineFit(win, key);
        out[i][key] = g[0] + g[1] * s[i].idx;
      }
    });
    return out;
  }
  function positionsFromSamples(samples, start, count) {
    var s = smoothSamples(cleanSamples(samples)), U = [], V = [];
    if (!s.length) return null;
    var k = Math.min(4, s.length), head = s.slice(0, k), tail = s.slice(s.length - k);
    var fh = [lineFit(head, 'u'), lineFit(head, 'v')], ft = [lineFit(tail, 'u'), lineFit(tail, 'v')];
    var lim = function (x) { return Math.max(-10, Math.min(11, x)); }, j = 0;
    for (var i = start; i < start + count; i++) {
      if (i < s[0].idx) { U.push(lim(fh[0][0] + fh[0][1] * i)); V.push(lim(fh[1][0] + fh[1][1] * i)); continue; }
      if (i > s[s.length - 1].idx) { U.push(lim(ft[0][0] + ft[0][1] * i)); V.push(lim(ft[1][0] + ft[1][1] * i)); continue; }
      while (j < s.length - 2 && s[j + 1].idx < i) j++;
      var a = s[j], b = s[Math.min(j + 1, s.length - 1)], t = b.idx === a.idx ? 0 : (i - a.idx) / (b.idx - a.idx);
      U.push(a.u + (b.u - a.u) * t); V.push(a.v + (b.v - a.v) * t);
    }
    return { u: U, v: V };
  }
  function tableFromPositions(P, aspect) {
    var t = [];
    for (var i = 0; i < P.u.length; i++) t.push(uvToEntry(P.u[i], P.v[i], aspect));
    return t;
  }
  // LED indexes lined up with the TV's left and right edge for a strip that runs along one side of it:
  // the contiguous run of LEDs whose position along the TV is between 0 and 1. null if there is no such run.
  function tvSpan(P, start) {
    var first = -1, last = -1, i;
    for (i = 0; i < P.u.length; i++) {
      var inside = P.u[i] >= 0 && P.u[i] <= 1;
      if (inside) { if (first < 0) first = i; last = i; }
      else if (first >= 0) { for (var k = i; k < P.u.length; k++) if (P.u[k] >= 0 && P.u[k] <= 1) return null; break; }
    }
    return first < 0 ? null : [start + first, start + last];
  }
  function tableToHex(t) {
    var s = '';
    for (var i = 0; i < t.length; i++) for (var k = 0; k < 4; k++) { var v = t[i][k] | 0; s += (v < 16 ? '0' : '') + v.toString(16); }
    return s;
  }
  function tableFromHex(s, per) {
    per = per || 4;
    var t = [];
    for (var i = 0; i + per * 2 - 1 < s.length; i += per * 2) {
      var e = [];
      for (var k = 0; k < per; k++) e.push(parseInt(s.substr(i + k * 2, 2), 16));
      if (per === 3) e.push(0);
      t.push(e);
    }
    return t;
  }
  function edgeSummary(P) {
    var c = { top: 0, right: 0, bottom: 0, left: 0 };
    for (var i = 0; i < P.u.length; i++) c[edgeOf(P.u[i], P.v[i])]++;
    return c;
  }
  // Longest run of visible flags allowing gaps of up to `gap` invisible entries. Returns [first, last] or null.
  function longestRun(flags, gap) {
    var best = null, start = -1, last = -1, miss = 0;
    for (var i = 0; i <= flags.length; i++) {
      var on = i < flags.length && flags[i];
      if (on) { if (start < 0) start = i; last = i; miss = 0; }
      else if (start >= 0) {
        miss++;
        if (miss > gap || i === flags.length) {
          if (!best || last - start > best[1] - best[0]) best = [start, last];
          start = -1; miss = 0;
        }
      }
    }
    return best;
  }

  return {
    SQUARES: SQUARES, OUT_MAX: OUT_MAX, DEFAULT_ASPECT: DEFAULT_ASPECT,
    newImg: newImg, meanImgs: meanImgs, subImg: subImg, lerpImg: lerpImg, meanOf: meanOf, median: median, percentile: percentile,
    boxBlur: boxBlur, otsu: otsu, component: component, allComponents: allComponents,
    detectTV: detectTV, detectSquares: detectSquares, squareCentres: squareCentres, locateBlob: locateBlob,
    homography: homography, homographyFit: homographyFit, applyH: applyH, invertH: invertH, quadFromH: quadFromH,
    uvToEntry: uvToEntry, edgeOf: edgeOf, tableLinear: tableLinear, tableLoop: tableLoop, autoSides: autoSides,
    smoothInfo: smoothInfo, applyTable: applyTable,
    cleanSamples: cleanSamples, smoothSamples: smoothSamples, positionsFromSamples: positionsFromSamples,
    tableFromPositions: tableFromPositions, tvSpan: tvSpan, tableToHex: tableToHex, tableFromHex: tableFromHex,
    edgeSummary: edgeSummary, longestRun: longestRun, polyArea: polyArea
  };
});
