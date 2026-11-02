// Synthetic room for testing the calibration: a (perspective-skewed) TV, glowing LEDs, noise, auto-exposure drift,
// and glare ("bloom") around bright areas of the screen.
'use strict';
const A = require('../web/analysis.js');

function rng(seed) {                         // mulberry32
  let a = seed >>> 0;
  return function () {
    a = (a + 0x6D2B79F5) >>> 0;
    let t = a;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

class Scene {
  /* o: w,h, tv:[tl,tr,br,bl] image coords, total LEDs, ledPos(i)->[x,y]|null (image coords),
        glowSigma, glowAmp, ambient, noise, tvOn, tvOff, bloom (glare strength, 0 = none), bloomRadius,
        ae (enable auto-exposure drift), squares (does the calibration screen draw the squares?), seed */
  constructor(o) {
    this.o = Object.assign({ w: 192, h: 108, total: 600, glowSigma: 3, glowAmp: 60, ambient: 22, noise: 2, tvOn: 210, tvOff: 4,
      bloom: 0.5, bloomRadius: 3, ae: true, squares: true, seed: 1 }, o);
    this.rand = rng(this.o.seed);
    this.screen = 'idle';
    this.lit = { start: 0, count: 0 };
    this.gain = 1;
    this.time = 0;
    this.renders = 0;
    this.H = A.homography(this.o.tv);          // true image -> picture mapping
    const w = this.o.w, h = this.o.h;
    this.tex = new Float32Array(w * h);        // fixed random "wall texture"
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++)
      this.tex[y * w + x] = 5 * Math.sin(x / 11 + 1) * Math.cos(y / 9) + 3 * Math.sin((x + y) / 23);
  }
  gauss() { const u = Math.max(1e-9, this.rand()), v = this.rand(); return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v); }
  advance(ms) {                              // auto-exposure: first-order lag towards a target that depends on the scene
    this.time += ms;
    if (!this.o.ae) return;
    const target = this.screen === 'white' ? 0.7 : 1.0;
    const steps = Math.max(1, Math.round(ms / 100));
    for (let i = 0; i < steps; i++) this.gain += (target - this.gain) * 0.18;
  }
  inTV(x, y) {
    const q = this.o.tv; let sign = 0;
    for (let i = 0; i < 4; i++) {
      const a = q[i], b = q[(i + 1) % 4], cr = (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0]);
      if (cr !== 0) { if (sign && Math.sign(cr) !== sign) return false; sign = Math.sign(cr); }
    }
    return true;
  }
  inSquare(u, v) {                           // squares are square on the screen: side is a fraction of the picture width
    const hs = A.SQUARES.side / 2, hv = hs * 16 / 9;
    for (const vc of A.SQUARES.vs) for (const uc of A.SQUARES.us) if (Math.abs(u - uc) <= hs && Math.abs(v - vc) <= hv) return true;
    return false;
  }
  render() {
    const o = this.o, w = o.w, h = o.h, img = A.newImg(w, h), bright = A.newImg(w, h);
    this.renders++;
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
      let v = o.ambient + this.tex[y * w + x];
      if (this.inTV(x + 0.5, y + 0.5)) {
        let on = this.screen === 'white';
        if (this.screen === 'squares' && o.squares) { const uv = A.applyH(this.H, x + 0.5, y + 0.5); on = this.inSquare(uv[0], uv[1]); }
        v = on ? o.tvOn : o.tvOff;
        if (on) bright.d[y * w + x] = o.tvOn;
      } else if (this.screen === 'white') v += 6;           // light spill on the wall
      img.d[y * w + x] = v;
    }
    if (o.bloom > 0 && (this.screen === 'white' || this.screen === 'squares')) {   // glare spreads symmetrically around bright areas
      const halo = A.boxBlur(A.boxBlur(bright, o.bloomRadius), o.bloomRadius);
      for (let i = 0; i < img.d.length; i++) img.d[i] += o.bloom * halo.d[i];
    }
    const s = o.glowSigma, r = Math.ceil(4 * s);
    for (let i = this.lit.start; i < this.lit.start + this.lit.count; i++) {
      const p = o.ledPos(i);
      if (!p) continue;
      for (let y = Math.max(0, Math.floor(p[1] - r)); y <= Math.min(h - 1, Math.ceil(p[1] + r)); y++)
        for (let x = Math.max(0, Math.floor(p[0] - r)); x <= Math.min(w - 1, Math.ceil(p[0] + r)); x++) {
          const dx = x + 0.5 - p[0], dy = y + 0.5 - p[1];
          img.d[y * w + x] += o.glowAmp * Math.exp(-(dx * dx + dy * dy) / (2 * s * s));
        }
    }
    for (let i = 0; i < img.d.length; i++) img.d[i] = Math.min(255, Math.max(0, img.d[i] * this.gain + o.noise * this.gauss()));
    return img;
  }
}

function makeEnv(scene, extra) {
  const log = { restored: false, released: false, calls: 0, screens: [] };
  const api = {
    prepare: async () => ({ total: scene.o.total }),
    restore: async () => { log.restored = true; },
    release: async () => { log.released = true; },
    led: async (s, c) => { log.calls++; scene.lit = { start: s, count: c }; },
    screen: async (m) => { scene.screen = m; log.screens.push(m); },
    zones: async () => ({ age: 50, zones: Array.from({ length: 24 }, () => (scene.screen === 'white' ? [200, 200, 200] : [0, 0, 0])) })
  };
  const camera = {
    grab: async (n, gap) => {
      const fr = [];
      for (let i = 0; i < n; i++) { scene.advance(gap); fr.push(scene.render()); }
      return A.meanImgs(fr);
    }
  };
  return {
    log: log,
    env: Object.assign({ api: api, camera: camera, sleep: async (ms) => scene.advance(ms), progress: () => {} }, extra || {})
  };
}

// Entry weights for comparing tables irrespective of how a blend is written.
function weights(e) { const w = new Float32Array(24); w[e[0]] += 1 - e[2] / 255; w[e[1]] += e[2] / 255; return w; }
function entryDiff(a, b) { const x = weights(a), y = weights(b); let m = 0; for (let i = 0; i < 24; i++) m = Math.max(m, Math.abs(x[i] - y[i])); return m; }

// Compare a calibration result with the truth. truthEntry(i) -> entry for LED i.
function compare(res, truthEntry) {
  let sum = 0, max = 0, n = 0, dsum = 0;
  for (let k = 0; k < res.count; k++) {
    const t = truthEntry(res.start + k), d = entryDiff(res.table[k], t);
    sum += d; n++; dsum += Math.abs(res.table[k][3] - t[3]); if (d > max) max = d;
  }
  return { mean: sum / n, max: max, n: n, dMean: dsum / n };
}

module.exports = { Scene, makeEnv, rng, weights, entryDiff, compare };
