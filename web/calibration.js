/* Auto-calibration procedure. Runs in the phone's browser (live camera) and in Node (tests).
 *
 * env.api     device control (all return promises):
 *               prepare() -> {total}        turn WLED on / full brightness, remember what to restore
 *               restore()                   put WLED back
 *               led(start, count)           light exactly LEDs [start, start+count) white, all others black (count 0 = all off)
 *               release()                   stop driving the LEDs
 *               screen(mode)                'black' | 'squares' | 'white' | 'idle'   (the calibration screen shown on the TV)
 *               zones() -> {age, zones}|null   latest Sync Box frame (to check that the box can see the TV screen)
 * env.camera  grab(n, gapMs) -> Promise<image>   average of n camera frames (grey 0..255)
 * env.sleep   ms => Promise
 * env.progress(text, fraction)    UI callback
 * env.opts    { range: [start, count] | null, refine: true, ... timings }
 *
 * Steps: TV dark -> 9 squares (exact picture geometry) -> white (cross-check) -> find the visible LEDs -> map them in
 * blocks -> finer pass over the part next to the TV -> table.
 */
(function (root, factory) {
  if (typeof module === 'object' && module.exports) module.exports = factory(require('./analysis.js'));
  else root.FWBCal = factory(root.FWB);
})(typeof self !== 'undefined' ? self : this, function (A) {
  'use strict';

  function CalError(msg) { this.name = 'CalError'; this.message = msg; this.userMessage = msg; }
  CalError.prototype = Object.create(Error.prototype);

  async function runCalibration(env) {
    var api = env.api, cam = env.camera, sleep = env.sleep;
    var say = env.progress || function () {};
    var o = Object.assign({ settleMs: 650, frames: 4, frameGapMs: 90, coarseSteps: 32, denseSteps: 40, refine: true, refineMax: 40,
      tvSettleMs: 1800, darkSettleMs: 2500, minBlock: 2, gapBlocks: 2, aspect: A.DEFAULT_ASPECT }, env.opts || {});
    var warnings = [];
    var stats = { steps: 0 };
    var t0 = Date.now();
    var grab = function () { return cam.grab(o.frames, o.frameGapMs); };
    var lit = async function (start, count) {
      await api.led(start, count);
      await sleep(o.settleMs);
      stats.steps++;
      return grab();
    };

    var info = await api.prepare();
    var total = info.total;
    try {
      // 1. The TV: dark, then the nine squares, then white.
      say('Looking at the TV', 0.02);
      await api.led(0, 0);
      await api.screen('black');
      await sleep(o.darkSettleMs);
      var dark0 = await grab();
      if (A.meanOf(dark0) > 170) warnings.push('The room looks very bright. Dim the lights for a more reliable result.');
      say('Measuring the TV with squares', 0.04);
      await api.screen('squares');
      await sleep(o.tvSettleMs);
      var sq = await grab();
      say('Turning the TV white', 0.06);
      await api.screen('white');
      await sleep(o.tvSettleMs);
      var white = await grab();
      var box = null;
      try { box = await api.zones(); } catch (e) { /* box check is optional */ }
      await api.screen('black');

      var sqRes = A.detectSquares(sq, dark0), tv = A.detectTV(white, dark0), H, quad;
      if (sqRes.ok) {
        H = sqRes.H; quad = sqRes.quad;
        sqRes.warnings.forEach(function (w) { warnings.push(w); });
        if (tv.ok) {                                              // the white outline blooms, so it is only a sanity check
          var width = Math.hypot(quad[1][0] - quad[0][0], quad[1][1] - quad[0][1]), off = 0;
          for (var c = 0; c < 4; c++) off += Math.hypot(quad[c][0] - tv.corners[c][0], quad[c][1] - tv.corners[c][1]) / 4;
          if (off > 0.06 * width) warnings.push('The squares and the white outline of the TV disagree by ' + Math.round(100 * off / width) + '% of the TV width. I used the squares; check that nothing else bright is in view.');
        }
      } else if (tv.ok) {
        warnings.push('I could not use the squares (' + sqRes.reason + ') so I used the white outline of the TV, which is less exact.');
        tv.warnings.forEach(function (w) { warnings.push(w); });
        H = A.homography(tv.corners); quad = tv.corners;
      } else {
        throw new CalError(sqRes.reason);
      }
      if (!H) throw new CalError('Could not work out the shape of the TV from the picture. Try again from a position more in front of the TV.');
      if (box && box.age != null && box.age < 3000 && box.zones) {
        var m = 0;
        box.zones.forEach(function (z) { m += (z[0] + z[1] + z[2]) / 3; });
        m /= box.zones.length;
        if (m < 60) warnings.push('The Sync Box did not report a white picture while the TV was white. Check that the calibration screen is on the input the box is using.');
      }

      // 2. Baseline before the LED scan (after the camera has re-adapted to the dark).
      say('Measuring the dark room', 0.08);
      await sleep(o.darkSettleMs);
      var base1 = await grab();

      // 3. Find which LEDs the camera can see.
      var ds, de, order = [], probes = [];
      if (o.range) {
        ds = o.range[0]; de = o.range[0] + o.range[1];
      } else {
        var bs = Math.max(1, Math.ceil(total / o.coarseSteps)), blocks = [], flags = [], s, i;
        for (s = 0; s < total; s += bs) blocks.push({ start: s, count: Math.min(bs, total - s) });
        for (i = 0; i < blocks.length; i++) {
          say('Scanning the strip (' + (i + 1) + '/' + blocks.length + ')', 0.1 + 0.25 * (i / blocks.length));
          var im = await lit(blocks[i].start, blocks[i].count);
          flags.push(A.locateBlob(im, base1).visible);
          order.push(im);
        }
        var run = A.longestRun(flags, 1);
        if (!run) throw new CalError('I could not see any LEDs light up. Make sure the camera can see the LEDs (not just the TV), the room is dim, and WLED is receiving DDP.');
        ds = Math.max(0, blocks[run[0]].start - bs);
        de = Math.min(total, blocks[run[1]].start + blocks[run[1]].count + bs);
      }
      if (de - ds < 2) throw new CalError('The LED range is too small to calibrate.');
      var n = Math.max(o.minBlock, Math.ceil((de - ds) / o.denseSteps)), dense = [];
      for (var st = ds; st < de; st += n) dense.push({ start: st, count: Math.min(n, de - st) });
      for (var d = 0; d < dense.length; d++) {
        say('Mapping the LEDs (' + (d + 1) + '/' + dense.length + ')', 0.35 + 0.3 * (d / dense.length));
        var img = await lit(dense[d].start, dense[d].count);
        probes.push({ block: dense[d], img: img, order: order.length, dense: true });
        order.push(img);
      }

      // 3b. Finer pass next to the TV: probe blocks shifted by half a block, so LED positions there are twice as dense.
      if (o.refine) {
        var cand = [];
        probes.forEach(function (p) {
          var b = A.locateBlob(p.img, base1);
          if (!b.visible) return;
          var uv = A.applyH(H, b.x, b.y);
          if (uv[0] > -0.15 && uv[0] < 1.15) cand.push(p.block);
        });
        if (cand.length > o.refineMax) {
          var pick = [];
          for (var q = 0; q < o.refineMax; q++) pick.push(cand[Math.floor(q * cand.length / o.refineMax)]);
          cand = pick;
        }
        for (var r = 0; r < cand.length; r++) {
          var bl = { start: cand[r].start + Math.floor(cand[r].count / 2), count: cand[r].count };
          if (bl.start + bl.count > de) continue;
          say('Measuring the TV area more precisely (' + (r + 1) + '/' + cand.length + ')', 0.65 + 0.27 * (r / cand.length));
          var im2 = await lit(bl.start, bl.count);
          probes.push({ block: bl, img: im2, order: order.length, dense: false });
          order.push(im2);
        }
      }
      await api.led(0, 0);
      await sleep(o.settleMs);
      say('Finishing', 0.94);
      var base2 = await grab();
      var drift = Math.abs(A.meanOf(base2) - A.meanOf(base1));
      if (drift > 0.2 * Math.max(8, A.meanOf(base1))) warnings.push('The camera brightness changed during the scan (auto-exposure). Results may be a little off; keep the room lighting constant.');

      // 4. Positions of every probe, corrected for slow exposure drift.
      var G = order.length + 1, samples = [], flagsDense = [];
      probes.forEach(function (p) {
        var base = A.lerpImg(base1, base2, (p.order + 1) / G), b = A.locateBlob(p.img, base);
        p.blob = b;
        if (p.dense) flagsDense.push(b.visible);
        if (b.visible) {
          var uv = A.applyH(H, b.x, b.y);
          samples.push({ idx: p.block.start + (p.block.count - 1) / 2, x: b.x, y: b.y, u: uv[0], v: uv[1], peak: b.peak, touches: b.touches });
        }
      });
      var fs = ds, fe = de;
      if (!o.range) {
        var run2 = A.longestRun(flagsDense, o.gapBlocks);
        if (!run2) throw new CalError('I could not follow the LEDs along the wall. Check that the camera can see them and try again.');
        var dp = probes.filter(function (p) { return p.dense; });
        fs = dp[run2[0]].block.start;
        fe = dp[run2[1]].block.start + dp[run2[1]].block.count;
      }
      var used = samples.filter(function (s2) { return s2.idx >= fs - 1 && s2.idx <= fe; }).sort(function (a, b) { return a.idx - b.idx; });
      if (used.length < 4) throw new CalError('I could only see ' + used.length + ' groups of LEDs. Move the phone back so the TV and all the LEDs are in view, dim the room, and try again.');
      var touching = used.filter(function (s3) { return s3.touches; }).length;
      if (touching > 0.25 * used.length) warnings.push('Many LED glows were cut off by the edge of the picture. Move the phone back so all the LEDs fit in view.');

      var P = A.positionsFromSamples(used, fs, fe - fs);
      var table = A.tableFromPositions(P, o.aspect);
      var first = used[0].idx, last = used[used.length - 1].idx, extrap = 0;
      for (var li = fs; li < fe; li++) if (li < first - n / 2 || li > last + n / 2) extrap++;
      if (extrap > 0.1 * (fe - fs)) warnings.push(extrap + ' LEDs were outside the camera view; their positions were extrapolated along the line of the visible ones. This is fine for a straight strip.');
      var span = A.tvSpan(P, fs), dmax = 0;
      table.forEach(function (e) { if (e[3] > dmax) dmax = e[3]; });
      say('Done', 1);
      return {
        ok: true, start: fs, count: fe - fs, table: table, hex: A.tableToHex(table), samples: used, quad: quad,
        size: [white.w, white.h], warnings: warnings, summary: A.edgeSummary(P), tvSpan: span,
        beyond: { max: dmax, picturesWide: dmax / 255 * A.OUT_MAX },
        tv: { method: sqRes.ok ? 'squares' : 'white outline', rms: sqRes.ok ? sqRes.rms : null },
        squares: sqRes.ok ? sqRes.points.map(function (p) { return { x: p.x, y: p.y }; }) : [],
        stats: { steps: stats.steps, seconds: Math.round((Date.now() - t0) / 1000), blocks: probes.length, seen: samples.length }
      };
    } finally {
      try { await api.led(0, 0); } catch (e) { /* ignore */ }
      try { await api.release(); } catch (e) { /* ignore */ }
      try { await api.screen('idle'); } catch (e) { /* ignore */ }
      try { await api.restore(); } catch (e) { /* ignore */ }
    }
  }

  return { runCalibration: runCalibration, CalError: CalError };
});
