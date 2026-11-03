// Regenerates the screenshots in docs/images from the real setup page running against test/mock_device.js, with made-up sample data
// (documentation addresses 192.0.2.x, no real devices), so no private information can end up in them.
//   node tools/make_screenshots.js        (needs Microsoft Edge or Google Chrome; uses its own temporary profile and debugging port)
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { createDevice } = require('../test/mock_device.js');

const CANDIDATES = [
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Google/Chrome/Application/chrome.exe', '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const exe = CANDIDATES.find(p => fs.existsSync(p));
if (!exe) { console.log('no Chromium-based browser found'); process.exit(1); }
const OUT = path.join(__dirname, '..', 'docs', 'images');
const PORT = 9341;
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function cdp(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  let id = 0; const waiting = {};
  ws.onmessage = ev => { const m = JSON.parse(ev.data); if (m.id && waiting[m.id]) { waiting[m.id](m); delete waiting[m.id]; } };
  const send = (method, params) => new Promise(r => { const i = ++id; waiting[i] = r; ws.send(JSON.stringify({ id: i, method, params: params || {} })); });
  const ev = async expr => { const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true }); return r.result.result ? r.result.result.value : undefined; };
  await send('Runtime.enable');
  return { send, ev, close: () => ws.close() };
}

(async function main() {
  fs.mkdirSync(OUT, { recursive: true });
  const dev = createDevice();
  await new Promise(r => dev.server.listen(0, r));
  const base = 'http://localhost:' + dev.server.address().port;
  const post = (p, b) => fetch(base + p, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(b) }).then(r => r.json());

  // ---- sample data: a ceiling strip, a cabinet light, a Govee desk light
  dev.state.relayState = 'relaying';
  dev.state.version = '0.3.0';
  dev.state.totals = { '192.0.2.50': 600, '192.0.2.51': 266 };
  await post('/api/targets', { action: 'add' });
  await post('/api/targets', { action: 'add' });
  await post('/api/config', { target: 0, name: 'TV strip', enabled: true, trim: 1, wled: { host: '192.0.2.50', start: 0, count: 210, turnOn: true, restore: true, brightness: -1 }, layout: { type: 'linear', edge: 'top', led0Right: false, spanFirst: 82, spanLast: 172 } });
  await post('/api/config', { target: 1, name: 'Cabinet', enabled: true, trim: 0.8, wled: { host: '192.0.2.51', start: 0, count: 266, turnOn: true, restore: true, brightness: -1 }, layout: { type: 'linear', edge: 'right', led0Right: false } });
  await post('/api/config', { target: 2, name: 'Desk lights', kind: 'govee', enabled: true, trim: 0.9, wled: { host: '192.0.2.60', count: 20, turnOn: true, restore: true, brightness: 80 }, layout: { type: 'linear', edge: 'bottom', led0Right: true } });
  await post('/api/config', { look: { brightness: 1, gamma: 1, saturation: 1.1, smoothing: 0.5, boost: 0.6, smoothOut: 100 } });
  dev.state.otaPass = 'a sample password'; dev.state.otaOn = true; dev.state.cfg.ota = { enabled: true, passSet: true };

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fwb-shots-'));
  const proc = spawn(exe, ['--headless=new', '--remote-debugging-port=' + PORT, '--user-data-dir=' + dir, '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--hide-scrollbars', 'about:blank'], { stdio: 'ignore' });
  let ok = false;
  for (let i = 0; i < 40 && !ok; i++) { try { await (await fetch('http://127.0.0.1:' + PORT + '/json')).json(); ok = true; } catch (e) { await sleep(250); } }
  if (!ok) { console.log('could not start the browser'); proc.kill(); process.exit(1); }
  try {
    const t = await (await fetch('http://127.0.0.1:' + PORT + '/json/new?' + encodeURIComponent(base + '/'), { method: 'PUT' })).json();
    const c = await cdp(t.webSocketDebuggerUrl);
    await c.send('Page.enable'); await c.send('Page.bringToFront'); await c.send('Emulation.setFocusEmulationEnabled', { enabled: true });
    await c.send('Emulation.setDeviceMetricsOverride', { width: 760, height: 1200, deviceScaleFactor: 2, mobile: false });
    const scheme = async v => { await c.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: v }] }); await sleep(300); };
    const waitStatus = async () => { await sleep(3200); };      // the page polls the status every 2.5 s
    await scheme('dark');
    await sleep(1800); await waitStatus();
    await c.ev("document.querySelectorAll('.sticky').forEach(function(x){x.style.display='none';}); 1");     // the Save bar sticks to the bottom of the window: it must not land in the middle of a card
    const tag = () => c.ev("document.querySelectorAll('.card').forEach(function(x){var h=x.querySelector('h2');if(h)x.setAttribute('data-title',h.textContent.trim());}); 1");
    await tag();

    async function shot(file, from, to) {
      const r = await c.ev("(function(){var a=document.querySelector('" + from + "'),b=document.querySelector('" + (to || from) + "');if(!a||!b)return null;var ra=a.getBoundingClientRect(),rb=b.getBoundingClientRect();return {x:ra.left+scrollX,y:ra.top+scrollY,w:Math.max(ra.width,rb.width),h:rb.bottom-ra.top};})()");
      if (!r) { console.log('MISSING ' + file + ' (' + from + ')'); return; }
      const pad = 14;
      const res = await c.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true, clip: { x: Math.max(0, r.x - pad), y: Math.max(0, r.y - pad), width: r.w + 2 * pad, height: r.h + 2 * pad, scale: 1 } });
      fs.writeFileSync(path.join(OUT, file), Buffer.from(res.result.data, 'base64'));
      console.log('wrote docs/images/' + file + '  (' + Math.round(fs.statSync(path.join(OUT, file)).size / 1024) + ' KB)');
    }
    const select = async i => { await c.ev("var s=document.getElementById('tSel');s.value='" + i + "';s.dispatchEvent(new Event('change',{bubbles:true}));1"); await sleep(900); await waitStatus(); };

    await shot('overview-dark.png', '#stat', '#mapCard');
    await shot('picture-map.png', '#mapCard');
    await shot('look-and-tv-off.png', '[data-title="Look"]', '#tvCard');
    await shot('firmware-updates.png', '#otaCard');
    await select(2); await tag();
    await shot('govee-target.png', '[data-title="Targets"]', '[data-title="Govee light"]');
    await select(0); await tag();
    await shot('wled-target.png', '[data-title="Targets"]', '[data-title="WLED"]');
    await scheme('light'); await sleep(500);
    await shot('overview-light.png', '#stat', '#mapCard');
    c.close();
  } finally { proc.kill(); dev.server.close(); try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) { /* the browser may still hold files */ } }
  process.exit(0);
})();
