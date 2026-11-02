// Tests for web/analysis.js + web/calibration.js.   Run:  node test/test_analysis.js
'use strict';
const fs = require('fs');
const path = require('path');
const A = require('../web/analysis.js');
const Cal = require('../web/calibration.js');
const { Scene, makeEnv, entryDiff, compare } = require('./sim_camera.js');

let pass = 0, fail = 0;
function check(name, cond, extra) {
  if (cond) pass++; else { fail++; console.log('FAIL: ' + name + (extra !== undefined ? '  ' + extra : '')); }
}
async function expectCalError(name, p, re) {
  try { await p; check(name + ' (expected an error)', false); }
  catch (e) { check(name, e instanceof Cal.CalError && (!re || re.test(e.message)), e && e.message); }
}
const sameEntry = (a, b) => entryDiff(a, b) <= 0.012 && Math.abs(a[3] - b[3]) <= 2;

/* ---------------------------------------------------------------- golden vectors */
(function golden() {
  const lines = fs.readFileSync(path.join(__dirname, 'golden.txt'), 'utf8').split('\n');
  const parse = s => s.trim().split(/\s+/).map(t => t.split(',').map(Number));
  let uv = 0, lin = 0, loop = 0, app = 0;
  for (const ln of lines) {
    const p = ln.split(' ');
    if (p[0] === 'UV') {
      const e = A.uvToEntry(+p[1], +p[2]), w = p[3].split(',').map(Number);
      check('uv ' + p[1] + ',' + p[2] + ' got ' + e + ' want ' + w, sameEntry(e, w)); uv++;
    } else if (p[0] === 'LINEAR') {
      const [count, right, sf, sl, edge] = p.slice(1, 6).map(Number);
      const want = parse(p.slice(6).join(' ')), got = A.tableLinear(count, right === 1, sf, sl, edge);
      let bad = -1; for (let i = 0; i < count; i++) if (!sameEntry(got[i], want[i])) { bad = i; break; }
      check('linear ' + count + ' right=' + right + ' span ' + sf + ',' + sl + ' edge ' + edge, got.length === want.length && bad < 0, bad); lin++;
    } else if (p[0] === 'LOOP') {
      const [count, corner, ccw] = p.slice(1, 4).map(Number), sides = p[4] === 'auto' ? null : p[4].split(',').map(Number);
      const shift = +p[5], aw = +p[6], ah = +p[7], want = parse(p.slice(8).join(' '));
      const got = A.tableLoop(count, corner, ccw === 1, sides, shift, aw, ah);
      let bad = -1; if (got) for (let i = 0; i < count; i++) if (!sameEntry(got[i], want[i])) { bad = i; break; }
      check('loop ' + count + ' corner ' + corner + ' ccw ' + ccw + ' shift ' + shift, got && bad < 0, bad); loop++;
    } else if (p[0] === 'APPLY') {
      // APPLY count side first last percent lenStart lenEnd zones | idx:r,g,b ...
      const count = +p[1], sf = +p[3], sl = +p[4], pct = +p[5], qs = +p[6], qe = +p[7];
      const zl = p[8].split(',').map(Number), zones = []; for (let z = 0; z < 24; z++) zones.push(zl.slice(z * 3, z * 3 + 3));
      const tbl = A.tableLinear(count, p[2] === 'right', sf, sl), si = A.smoothInfo(tbl, pct);
      check('smoothInfo pct=' + pct, si.lenStart === qs && si.lenEnd === qe, si.lenStart + ',' + si.lenEnd + ' vs ' + qs + ',' + qe);
      const cols = A.applyTable(tbl, zones, si), tail = ln.split('|')[1].trim().split(/\s+/);
      let worst = 0;
      for (const t of tail) { const [i, rgb] = t.split(':'), want = rgb.split(',').map(Number); for (let c = 0; c < 3; c++) worst = Math.max(worst, Math.abs(cols[+i][c] - want[c])); }
      check('apply pct=' + pct + ' matches the reference (worst ' + worst.toFixed(2) + ')', worst < 0.7); app++;
    }
  }
  console.log('golden: ' + uv + ' uv, ' + lin + ' linear, ' + loop + ' loop, ' + app + ' apply vectors');
})();

/* -------------------------------------------------------------- building blocks */
(function units() {
  const H = A.homography([[10, 20], [110, 20], [110, 70], [10, 70]]);
  const m = A.applyH(H, 60, 45);
  check('homography maps centre to (0.5,0.5)', Math.abs(m[0] - 0.5) < 1e-6 && Math.abs(m[1] - 0.5) < 1e-6, m);
  const o = A.applyH(H, 10, 20), q = A.applyH(H, 110, 70);
  check('homography corners', Math.abs(o[0]) < 1e-6 && Math.abs(q[0] - 1) < 1e-6 && Math.abs(q[1] - 1) < 1e-6);
  const inv = A.invertH(H), back = A.applyH(inv, 0.5, 0.5);
  check('invertH round trip', Math.abs(back[0] - 60) < 1e-6 && Math.abs(back[1] - 45) < 1e-6, back);
  const noisy = []; for (const c of A.squareCentres()) { const p = A.applyH(inv, c[0], c[1]); noisy.push({ x: p[0] + 0.05 * Math.sin(c[0] * 99), y: p[1] + 0.05 * Math.cos(c[1] * 99), u: c[0], v: c[1] }); }
  const Hf = A.homographyFit(noisy), r = A.applyH(Hf, 110, 70);
  check('homographyFit from 9 noisy points', Math.abs(r[0] - 1) < 0.01 && Math.abs(r[1] - 1) < 0.01, r);
  const hex = A.tableToHex([[0, 5, 255, 0], [17, 23, 3, 200]]);
  check('table hex round trip (4 bytes per LED)', hex === '0005ff00111703c8' && JSON.stringify(A.tableFromHex(hex)) === '[[0,5,255,0],[17,23,3,200]]', hex);
  check('legacy 3-byte tables still load', JSON.stringify(A.tableFromHex('0005ff', 3)) === '[[0,5,255,0]]');
  check('longestRun', JSON.stringify(A.longestRun([0, 1, 1, 0, 1, 1, 1, 0, 0, 0, 1], 1)) === '[1,6]' && A.longestRun([0, 0], 1) === null);
  const img = A.newImg(8, 8); img.d.fill(10); img.d[27] = 90;
  check('boxBlur keeps energy locally', Math.abs(A.boxBlur(img, 1).d[27] - (10 * 8 + 90) / 9) < 1e-4);
  // the calibration screen draws the same squares the analysis expects
  const scr = fs.readFileSync(path.join(__dirname, '..', 'web', 'screen.html'), 'utf8');
  const m2 = scr.match(/var SQ=\{us:\[([^\]]*)\],vs:\[([^\]]*)\],side:([0-9.]+)\}/);
  check('screen.html squares match analysis.js', m2 && JSON.stringify(m2[1].split(',').map(Number)) === JSON.stringify(A.SQUARES.us) &&
    JSON.stringify(m2[2].split(',').map(Number)) === JSON.stringify(A.SQUARES.vs) && +m2[3] === A.SQUARES.side, m2 && m2[0]);
  // smoothing beyond the TV: distance byte
  const t = A.tableLinear(210, false, 91, 174);
  check('no distance over the TV, growing beyond it', t[91][3] === 0 && t[130][3] === 0 && t[174][3] === 0 && t[0][3] > t[60][3] && t[209][3] > t[180][3] && t[0][3] > 0);
  const z = []; for (let i = 0; i < 24; i++) z.push([i % 6 === 0 ? 255 : 0, 0, 0]);
  const avgR = z.reduce((s, c) => s + c[0], 0) / 24, cols = A.applyTable(t, z, A.smoothInfo(t, 100));
  check('over the TV: exact edge colour; strip end reaches the average', Math.abs(cols[91][0] - 255) < 1 && Math.abs(cols[0][0] - 255) < 2 && Math.abs(cols[209][0]) < 2, cols[91][0] + ' ' + cols[0][0]);
  check('smoothing off: the edge colour just repeats', Math.abs(A.applyTable(t, z, A.smoothInfo(t, 0))[0][0] - 255) < 1);
  // a strip on the ceiling only ever sees the top row: a green dot in the bottom-left corner (zone 18) must not tint it
  const tc = A.tableLinear(210, false, 82, 172, 0), sic = A.smoothInfo(tc, 100), zg = []; for (let i = 0; i < 24; i++) zg.push([0, 0, 0]);
  zg[18] = [51, 87, 13];
  const cg = A.applyTable(tc, zg, sic); let brightest = 0; for (const c of cg) brightest = Math.max(brightest, c[0], c[1], c[2]);
  check('a top strip: left tail -> top-left zone, right tail -> top-right zone', JSON.stringify(sic.zonesStart) === '[0]' && JSON.stringify(sic.zonesEnd) === '[5]', JSON.stringify(sic));
  check('a green dot at the bottom-left leaves the ceiling strip dark', brightest < 0.5, brightest);
  // an orange dot in the top-left corner: the left tail shows it, the right tail stays dark
  const zo = []; for (let i = 0; i < 24; i++) zo.push([0, 0, 0]); zo[0] = [255, 120, 0];
  const co = A.applyTable(tc, zo, sic); let rbright = 0; for (let i = 173; i < 210; i++) rbright = Math.max(rbright, co[i][0], co[i][1], co[i][2]);
  check('orange top-left dot: left tail carries it', co[0][0] > 20 && co[81][0] > 100, co[0][0] + ' ' + co[81][0]);
  check('orange top-left dot: right tail stays dark', rbright < 0.5, rbright);
  // a blue dot near the top right middle (zone 3) or next to the corner (zone 4) must not reach the tails; only the corner zone does
  function tailsBlue(zone) { const zz = []; for (let i = 0; i < 24; i++) zz.push([0, 0, 0]); zz[zone] = [0, 0, 255];
    const cc = A.applyTable(tc, zz, sic); let m = 0; for (let i = 0; i < 210; i++) if (i < 80 || i > 172) m = Math.max(m, cc[i][2]); return { m: m, tv: cc[140][2], last: cc[209][2] }; }
  const b3 = tailsBlue(3), b4 = tailsBlue(4), b5 = tailsBlue(5);
  check('blue dot in zone 3: tails stay dark, TV part lights', b3.m < 0.5 && b3.tv > 100, JSON.stringify(b3));
  check('blue dot in zone 4: tails stay dark', b4.m < 0.5, JSON.stringify(b4));
  check('blue dot in the corner zone 5: the right tail lights', b5.last > 250, JSON.stringify(b5));  const tb = A.tableLinear(210, false, 82, 172, 2), sib = A.smoothInfo(tb, 100);
  check('a strip under the TV uses its corner zones', JSON.stringify(sib.zonesStart) === '[18]' && JSON.stringify(sib.zonesEnd) === '[23]', JSON.stringify(sib));
  const sir = A.smoothInfo(A.tableLinear(210, true, 82, 172, 0), 100);
  check('LED 0 on the right: the first tail uses the top-right corner', JSON.stringify(sir.zonesStart) === '[5]' && JSON.stringify(sir.zonesEnd) === '[0]', JSON.stringify(sir));
})();

/* ------------------------------------------------------------------ scenarios */
function tvQuad() { return [[62, 46], [132, 44], [134, 90], [60, 92]]; }       // slightly rotated / keystoned TV

function truthFromGeometry(tv, ledPos, lo, hi) {   // ground-truth entry for LED i from true image positions (clamped to the real LED range)
  const H = A.homography(tv);
  return function (i) { const p = ledPos(Math.min(hi, Math.max(lo, i))); const uv = A.applyH(H, p[0], p[1]); return A.uvToEntry(uv[0], uv[1]); };
}
function truthSpan(tv, ledPos, lo, hi) {           // first / last LED whose true position is within the TV's width
  const H = A.homography(tv); let first = -1, last = -1;
  for (let i = lo; i <= hi; i++) { const uv = A.applyH(H, ledPos(i)[0], ledPos(i)[1]); if (uv[0] >= 0 && uv[0] <= 1) { if (first < 0) first = i; last = i; } }
  return [first, last];
}

async function scenario(name, scene, opts, truthFn, expect) {
  const { env, log } = makeEnv(scene, { opts: opts });
  let res;
  try { res = await Cal.runCalibration(env); }
  catch (e) { check(name + ': no exception (' + e.message + ')', false); return null; }
  const c = compare(res, truthFn);
  check(name + ': mean entry error ' + c.mean.toFixed(3) + ' <= ' + expect.mean, c.mean <= expect.mean);
  check(name + ': worst entry error ' + c.max.toFixed(2) + ' <= ' + expect.max, c.max <= expect.max);
  check(name + ': start ' + res.start + ' ~ ' + expect.start, Math.abs(res.start - expect.start) <= (expect.tol || 8));
  check(name + ': count ' + res.count + ' ~ ' + expect.count, Math.abs(res.count - expect.count) <= (expect.tol || 8));
  check(name + ': WLED restored and LEDs released', log.restored && log.released);
  check(name + ': squares measured before the white flash', log.screens.indexOf('squares') >= 0 && log.screens.indexOf('squares') < log.screens.indexOf('white'), log.screens.join(','));
  console.log('  ' + name + ': start ' + res.start + ' count ' + res.count + ' | mean err ' + c.mean.toFixed(3) + ' max ' + c.max.toFixed(2) +
    ' | ' + res.stats.steps + ' steps | TV span ' + JSON.stringify(res.tvSpan) + ' | via ' + res.tv.method + (res.warnings.length ? ' | warnings: ' + res.warnings.length : ''));
  return res;
}

(async function main() {
  // A. straight run of 210 LEDs above the TV, rest of the 600-LED strip out of view; glare around the bright screen
  {
    const tv = tvQuad();
    const ledPos = i => i < 210 ? [8 + (i / 209) * 176, 24 + 2 * Math.sin(i / 40)] : null;   // slightly wavy line, 0.84 px / LED
    const scene = new Scene({ tv, ledPos, seed: 11 });
    const res = await scenario('A linear run (auto range)', scene, null, truthFromGeometry(tv, ledPos, 0, 209), { mean: 0.06, max: 0.3, start: 0, count: 210 });
    if (res) {
      const ts = truthSpan(tv, ledPos, 0, 209);
      check('A: the LEDs lined up with the TV edges are found to within 1 LED (got ' + res.tvSpan + ', true ' + ts + ')',
        res.tvSpan && Math.abs(res.tvSpan[0] - ts[0]) <= 1 && Math.abs(res.tvSpan[1] - ts[1]) <= 1);
      const outsideL = res.table.filter((e, i) => res.start + i < ts[0] - 2), inside = res.table.filter((e, i) => res.start + i >= ts[0] + 1 && res.start + i <= ts[1] - 1);
      check('A: exact (no smoothing distance) along the TV, smoothing distance beyond it', inside.every(e => e[3] <= 2) && outsideL.every(e => e[3] > 0) && outsideL[0][3] > outsideL[outsideL.length - 1][3]);
      const zones = []; for (let z = 0; z < 24; z++) zones.push([z % 6 === 0 ? 255 : 0, 40, z % 6 === 5 ? 255 : 0]);
      const cols = A.applyTable(res.table, zones, A.smoothInfo(res.table, 100));
      const avg = 255;     // each tail ends on its corner zone: red at the left, blue at the right
      check('A: the far end of the strip reaches the average colour', Math.abs(cols[0][0] - avg) < 20 && Math.abs(cols[res.count - 1][2] - avg) < 20, cols[0][0] + ' / ' + cols[res.count - 1][2] + ' vs ' + avg);
    }
  }
  // A2. the squares beat the white outline when the screen glares
  {
    const tv = tvQuad(), ledPos = i => i < 10 ? [20 + i, 24] : null;
    const scene = new Scene({ tv, ledPos, seed: 21, bloom: 0.6, bloomRadius: 4 });
    const { env } = makeEnv(scene);
    scene.screen = 'black'; const dark = await env.camera.grab(6, 90);
    scene.screen = 'squares'; const sqImg = await env.camera.grab(6, 90);
    scene.screen = 'white'; const whiteImg = await env.camera.grab(6, 90);
    const sq = A.detectSquares(sqImg, dark), wh = A.detectTV(whiteImg, dark);
    check('A2: squares found', sq.ok, sq.reason);
    if (sq.ok && wh.ok) {
      const err = q => q.reduce((s, c, i) => s + Math.hypot(c[0] - tv[i][0], c[1] - tv[i][1]), 0) / 4;
      const eS = err(sq.quad), eW = err(wh.corners);
      console.log('  A2: TV corner error with glare: squares ' + eS.toFixed(2) + ' px, white outline ' + eW.toFixed(2) + ' px, squares fit error ' + (sq.rms * 100).toFixed(2) + '% of the width');
      check('A2: squares locate the TV corners within 0.7 px', eS < 0.7, eS);
      check('A2: squares are more accurate than the white outline', eS < eW, eS + ' vs ' + eW);
    }
  }
  // B. run is wider than the camera view; user supplies the range, ends get extrapolated
  {
    const tv = tvQuad();
    const ledPos = i => i < 210 ? [-30 + (i / 209) * 250, 26] : null;                         // x from -30 .. 220 (image is 192 wide)
    const scene = new Scene({ tv, ledPos, seed: 12 });
    const res = await scenario('B run wider than view (range given)', scene, { range: [0, 210] }, truthFromGeometry(tv, ledPos, 0, 209),
      { mean: 0.1, max: 0.5, start: 0, count: 210, tol: 0 });
    if (res) {
      check('B warns about extrapolation', res.warnings.some(w => /outside the camera view/.test(w)), JSON.stringify(res.warnings));
      const ts = truthSpan(tv, ledPos, 0, 209);
      check('B: TV edge LEDs within 1.5 LEDs (got ' + res.tvSpan + ', true ' + ts + ')', res.tvSpan && Math.abs(res.tvSpan[0] - ts[0]) <= 1.5 && Math.abs(res.tvSpan[1] - ts[1]) <= 1.5);
    }
  }
  // C. strip around the TV: 142 LEDs clockwise starting at the top-left, LEDs 142..599 out of view
  {
    const tv = [[60, 40], [140, 40], [140, 88], [60, 88]];
    const pad = 9, x0 = 60 - pad, x1 = 140 + pad, y0 = 40 - pad, y1 = 88 + pad;
    const per = 2 * (x1 - x0) + 2 * (y1 - y0);
    const ledPos = i => {
      if (i >= 142) return null;
      let d = ((i + 0.5) / 142) * per;                                                       // clockwise from the top-left
      if (d < x1 - x0) return [x0 + d, y0]; d -= x1 - x0;
      if (d < y1 - y0) return [x1, y0 + d]; d -= y1 - y0;
      if (d < x1 - x0) return [x1 - d, y1]; d -= x1 - x0;
      return [x0, y1 - d];
    };
    const scene = new Scene({ tv, ledPos, seed: 13, glowSigma: 2.6, glowAmp: 70 });
    const res = await scenario('C loop around the TV', scene, null, truthFromGeometry(tv, ledPos, 0, 141), { mean: 0.2, max: 0.8, start: 0, count: 142, tol: 10 });
    if (res) {
      check('C finds all four sides', res.summary.top > 20 && res.summary.right > 10 && res.summary.bottom > 20 && res.summary.left > 10, JSON.stringify(res.summary));
      check('C: a loop hugging the TV is not smoothed away (max distance small)', res.beyond.picturesWide < 0.2, res.beyond.picturesWide);
    }
  }
  // D. strip starts part-way along the WLED device (LEDs 100..259 are the TV run)
  {
    const tv = tvQuad();
    const ledPos = i => (i >= 100 && i < 260) ? [10 + ((i - 100) / 159) * 172, 25] : null;
    const scene = new Scene({ tv, ledPos, seed: 14 });
    await scenario('D run that starts at LED 100', scene, null, truthFromGeometry(tv, ledPos, 100, 259), { mean: 0.07, max: 0.35, start: 100, count: 160 });
  }
  // E. dim LEDs and a noisy camera
  {
    const tv = tvQuad();
    const ledPos = i => i < 150 ? [14 + (i / 149) * 164, 27] : null;
    const scene = new Scene({ tv, ledPos, seed: 15, glowAmp: 16, noise: 3.5 });
    await scenario('E dim LEDs, noisy camera', scene, null, truthFromGeometry(tv, ledPos, 0, 149), { mean: 0.1, max: 0.5, start: 0, count: 150, tol: 12 });
  }
  // G. the screen cannot show the squares (e.g. old calibration screen): fall back to the white outline, with a warning
  {
    const tv = tvQuad();
    const ledPos = i => i < 150 ? [14 + (i / 149) * 164, 27] : null;
    const scene = new Scene({ tv, ledPos, seed: 16, squares: false });
    const res = await scenario('G squares unavailable -> white outline', scene, null, truthFromGeometry(tv, ledPos, 0, 149), { mean: 0.2, max: 0.8, start: 0, count: 150, tol: 12 });
    if (res) check('G: warns that the white outline is less exact', res.warnings.some(w => /white outline/.test(w)) && res.tv.method === 'white outline', JSON.stringify(res.warnings));
  }
  // F. failure modes give useful messages
  {
    const tv = tvQuad();
    const ledPos = i => i < 100 ? [20 + i, 25] : null;
    await expectCalError('F1 TV never changes', Cal.runCalibration(makeEnv(new Scene({ tv, ledPos, tvOn: 4, tvOff: 4, seed: 16 })).env), /did not see|too small|could not/);
    await expectCalError('F2 no LEDs visible', Cal.runCalibration(makeEnv(new Scene({ tv, ledPos: () => null, seed: 17 })).env), /could not see any LEDs/);
    await expectCalError('F3 almost no LEDs in view', Cal.runCalibration(makeEnv(new Scene({ tv, ledPos: i => (i < 3 ? [20 + 10 * i, 25] : null), seed: 18 }), { opts: { range: [0, 40] } }).env), /could only see|could not follow/);
  }

  console.log('\n' + pass + ' checks passed, ' + fail + ' failed');
  console.log(fail ? 'SOME FAILED' : 'ALL PASSED');
  process.exit(fail ? 1 : 0);
})();
