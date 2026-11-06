// Measures how much scrolling the setup page needs and saves pictures of it, at a phone size and a desktop size, against the sample mock relay (test/mock_device.js).
//   node tools/check_layout.js [--out DIR] [--shots] [--max-screens N] [--scheme dark|light]
// It prints, for every view of the page (the whole page, or one tab at a time when the page has a navigation bar), how many screens tall it is. With --max-screens it exits
// with an error when a view is taller than that on the phone. --shots also saves a picture of the first screen and of the whole view into DIR. Needs Microsoft Edge or
// Google Chrome (its own temporary profile and debugging port).
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
const args = process.argv.slice(2);
const opt = (name, def) => { const i = args.indexOf(name); return i >= 0 ? args[i + 1] : def; };
const OUT = opt('--out', path.join(os.tmpdir(), 'fwb-layout'));
const SHOTS = args.includes('--shots');
const SCHEME = opt('--scheme', 'dark');
const MAX = parseFloat(opt('--max-screens', '0'));
const PORT = 9342;
const sleep = ms => new Promise(r => setTimeout(r, ms));
const exe = CANDIDATES.find(p => fs.existsSync(p));
if (!exe) { console.log('no Chromium-based browser found'); process.exit(1); }

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
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fwb-layout-prof-'));
  const proc = spawn(exe, ['--headless=new', '--remote-debugging-port=' + PORT, '--user-data-dir=' + dir, '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--hide-scrollbars', 'about:blank'], { stdio: 'ignore' });
  let ok = false;
  for (let i = 0; i < 40 && !ok; i++) { try { await (await fetch('http://127.0.0.1:' + PORT + '/json')).json(); ok = true; } catch (e) { await sleep(250); } }
  if (!ok) { console.log('could not start the browser'); proc.kill(); process.exit(1); }
  let tooTall = 0;
  const rows = [];
  try {
    for (const vp of [{ name: 'phone', w: 390, h: 844, mobile: true }, { name: 'desktop', w: 1200, h: 800, mobile: false }]) {
      const t = await (await fetch('http://127.0.0.1:' + PORT + '/json/new?' + encodeURIComponent(base + '/'), { method: 'PUT' })).json();
      const c = await cdp(t.webSocketDebuggerUrl);
      await c.send('Page.enable'); await c.send('Page.bringToFront'); await c.send('Emulation.setFocusEmulationEnabled', { enabled: true });
      await c.send('Emulation.setDeviceMetricsOverride', { width: vp.w, height: vp.h, deviceScaleFactor: vp.mobile ? 2 : 1, mobile: vp.mobile });
      await c.send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value: SCHEME }] });
      await sleep(1800); await sleep(3000);                                  // the page polls the status every 2.5 s
      const tabs = await c.ev("Array.prototype.map.call(document.querySelectorAll('#nav [data-tab]'),function(b){return b.getAttribute('data-tab');})");
      const subs = tabs && tabs.length ? await c.ev("Array.prototype.map.call(document.querySelectorAll('.seg [data-sub]'),function(b){return b.getAttribute('data-sub');})") : [];
      const views = [];
      (tabs && tabs.length ? tabs : [null]).forEach(t => { if (t === 'lights' && subs.length) subs.forEach(s => views.push('lights/' + s)); else views.push(t); });
      for (const tab of views) {
        if (tab) {
          const parts = tab.split('/');
          await c.ev("document.querySelector('#nav [data-tab=\"" + parts[0] + "\"]').click(); 1");
          if (parts[1]) await c.ev("document.querySelector('.seg [data-sub=\"" + parts[1] + "\"]').click(); 1");
          await c.ev("window.scrollTo(0,0); 1"); await sleep(700);
        }
        const m = await c.ev("(function(){var d=document.documentElement;return {h:Math.max(d.scrollHeight,document.body.scrollHeight),vh:window.innerHeight};})()");
        const screens = m.h / m.vh;
        rows.push({ vp: vp.name, view: tab || 'whole page', px: m.h, screens });
        if (SHOTS) {
          const name = vp.name + '-' + (tab || 'page').replace('/', '-');
          const first = await c.send('Page.captureScreenshot', { format: 'png' });
          fs.writeFileSync(path.join(OUT, name + '-first.png'), Buffer.from(first.result.data, 'base64'));
          const full = await c.send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true, clip: { x: 0, y: 0, width: vp.w, height: m.h, scale: 1 } });
          fs.writeFileSync(path.join(OUT, name + '-full.png'), Buffer.from(full.result.data, 'base64'));
        }
        if (MAX && vp.name === 'phone' && screens > MAX) tooTall++;
      }
      c.close();
    }
  } finally { proc.kill(); dev.server.close(); try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) { /* the browser may still hold files */ } }
  console.log('view'.padEnd(14) + 'phone (390 x 844)'.padEnd(26) + 'desktop (1200 x 800)');
  const views = Array.from(new Set(rows.map(r => r.view)));
  for (const v of views) {
    const p = rows.find(r => r.vp === 'phone' && r.view === v), d = rows.find(r => r.vp === 'desktop' && r.view === v);
    console.log(v.padEnd(14) + (p ? p.px + ' px = ' + p.screens.toFixed(1) + ' screens' : '').padEnd(26) + (d ? d.px + ' px = ' + d.screens.toFixed(1) + ' screens' : ''));
  }
  if (SHOTS) console.log('pictures in ' + OUT);
  if (tooTall) { console.log(tooTall + ' view(s) taller than ' + MAX + ' screens on the phone'); process.exit(1); }
  process.exit(0);
})();
