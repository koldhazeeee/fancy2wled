// A fake relay for developing and testing the web UI without hardware.
//   node test/mock_device.js [port]      then open http://localhost:8099/
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');

const WEB = path.join(__dirname, '..', 'web');
const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'application/javascript', '.css': 'text/css' };
const ROUTES = { '/': 'index.html', '/calibrate': 'calibrate.html', '/screen': 'screen.html', '/analysis.js': 'analysis.js', '/calibration.js': 'calibration.js', '/style.css': 'style.css' };

const A = require('../web/analysis.js');       // the mapping code that is checked against the Python reference and the firmware

// How many of a target's LEDs follow each of the 24 zones (what the firmware's zoneUsage() reports).
function usageOf(wled, layout) {
  const n = wled.count | 0, out = new Array(24).fill(0);
  if (n <= 0 || n > 1000) return out;
  let tbl = null;
  if (layout.type === 'loop') tbl = A.tableLoop(n, layout.corner | 0, !!layout.ccw, (layout.sides && layout.sides[0] > 0) ? layout.sides : null, layout.shift | 0, 16, 9);
  if (!tbl) tbl = A.tableLinear(n, !!layout.led0Right, layout.spanFirst, layout.spanLast, layout.edge || 'top');
  tbl.forEach(e => { out[e[0]] += 1 - e[2] / 255; out[e[1]] += e[2] / 255; });
  return out.map(v => Math.round(v * 10) / 10);
}

function newTarget(i) {
  const t = { name: 'Target ' + (i + 1), enabled: true, trim: 1,
    wled: { host: '', start: 0, count: 0, turnOn: true, restore: true, brightness: -1 },
    layout: { type: 'linear', edge: 'top', led0Right: false, spanFirst: -1, spanLast: -1, corner: 0, ccw: false, sides: [0, 0, 0, 0], shift: 0, hasTable: false } };
  Object.defineProperty(t, 'usage', { enumerable: true, get() { return usageOf(this.wled, this.layout); } });   // like the firmware, derived from the settings
  return t;
}

function createDevice(opts) {
  opts = opts || {};
  const calls = [];
  const state = {
    cfg: { targets: [newTarget(0)],
      look: { brightness: 1, gamma: 1, saturation: 1, smoothing: 0.5, boost: 0, smoothOut: 100 }, box: { host: '' },
      ota: { enabled: false, passSet: false },
      relay: { enabled: true, tvOff: { mode: 'handback', seconds: 15 } }, mqtt: { enabled: false, host: '', port: 1883, user: '', passSet: false, base: '', discovery: 'homeassistant', perf: false } },
    screen: 'idle', calActive: false, maps: {}, test: null, total: opts.total || 600
  };
  // like the firmware, the first target is also available under its old names (wled / layout)
  const alias = () => { state.cfg.wled = state.cfg.targets[0].wled; state.cfg.layout = state.cfg.targets[0].layout; };
  alias();
  const json = (res, code, obj) => { const s = JSON.stringify(obj); res.writeHead(code, { 'Content-Type': 'application/json', 'Content-Length': Buffer.byteLength(s) }); res.end(s); };
  const server = http.createServer((req, res) => {
    const url = new URL(req.url, 'http://x');
    const p = url.pathname;
    if (req.method === 'GET' && ROUTES[p]) {
      const f = path.join(WEB, ROUTES[p]);
      res.writeHead(200, { 'Content-Type': TYPES[path.extname(f)] });
      return res.end(fs.readFileSync(f));
    }
    if (req.method === 'GET') {
      calls.push('GET ' + p);
      if (p === '/api/status') {
        // a name points to some address (192.0.2.51) unless the test says it was not found yet; an IP address is its own address
        const addrOf = h => !h ? '' : (/^\d+\.\d+\.\d+\.\d+$/.test(h) ? h : (state.nameNotFound ? '' : '192.0.2.51'));
        const targets = state.cfg.targets.map(t => ({ name: t.name, host: t.wled.host, ip: addrOf(t.wled.host), ipAge: addrOf(t.wled.host) ? 7 : -1, enabled: t.enabled, ok: !!t.wled.host, leds: state.total, wledName: 'WLED' }));
        return json(res, 200, { ver: 'mock', mode: 'sta', ssid: 'TestNet', ip: '192.0.2.10', hostname: 'fancy2wled', rssi: -50, uptime: 5, state: 'waiting for the Sync Box', https: false, heap: 1e5,
          enabled: state.cfg.relay.enabled, mqtt: { enabled: state.cfg.mqtt.enabled, connected: state.cfg.mqtt.enabled && !!state.cfg.mqtt.host, error: '', base: 'fancy2wled/abcd' },
          features: { phoneCal: opts.phoneCal !== false },
          box: { fps: state.boxProblem ? 0 : 13.4, age: 50, sender: '192.0.2.20', problem: !!state.boxProblem, tvOff: !!state.tvOff, blackS: state.tvOff ? 31 : (state.blackS || 0), frames: { understood: 1200, newLayout: state.boxProblem ? 340 : 0, unreadable: 2 } }, targets, wled: { host: targets[0].host, ok: targets[0].ok, leds: state.total, name: 'WLED' } });
      }
      if (p === '/api/config') return json(res, 200, state.cfg);
      if (p === '/api/diag') return json(res, 200, { format: 'fancy2wled-diagnostics', version: 1, firmware: 'mock', relay: { frames: { understood: 1200, newLayout: 340, unreadable: 2 }, samples: [{ ageMs: 100, class: 'sync box framing, payload not understood', len: 90, checksumOk: true, hex: '55aa5a55' }] } });
      if (p === '/api/zones') return json(res, 200, { age: 40, zones: Array.from({ length: 24 }, () => [state.screen === 'white' ? 200 : 0, 0, 0]) });
      if (p === '/api/screen') return json(res, 200, { mode: state.calActive ? state.screen : 'idle' });
      if (p === '/api/wled/discover') return json(res, 200, { devices: [{ name: 'wled-test', ip: '192.0.2.50', host: 'wled-test.local' }] });
      if (p === '/api/wled/info') return json(res, 200, { name: 'WLED', ver: '0.15', leds: state.total, rgbw: false, on: true, bri: 128 });
      if (p === '/api/wifi/scan') return json(res, 200, { networks: ['TestNet', 'Other'] });
      if (p === '/api/ota') return json(res, 200, { enabled: !!(state.otaOn && state.otaPass), passSet: !!state.otaPass, version: 'mock', slot: 'app0', imageState: state.imagePending ? 'pending' : 'valid', maxBytes: 3342336, rolledBack: !!state.rolledBack, busy: false });
      res.writeHead(404); return res.end('nope');
    }
    if (req.method === 'POST' && p === '/api/ota') {       // the firmware upload: a raw body and the update password in a header, like the board
      const chunks = [];
      req.on('data', c => chunks.push(c));
      req.on('end', () => {
        const body = Buffer.concat(chunks);
        calls.push('POST /api/ota');
        state.lastUpload = { len: body.length, password: req.headers['x-update-password'], type: req.headers['content-type'] };
        if (!(state.otaOn && state.otaPass)) return json(res, 403, { error: 'Firmware updates are switched off. Set an update password and switch updates on in the setup page first.' });
        if (req.headers['x-update-password'] === undefined) return json(res, 401, { error: 'Enter the update password.' });
        if (req.headers['x-update-password'] !== state.otaPass) return json(res, 401, { error: 'Wrong update password.' });
        if (state.imagePending) return json(res, 400, { error: 'The firmware that is running has not been confirmed yet (a new firmware is checked for about a minute after it starts, so that a bad one can be undone). Try again in 30 seconds.' });
        if (body.length < 112) return json(res, 400, { error: 'That file is too small to be a firmware image.' });
        if (body[0] !== 0xE9) return json(res, 400, { error: 'That is not a firmware image (use firmware.bin from .pio/build/atoms3, not a merged or bootloader file).' });
        state.uploaded = body.length;
        return json(res, 200, { ok: true, restarting: true });
      });
      return;
    }
    if (req.method === 'POST') {
      if (!/application\/json/.test(req.headers['content-type'] || '')) return json(res, 400, { error: 'Content-Type must be application/json' });
      let body = '';
      req.on('data', c => body += c);
      req.on('end', () => {
        let d = {}; try { d = JSON.parse(body || '{}'); } catch (e) { return json(res, 400, { error: 'Invalid JSON' }); }
        calls.push('POST ' + p + (p.startsWith('/api/cal/') ? ' ' + body : ''));
        if (p === '/api/config') {
          state.lastConfigPost = d;
          const T = state.cfg.targets[d.target || 0];
          if (!T) return json(res, 400, { error: 'There is no target ' + ((d.target || 0) + 1) });
          if (d.name !== undefined) T.name = d.name;
          if (d.enabled !== undefined) T.enabled = d.enabled;
          if (d.trim !== undefined) T.trim = d.trim;
          if (d.wled) {
            if (typeof d.wled.host === 'string') {   // like the board: a pasted URL is cleaned up ("HTTP://Wled-Test.local/" -> "wled-test.local")
              d = Object.assign({}, d, { wled: Object.assign({}, d.wled, { host: d.wled.host.trim().replace(/^https?:\/\//i, '').split(/[\/?#]/)[0].replace(/:80$/, '').replace(/\.+$/, '').toLowerCase() }) });
              if (!/^[A-Za-z0-9._-]*$/.test(d.wled.host)) return json(res, 400, { error: 'WLED address may only contain letters, digits, dots and dashes' });
            }
            Object.assign(T.wled, d.wled);
          }
          if (d.layout) Object.assign(T.layout, d.layout);
          if (d.ota && !d.preview) {      // the same rules as the board: a first password is free, a change needs the current one, no switching on without one
            const o = d.ota;
            if (o.pass !== undefined) {
              if (typeof o.pass !== 'string' || o.pass.length < 8 || o.pass.length > 64) return json(res, 400, { error: 'The update password must be 8 to 64 characters (letters, digits, symbols or spaces).' });
              if (state.otaPass && o.currentPass !== state.otaPass) return json(res, 400, { error: 'The current update password is wrong.' });
              state.otaPass = o.pass;
            }
            if (o.enabled !== undefined) {
              if (o.enabled && !state.otaPass) return json(res, 400, { error: 'Set an update password first.' });
              state.otaOn = !!o.enabled;
            }
            state.cfg.ota = { enabled: !!state.otaOn, passSet: !!state.otaPass };
          }
          ['look', 'box', 'relay', 'mqtt'].forEach(k => { if (d[k]) Object.assign(state.cfg[k], d[k]); });
          if (d.mqtt && d.mqtt.pass !== undefined) { state.mqttPass = d.mqtt.pass; state.cfg.mqtt.passSet = d.mqtt.pass !== ''; delete state.cfg.mqtt.pass; }
          return json(res, 200, state.cfg);
        }
        if (p === '/api/targets') {
          state.lastTargetsPost = d;
          if (d.action === 'add') {
            if (state.cfg.targets.length >= 4) return json(res, 400, { error: 'There can be at most 4 targets' });
            state.cfg.targets.push(newTarget(state.cfg.targets.length));
          } else if (d.action === 'remove') {
            if (state.cfg.targets.length <= 1) return json(res, 400, { error: 'At least one target is needed' });
            if (!(d.index >= 0 && d.index < state.cfg.targets.length)) return json(res, 400, { error: 'There is no target ' + (d.index + 1) });
            state.cfg.targets.splice(d.index, 1);
          } else return json(res, 400, { error: 'action must be add or remove' });
          alias();
          return json(res, 200, state.cfg);
        }
        if (p === '/api/usage') {         // the picture map for settings that are being edited: nothing is stored
          state.lastUsagePost = d;
          const T = state.cfg.targets[d.target || 0];
          if (!T) return json(res, 400, { error: 'There is no target ' + ((d.target || 0) + 1) });
          const w = Object.assign({}, T.wled, d.wled || {}), l = Object.assign({}, T.layout, d.layout || {});
          if (w.count > 1000) return json(res, 400, { error: 'A target can have at most 1000 LEDs' });
          return json(res, 200, { leds: w.count | 0, zones: usageOf(w, l) });
        }
        if (p === '/api/export') {     // like the firmware: settings + calibrated tables; never Wi-Fi; the MQTT password only on request
          const cfg = JSON.parse(JSON.stringify(state.cfg)); delete cfg.wled; delete cfg.layout; delete cfg.mqtt.passSet; delete cfg.ota;
          cfg.targets.forEach(t => { delete t.layout.hasTable; delete t.usage; });
          if (d.secrets && state.mqttPass) cfg.mqtt.pass = state.mqttPass;
          const tables = Object.keys(state.maps).map(k => ({ target: +k, start: state.maps[k].start, count: state.maps[k].count, hex: state.maps[k].hex }));
          return json(res, 200, { format: 'fancy2wled-config', version: 2, firmware: 'mock', config: cfg, tables });
        }
        if (p === '/api/import') {
          if (d.format !== 'fancy2wled-config') return json(res, 400, { error: 'This is not a Fancy2WLED config file.' });
          if (!(d.version >= 1)) return json(res, 400, { error: 'This config file has no version number.' });
          if (d.version > 2) return json(res, 400, { error: 'This config file was made by a newer version of Fancy2WLED. Update this board first.' });
          if (!d.config) return json(res, 400, { error: 'The config file has no settings in it.' });
          state.imported = d;
          const c = d.config;
          if (c.targets) {
            while (state.cfg.targets.length < c.targets.length) state.cfg.targets.push(newTarget(state.cfg.targets.length));
            state.cfg.targets.length = c.targets.length;
            c.targets.forEach((t, i) => { const T = state.cfg.targets[i]; ['name', 'enabled', 'trim'].forEach(k => { if (t[k] !== undefined) T[k] = t[k]; }); Object.assign(T.wled, t.wled || {}); Object.assign(T.layout, t.layout || {}); });
          } else {                      // a file from before several targets: it describes target 0
            Object.assign(state.cfg.targets[0].wled, c.wled || {}); Object.assign(state.cfg.targets[0].layout, c.layout || {});
          }
          const tables = d.tables || (d.table ? [Object.assign({ target: 0 }, d.table)] : []);
          tables.forEach(t => { state.maps[t.target || 0] = t; state.cfg.targets[t.target || 0].layout.hasTable = true; });
          ['look', 'box', 'relay', 'mqtt'].forEach(k => { if (c[k]) Object.assign(state.cfg[k], c[k]); });
          if (c.mqtt && c.mqtt.pass !== undefined) { state.mqttPass = c.mqtt.pass; state.cfg.mqtt.passSet = c.mqtt.pass !== ''; delete state.cfg.mqtt.pass; }
          alias();
          return json(res, 200, state.cfg);
        }
        if (p === '/api/map') {
          const ti = d.target || 0, T = state.cfg.targets[ti];
          state.maps[ti] = d; state.lastMapPost = d;
          T.layout.hasTable = true; T.layout.type = 'table'; T.wled.start = d.start; T.wled.count = d.count;
          return json(res, 200, { ok: true });
        }
        if (p === '/api/test') { state.test = d; return json(res, 200, { ok: true }); }
        if (p === '/api/mark') { state.mark = d; state.marks = (state.marks || 0) + 1; return json(res, 200, { ok: true }); }
        if (p === '/api/cal/begin') { state.calActive = true; state.screen = 'black'; return json(res, 200, { total: state.total }); }
        if (p === '/api/cal/led') return json(res, 200, { ok: true });
        if (p === '/api/cal/screen') { state.screen = d.mode; return json(res, 200, { ok: true }); }
        if (p === '/api/cal/end') { state.calActive = false; state.screen = 'idle'; return json(res, 200, { ok: true }); }
        return json(res, 200, { ok: true });
      });
      return;
    }
    res.writeHead(405); res.end();
  });
  return { server, calls, state };
}

if (require.main === module) {
  const port = +(process.argv[2] || 8099);
  const dev = createDevice();
  dev.server.listen(port, () => console.log('mock relay on http://localhost:' + port + '/'));
}
module.exports = { createDevice };
