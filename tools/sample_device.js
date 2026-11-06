// A mock relay (test/mock_device.js) filled with made-up sample data: a ceiling strip, a cabinet light and a Govee desk light, with documentation addresses (192.0.2.x) only,
// so pictures made from it cannot show anything private. Used by tools/make_screenshots.js and tools/check_layout.js.
'use strict';
const { createDevice } = require('../test/mock_device.js');

async function sampleDevice() {
  const dev = createDevice();
  await new Promise(r => dev.server.listen(0, r));
  const base = 'http://localhost:' + dev.server.address().port;
  const post = (p, b) => fetch(base + p, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(b) }).then(r => r.json());

  // ---- sample data: a ceiling strip, a cabinet light, a Govee desk light
  dev.state.relayState = 'relaying';
  // what the Sync Box is sending in the live picture map: a colourful scene, brighter along the edges the targets follow
  { const row = [[78, 4, 152], [150, 130, 45], [98, 2, 141], [67, 109, 116], [158, 17, 69], [67, 102, 131]]; const mid = row.map(c => c.map(v => Math.round(v * 0.55)));
    dev.state.zones = [].concat(row, mid, mid, row); }
  dev.state.version = '0.3.0';
  dev.state.totals = { '192.0.2.50': 600, '192.0.2.51': 266 };
  await post('/api/targets', { action: 'add' });
  await post('/api/targets', { action: 'add' });
  await post('/api/config', { target: 0, name: 'TV strip', enabled: true, trim: 1, wled: { host: '192.0.2.50', start: 0, count: 210, turnOn: true, restore: true, brightness: -1 }, layout: { type: 'linear', edge: 'top', led0Right: false, spanFirst: 82, spanLast: 172 } });
  await post('/api/config', { target: 1, name: 'Cabinet', enabled: true, trim: 0.8, wled: { host: '192.0.2.51', start: 0, count: 266, turnOn: true, restore: true, brightness: -1 }, layout: { type: 'linear', edge: 'right', led0Right: false } });
  await post('/api/config', { target: 2, name: 'Desk lights', kind: 'govee', enabled: true, trim: 0.9, wled: { host: '192.0.2.60', count: 20, turnOn: true, restore: true, brightness: 80 }, layout: { type: 'linear', edge: 'bottom', led0Right: true } });
  await post('/api/config', { look: { brightness: 1, gamma: 1, saturation: 1.1, smoothing: 0.5, boost: 0.6, smoothOut: 100 } });
  dev.state.otaPass = 'a sample password'; dev.state.otaOn = true; dev.state.cfg.ota = { enabled: true, passSet: true };
  return { dev, base, post };
}

module.exports = { sampleDevice };
