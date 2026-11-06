// Regenerates the screenshots in docs/images from the real setup page running against test/mock_device.js, with made-up sample data
// (documentation addresses 192.0.2.x, no real devices; tools/sample_device.js), so no private information can end up in them.
//   node tools/make_screenshots.js        (needs Microsoft Edge or Google Chrome; uses its own temporary profile and debugging port)
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { sampleDevice } = require('./sample_device.js');

const CANDIDATES = [
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Google/Chrome/Application/chrome.exe', '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const exe = CANDIDATES.find(p => fs.existsSync(p));
if (!exe) { console.log('no Chromium-based browser found'); process.exit(1); }
const OUT = process.env.FWB_SHOT_OUT || path.join(__dirname, '..', 'docs', 'images');      // FWB_SHOT_OUT / FWB_SHOT_WIDTH: for trying a design at another size
const WIDTH = parseInt(process.env.FWB_SHOT_WIDTH || '1000', 10);
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
  const { dev, base } = await sampleDevice();

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fwb-shots-'));
  const proc = spawn(exe, ['--headless=new', '--remote-debugging-port=' + PORT, '--user-data-dir=' + dir, '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--hide-scrollbars', 'about:blank'], { stdio: 'ignore' });
  let ok = false;
  for (let i = 0; i < 40 && !ok; i++) { try { await (await fetch('http://127.0.0.1:' + PORT + '/json')).json(); ok = true; } catch (e) { await sleep(250); } }
  if (!ok) { console.log('could not start the browser'); proc.kill(); process.exit(1); }
  try {
    const t = await (await fetch('http://127.0.0.1:' + PORT + '/json/new?' + encodeURIComponent(base + '/'), { method: 'PUT' })).json();
    const c = await cdp(t.webSocketDebuggerUrl);
    await c.send('Page.enable'); await c.send('Page.bringToFront'); await c.send('Emulation.setFocusEmulationEnabled', { enabled: true });
    const size = (w, h, mobile) => c.send('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 2, mobile: !!mobile });
    const scheme = async v => { await c.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: v }] }); await sleep(300); };
    const waitStatus = async () => { await sleep(3200); };      // the page polls the status every 2.5 s
    await size(WIDTH, 900);
    await scheme('dark');
    await sleep(1800); await waitStatus();

    const open = async (tab, sub) => {
      await c.ev("document.querySelector('#nav [data-tab=" + tab + "]').click(); " + (sub ? "document.querySelector('.seg [data-sub=" + sub + "]').click(); " : "") + "window.scrollTo(0,0); 1");
      await sleep(500);
    };
    // a picture of the bar and the section that is open, as tall as its content (so the Save bar sits right under it, not in the middle of it)
    async function shotView(file, w) {
      await size(w || WIDTH, 200, !!w && w < 600);
      await sleep(300);
      const h = await c.ev("Math.ceil(document.querySelector('main').getBoundingClientRect().bottom + window.scrollY + 16)");
      await size(w || WIDTH, h, !!w && w < 600);
      await sleep(400);
      const res = await c.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true, clip: { x: 0, y: 0, width: w || WIDTH, height: h, scale: 1 } });
      fs.writeFileSync(path.join(OUT, file), Buffer.from(res.result.data, 'base64'));
      console.log('wrote ' + path.join(OUT, file) + '  (' + Math.round(fs.statSync(path.join(OUT, file)).size / 1024) + ' KB)');
      await size(WIDTH, 900);
    }
    // a picture of one element (or from one element to another)
    async function shot(file, from, to) {
      const r = await c.ev("(function(){var a=document.querySelector('" + from + "'),b=document.querySelector('" + (to || from) + "');if(!a||!b)return null;var ra=a.getBoundingClientRect(),rb=b.getBoundingClientRect();return {x:ra.left+window.scrollX,y:ra.top+window.scrollY,w:Math.max(ra.right,rb.right)-ra.left,h:rb.bottom-ra.top};})()");
      if (!r) { console.log('MISSING ' + file + ' (' + from + ')'); return; }
      const pad = 14;
      const res = await c.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true, clip: { x: Math.max(0, r.x - pad), y: Math.max(0, r.y - pad), width: r.w + 2 * pad, height: r.h + 2 * pad, scale: 1 } });
      fs.writeFileSync(path.join(OUT, file), Buffer.from(res.result.data, 'base64'));
      console.log('wrote ' + path.join(OUT, file) + '  (' + Math.round(fs.statSync(path.join(OUT, file)).size / 1024) + ' KB)');
    }
    const select = async i => { await c.ev("var s=document.getElementById('tSel');s.value='" + i + "';s.dispatchEvent(new Event('change',{bubbles:true}));1"); await sleep(900); await waitStatus(); };

    await open('status');
    await shotView('overview-dark.png');
    await open('lights', 'layout');
    await sleep(1200);                                          // the map's live colours
    await shot('picture-map.png', '#mapCard');
    await shotView('lights-layout.png');
    await open('lights', 'conn');
    await select(0);
    await shot('wled-target.png', '.picker', '.sub.on');
    await select(2);
    await shot('govee-target.png', '.picker', '.sub.on');
    await select(0);
    await open('look');
    await shotView('look-and-tv-off.png');
    await open('device');
    await shot('firmware-updates.png', '#otaCard');
    await open('lights', 'layout');
    await shotView('phone-lights.png', 390);
    await scheme('light'); await sleep(500);
    await open('status');
    await shotView('overview-light.png');
    c.close();
  } finally { proc.kill(); dev.server.close(); try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) { /* the browser may still hold files */ } }
  process.exit(0);
})();
