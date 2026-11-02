// A fake relay for developing and testing the web UI without hardware.
//   node test/mock_device.js [port]      then open http://localhost:8099/
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');

const WEB = path.join(__dirname, '..', 'web');
const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'application/javascript', '.css': 'text/css' };
const ROUTES = { '/': 'index.html', '/calibrate': 'calibrate.html', '/screen': 'screen.html', '/analysis.js': 'analysis.js', '/calibration.js': 'calibration.js', '/style.css': 'style.css' };

function createDevice(opts) {
  opts = opts || {};
  const calls = [];
  const state = {
    cfg: { wled: { host: '', start: 0, count: 0, turnOn: true, restore: true, brightness: -1 },
      layout: { type: 'linear', edge: 'top', led0Right: false, spanFirst: -1, spanLast: -1, corner: 0, ccw: false, sides: [0, 0, 0, 0], shift: 0, hasTable: false },
      look: { brightness: 1, gamma: 1, saturation: 1, smoothing: 0.5, smoothOut: 100 }, box: { host: '' },
      relay: { enabled: true }, mqtt: { enabled: false, host: '', port: 1883, user: '', passSet: false, base: '', discovery: 'homeassistant' } },
    screen: 'idle', calActive: false, map: null, test: null, total: opts.total || 600
  };
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
      if (p === '/api/status') return json(res, 200, { ver: 'mock', mode: 'sta', ssid: 'TestNet', ip: '192.0.2.10', hostname: 'fancy2wled', rssi: -50, uptime: 5, state: 'waiting for the Sync Box', https: false, heap: 1e5,
        enabled: state.cfg.relay.enabled, mqtt: { enabled: state.cfg.mqtt.enabled, connected: state.cfg.mqtt.enabled && !!state.cfg.mqtt.host, error: '', base: 'fancy2wled/abcd' },
        box: { fps: 13.4, age: 50, sender: '192.0.2.20' }, wled: { host: state.cfg.wled.host, ok: !!state.cfg.wled.host, leds: state.total, name: 'WLED' } });
      if (p === '/api/config') return json(res, 200, state.cfg);
      if (p === '/api/zones') return json(res, 200, { age: 40, zones: Array.from({ length: 24 }, () => [state.screen === 'white' ? 200 : 0, 0, 0]) });
      if (p === '/api/screen') return json(res, 200, { mode: state.calActive ? state.screen : 'idle' });
      if (p === '/api/wled/discover') return json(res, 200, { devices: [{ name: 'wled-test', ip: '192.0.2.50' }] });
      if (p === '/api/wled/info') return json(res, 200, { name: 'WLED', ver: '0.15', leds: state.total, rgbw: false, on: true, bri: 128 });
      if (p === '/api/wifi/scan') return json(res, 200, { networks: ['TestNet', 'Other'] });
      res.writeHead(404); return res.end('nope');
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
          Object.keys(d).forEach(k => { if (k !== 'preview') Object.assign(state.cfg[k] || (state.cfg[k] = {}), d[k]); });
          if (d.mqtt && d.mqtt.pass !== undefined) { state.mqttPass = d.mqtt.pass; state.cfg.mqtt.passSet = d.mqtt.pass !== ''; delete state.cfg.mqtt.pass; }
          return json(res, 200, state.cfg);
        }
        if (p === '/api/export') {     // like the firmware: settings + calibrated table; never Wi-Fi; the MQTT password only on request
          const cfg = JSON.parse(JSON.stringify(state.cfg)); delete cfg.layout.hasTable; delete cfg.mqtt.passSet;
          if (d.secrets && state.mqttPass) cfg.mqtt.pass = state.mqttPass;
          const out = { format: 'fancy2wled-config', version: 1, firmware: 'mock', config: cfg };
          if (state.map) out.table = { start: state.map.start, count: state.map.count, hex: state.map.hex };
          return json(res, 200, out);
        }
        if (p === '/api/import') {
          if (d.format !== 'fancy2wled-config') return json(res, 400, { error: 'This is not a Fancy2WLED config file.' });
          if (!(d.version >= 1)) return json(res, 400, { error: 'This config file has no version number.' });
          if (d.version > 1) return json(res, 400, { error: 'This config file was made by a newer version of Fancy2WLED. Update this board first.' });
          if (!d.config) return json(res, 400, { error: 'The config file has no settings in it.' });
          state.imported = d;
          if (d.table) { state.map = d.table; state.cfg.layout.hasTable = true; }
          Object.keys(d.config).forEach(k => Object.assign(state.cfg[k] || (state.cfg[k] = {}), d.config[k]));
          if (d.config.mqtt && d.config.mqtt.pass !== undefined) { state.mqttPass = d.config.mqtt.pass; state.cfg.mqtt.passSet = d.config.mqtt.pass !== ''; delete state.cfg.mqtt.pass; }
          return json(res, 200, state.cfg);
        }
        if (p === '/api/map') { state.map = d; state.cfg.layout.hasTable = true; state.cfg.layout.type = 'table'; state.cfg.wled.start = d.start; state.cfg.wled.count = d.count; return json(res, 200, { ok: true }); }
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
