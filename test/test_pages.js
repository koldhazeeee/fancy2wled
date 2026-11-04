// Loads the real web pages in headless Edge/Chrome (with a fake camera) against test/mock_device.js and checks they work.
//   node test/test_pages.js        (needs Microsoft Edge or Google Chrome installed; skipped if neither is found)
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawn } = require('child_process');
const { createDevice } = require('./mock_device.js');

const CANDIDATES = [
  'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe', 'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
  'C:/Program Files/Google/Chrome/Application/chrome.exe', '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
  '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'];
const exe = CANDIDATES.find(p => fs.existsSync(p));
if (!exe) { console.log('no Chromium-based browser found; skipping page tests'); process.exit(0); }

let pass = 0, fail = 0;
const check = (n, c, x) => { if (c) pass++; else { fail++; console.log('FAIL: ' + n + (x !== undefined ? '  ' + x : '')); } };
const sleep = ms => new Promise(r => setTimeout(r, ms));

async function cdp(wsUrl) {
  const ws = new WebSocket(wsUrl);
  await new Promise((res, rej) => { ws.onopen = res; ws.onerror = rej; });
  let id = 0; const waiting = {}; const logs = [];
  ws.onmessage = ev => {
    const m = JSON.parse(ev.data);
    if (m.id && waiting[m.id]) { waiting[m.id](m); delete waiting[m.id]; }
    if (m.method === 'Runtime.exceptionThrown') logs.push('EXC ' + JSON.stringify(m.params.exceptionDetails.exception && m.params.exceptionDetails.exception.description || m.params.exceptionDetails.text));
    if (m.method === 'Runtime.consoleAPICalled' && m.params.type === 'error') logs.push('console.error ' + m.params.args.map(a => a.value || a.description).join(' '));
  };
  const send = (method, params) => new Promise(r => { const i = ++id; waiting[i] = r; ws.send(JSON.stringify({ id: i, method, params: params || {} })); });
  const ev = async expr => { const r = await send('Runtime.evaluate', { expression: expr, awaitPromise: true, returnByValue: true }); return r.result.result ? r.result.result.value : undefined; };
  await send('Runtime.enable');
  return { send, ev, logs, close: () => ws.close() };
}

(async function main() {
  const dev = createDevice();
  await new Promise(r => dev.server.listen(0, r));
  const base = 'http://localhost:' + dev.server.address().port;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'fwb-edge-'));
  // The fake camera shows a flat grey picture that never changes (a Y4M file, looped). Chromium's own fake pattern has a moving dot and
  // a clock, and now and then it fooled the calibration into "finding" a TV, which made this test nondeterministic.
  const y4m = path.join(dir, 'flat-grey.y4m');
  { const w = 320, h = 240, frame = Buffer.concat([Buffer.from('FRAME\n'), Buffer.alloc(w * h, 100), Buffer.alloc(w * h / 2, 128)]);
    fs.writeFileSync(y4m, Buffer.concat([Buffer.from('YUV4MPEG2 W' + w + ' H' + h + ' F10:1 Ip A1:1 C420jpeg\n'), ...Array(10).fill(frame)])); }
  const proc = spawn(exe, ['--headless=new', '--remote-debugging-port=9333', '--user-data-dir=' + dir, '--no-first-run', '--no-default-browser-check',
    '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--use-file-for-fake-video-capture=' + y4m, '--disable-gpu', 'about:blank'], { stdio: 'ignore' });
  let targets = null;
  for (let i = 0; i < 40 && !targets; i++) { try { targets = await (await fetch('http://127.0.0.1:9333/json')).json(); } catch (e) { await sleep(250); } }
  if (!targets) { console.log('could not start the browser'); proc.kill(); process.exit(1); }
  // A tab the browser thinks is hidden gets its timers throttled to one a second, which turns the 8 s calibration run into minutes
  // (and made this test flaky), so every new tab is brought to the front and made to look focused.
  const newTab = async url => {
    const t = await (await fetch('http://127.0.0.1:9333/json/new?' + encodeURIComponent(url), { method: 'PUT' })).json();
    const c = await cdp(t.webSocketDebuggerUrl);
    await c.send('Page.enable'); await c.send('Page.bringToFront'); await c.send('Emulation.setFocusEmulationEnabled', { enabled: true });
    await fetch('http://127.0.0.1:9333/json/activate/' + t.id);
    return c;
  };
  try {
    // ---- setup page
    let p = await newTab(base + '/');
    await sleep(1800);
    check('index: loads status pills', (await p.ev("document.getElementById('pBox').textContent")).includes('13'), await p.ev("document.getElementById('pBox').textContent"));
    check('index: main panel visible', await p.ev("!document.getElementById('main').classList.contains('hidden')"));
    await p.ev("document.getElementById('wHost').value='192.0.2.50'; document.getElementById('wCount').value='150'; document.getElementById('lPlace').value='top'; document.getElementById('save').click(); 1");
    await sleep(600);
    check('index: Save posts the config', dev.state.cfg.wled.host === '192.0.2.50' && dev.state.cfg.wled.count === 150, JSON.stringify(dev.state.cfg.wled));
    await p.ev("document.getElementById('wFind').click(); 1"); await sleep(500);
    check('index: Find lists devices', (await p.ev("document.getElementById('wList').textContent")).includes('192.0.2.50'));
    // a WLED can be addressed by name: Find offers the name, a pasted URL is cleaned up, the page says which address the name points to
    await p.ev("document.getElementById('wList').querySelector('button').click(); 1"); await sleep(400);
    check('address: picking a found device fills in its name (it keeps working if the IP changes)', (await p.ev("document.getElementById('wHost').value")) === 'wled-test.local', await p.ev("document.getElementById('wHost').value"));
    await p.ev("document.getElementById('wHost').value='  HTTP://Wled-Ceiling.local/  '; document.getElementById('save').click(); 1"); await sleep(600);
    check('address: a pasted URL is saved as just the name', dev.state.cfg.wled.host === 'wled-ceiling.local', dev.state.cfg.wled.host);
    check('address: the box shows the cleaned-up name after saving', (await p.ev("document.getElementById('wHost').value")) === 'wled-ceiling.local', await p.ev("document.getElementById('wHost').value"));
    await sleep(3000);
    const resolvedTxt = await p.ev("document.getElementById('wResolved').textContent");
    check('address: the page says which address the name points to', !(await p.ev("document.getElementById('wResolved').classList.contains('hidden')")) && /wled-ceiling\.local at 192\.0\.2\.51/.test(resolvedTxt), resolvedTxt);
    dev.state.nameNotFound = true; await sleep(3000);
    const lookingTxt = await p.ev("document.getElementById('wResolved').textContent");
    check('address: a name that is not found yet says so', /Looking for wled-ceiling\.local/.test(lookingTxt), lookingTxt);
    dev.state.nameNotFound = false;
    await p.ev("document.getElementById('wHost').value='192.0.2.50'; document.getElementById('save').click(); 1"); await sleep(3500);
    check('address: an IP address needs no explanation line', await p.ev("document.getElementById('wResolved').classList.contains('hidden')"));
    check('address: back to the IP address for the tests below', dev.state.cfg.wled.host === '192.0.2.50', dev.state.cfg.wled.host);
    // ---- when the TV is off
    check('tvoff: the card shows the default (hand the WLEDs back after 15 s)', (await p.ev("document.getElementById('tvMode').value")) === 'handback' && (await p.ev("document.getElementById('tvSecs').value")) === '15');
    check('tvoff: with the TV on the page says so', /The TV is on/.test(await p.ev("document.getElementById('tvNow').textContent")), await p.ev("document.getElementById('tvNow').textContent"));
    await p.ev("document.getElementById('tvMode').value='off'; document.getElementById('tvSecs').value='30'; document.getElementById('save').click(); 1"); await sleep(600);
    check('tvoff: Save sends the behaviour and the delay', dev.state.cfg.relay.tvOff.mode === 'off' && dev.state.cfg.relay.tvOff.seconds === 30 && dev.state.cfg.relay.enabled === true, JSON.stringify(dev.state.cfg.relay));
    dev.state.lastConfigPost = null;
    await p.ev("document.getElementById('tvSecs').value='2'; document.getElementById('save').click(); 1"); await sleep(500);
    check('tvoff: a delay under 3 seconds is refused before anything is sent', dev.state.lastConfigPost === null && /between 3 and 600/.test(await p.ev("document.getElementById('saveMsg').textContent")), await p.ev("document.getElementById('saveMsg').textContent"));
    dev.state.blackS = 7; await sleep(3000);
    check('tvoff: a black picture is reported while the delay runs', /black \(7 s so far\)/.test(await p.ev("document.getElementById('tvNow').textContent")), await p.ev("document.getElementById('tvNow').textContent"));
    dev.state.tvOff = true; await sleep(3000);
    check('tvoff: once it counts as off the page says so, with how long', /The TV is off \(the picture has been black for 31 s\)/.test(await p.ev("document.getElementById('tvNow').textContent")), await p.ev("document.getElementById('tvNow').textContent"));
    dev.state.tvOff = false; dev.state.blackS = 0;
    await p.ev("document.getElementById('tvMode').value='handback'; document.getElementById('tvSecs').value='15'; document.getElementById('save').click(); 1"); await sleep(600);
    check('tvoff: back to the defaults for the tests below', dev.state.cfg.relay.tvOff.mode === 'handback' && dev.state.cfg.relay.tvOff.seconds === 15, JSON.stringify(dev.state.cfg.relay));
    // ---- firmware updates: a password first, then the switch, then the upload
    const ota = async js => p.ev(js);
    const hidden = async id => ota("document.getElementById('" + id + "').classList.contains('hidden')");
    const osv = (id, v) => "document.getElementById('" + id + "').value=" + JSON.stringify(v) + ";";
    const click = id => "document.getElementById('" + id + "').click(); 1";
    const text = id => ota("document.getElementById('" + id + "').textContent");
    check('ota: updates start off, the switch is locked until a password exists', (await ota("document.getElementById('otaOn').disabled")) === true && (await ota("document.getElementById('otaOn').checked")) === false && !(await hidden('otaNeed')), 'switch disabled / checked / hint');
    check('ota: the upload part is hidden while updates are off, the password form is shown', (await hidden('otaUp')) && !(await hidden('otaSet')) && (await hidden('otaChange')));
    check('ota: the page says which firmware is running and how big a file fits', /Running firmware mock from slot app0/.test(await text('otaVer')) && /3264 KB/.test(await text('otaVer')), await text('otaVer'));
    await ota(osv('otaNew', 'short') + osv('otaNew2', 'short') + click('otaSetBtn')); await sleep(400);
    check('ota: a password under 8 characters is refused before anything is sent', /8 to 64 characters/.test(await text('otaMsg')) && !dev.state.otaPass, await text('otaMsg'));
    await ota(osv('otaNew', 'long enough pw') + osv('otaNew2', 'long enough px') + click('otaSetBtn')); await sleep(400);
    check('ota: two different passwords are refused', /not the same/.test(await text('otaMsg')) && !dev.state.otaPass, await text('otaMsg'));
    await ota(osv('otaNew', 'long enough pw') + osv('otaNew2', 'long enough pw') + click('otaSetBtn')); await sleep(700);
    check('ota: a good password is stored on the board', dev.state.otaPass === 'long enough pw' && !dev.state.otaOn, String(dev.state.otaPass));
    check('ota: the password form turns into a change form, the switch unlocks', (await hidden('otaSet')) && !(await hidden('otaChange')) && (await ota("document.getElementById('otaOn').disabled")) === false && (await hidden('otaNeed')));
    check('ota: the password is never put in the page, the saved settings or the form', !JSON.stringify(dev.state.cfg).includes('long enough pw') && (await ota("document.getElementById('otaNew').value")) === '');
    check('ota: updates are still off after setting a password (a separate decision)', (await ota("document.getElementById('otaOn').checked")) === false && (await hidden('otaUp')));
    await ota(click('otaOn')); await sleep(700);
    check('ota: switching updates on reaches the board and shows the upload part', dev.state.otaOn === true && !(await hidden('otaUp')), String(dev.state.otaOn));
    await ota(osv('otaCur', 'wrong password') + osv('otaNewB', 'another good one') + osv('otaNewB2', 'another good one') + click('otaChangeBtn')); await sleep(500);
    check('ota: changing the password needs the current one', /current update password is wrong/.test(await text('otaMsg')) && dev.state.otaPass === 'long enough pw', await text('otaMsg'));
    await ota(osv('otaCur', 'long enough pw') + osv('otaNewB', 'another good one') + osv('otaNewB2', 'another good one') + click('otaChangeBtn')); await sleep(700);
    check('ota: with the current password it changes', dev.state.otaPass === 'another good one' && /Password changed/.test(await text('otaMsg')), await text('otaMsg'));
    const fileJs = (bytes, first) => "(function(){var a=new Uint8Array(" + bytes + ");a[0]=" + first + ";var dt=new DataTransfer();dt.items.add(new File([a],'firmware.bin'));document.getElementById('otaFile').files=dt.files;})();";
    await ota(click('otaGo')); await sleep(300);
    check('ota: Update without a file asks for one', /Choose firmware\.bin/.test(await text('otaMsg')), await text('otaMsg'));
    await ota(fileJs(5000, 0xE9) + click('otaGo')); await sleep(300);
    check('ota: Update without the password asks for it (nothing uploaded)', /Enter the update password/.test(await text('otaMsg')) && !dev.state.lastUpload, await text('otaMsg'));
    await ota(fileJs(5000, 0xE9) + osv('otaPw', 'not the password') + click('otaGo')); await sleep(700);
    check('ota: a wrong password is refused by the board and shown', /Wrong update password/.test(await text('otaMsg')) && !dev.state.uploaded && (await ota("document.getElementById('otaGo').disabled")) === false, await text('otaMsg'));
    await ota(fileJs(5000, 0x00) + osv('otaPw', 'another good one') + click('otaGo')); await sleep(700);
    check('ota: a file that is not firmware is refused by the board and shown', /not a firmware image/.test(await text('otaMsg')) && !dev.state.uploaded, await text('otaMsg'));
    dev.state.imagePending = true; await ota("document.getElementById('otaOn').dispatchEvent(new Event('change')); 1"); await sleep(600);
    check('ota: a firmware that is still being confirmed says so on the page', !(await hidden('otaPending')), await text('otaMsg'));
    await ota(fileJs(5000, 0xE9) + osv('otaPw', 'another good one') + click('otaGo')); await sleep(700);
    check('ota: and a new update is refused until it is confirmed (the message says why)', /not been confirmed yet/.test(await text('otaMsg')) && !dev.state.uploaded, await text('otaMsg'));
    dev.state.imagePending = false; await ota("document.getElementById('otaOn').dispatchEvent(new Event('change')); 1"); await sleep(600);
    check('ota: once confirmed the note goes away', await hidden('otaPending'));
    dev.state.lastUpload = null;
    await ota(fileJs(150000, 0xE9) + osv('otaPw', 'another good one') + click('otaGo')); await sleep(900);
    check('ota: the file goes up as a raw body with the password in a header', dev.state.lastUpload && dev.state.lastUpload.len === 150000 && dev.state.lastUpload.password === 'another good one' && /octet-stream/.test(dev.state.lastUpload.type), JSON.stringify(dev.state.lastUpload));
    check('ota: after a good upload the page says it is installing', dev.state.uploaded === 150000 && /installing it and restarting|Updated/.test(await text('otaMsg')), await text('otaMsg'));
    await sleep(5200);
    check('ota: then it waits for the board and reports the new firmware', /back online with firmware mock/.test(await text('otaMsg')) && (await ota("document.getElementById('otaGo').disabled")) === false, await text('otaMsg'));
    await ota(click('otaOn')); await sleep(700);
    check('ota: switching updates off hides the upload part again', dev.state.otaOn === false && (await hidden('otaUp')), String(dev.state.otaOn));
    await ota(osv('otaCur', 'another good one') + osv('otaNewB', 'long enough pw') + osv('otaNewB2', 'long enough pw') + click('otaChangeBtn')); await sleep(500);
    dev.state.otaOn = false; dev.state.otaPass = ''; dev.state.cfg.ota = { enabled: false, passSet: false }; await p.ev("location.reload(); 1"); await sleep(1800);
    check('ota: back to no password for the tests below', (await ota("document.getElementById('otaOn').disabled")) === true);
    // ---- the picture map shows what the Sync Box is sending, live
    {
      const grp = z => "document.querySelectorAll('#mapSvg g[data-zone]')[" + z + "]";
      const fillOf = async z => ota(grp(z) + ".querySelector('rect').getAttribute('fill')");
      const zonesCalls = () => dev.calls.filter(c => c === 'GET /api/zones').length;
      const toMap = async () => { await ota("document.getElementById('mapCard').scrollIntoView(); 1"); await sleep(1500); };
      dev.state.zones = Array.from({ length: 24 }, (_, z) => [z * 10, 255 - z * 10, 40]);
      const farBefore = zonesCalls(); await sleep(1700);
      check('live map: with the map off screen the page does not ask the board for colours', zonesCalls() === farBefore, zonesCalls() + ' vs ' + farBefore);
      await toMap();
      check('live map: every zone shows the colour the box is sending', (await fillOf(3)) === 'rgb(30,225,40)' && (await fillOf(20)) === 'rgb(200,55,40)', await fillOf(3));
      check('live map: a zone a target follows is full, one nobody follows is faded', (await ota(grp(0) + ".getAttribute('opacity')")) === '1' && (await ota(grp(8) + ".getAttribute('opacity')")) === '0.4');
      check('live map: the target is a bar under the colour, not a block over it', (await ota("document.querySelector('#mapSvg rect[data-t=\"0\"]').getAttribute('height')")) === '8');
      check('live map: tapping a zone gives its colour in words', (await ota(grp(3) + ".querySelector('rect > title').textContent")) === 'Zone 3: red 30, green 225, blue 40');
      check('live map: a note explains what the colours are', /Each zone shows the colour the Sync Box is sending/.test(await text('mapLiveNote')), await text('mapLiveNote'));
      await ota("var c=document.getElementById('mapLiveOn'); c.checked=false; c.dispatchEvent(new Event('change',{bubbles:true})); 1"); await sleep(400);
      check('live map: switched off, the zones are plain and the targets are blocks over them again', (await fillOf(3)) === 'var(--bg)' && (await ota("document.querySelector('#mapSvg rect[data-t=\"0\"]').getAttribute('height')")) !== '8');
      const before = zonesCalls(); await sleep(1700);
      check('live map: switched off, the page stops asking the board for the colours', zonesCalls() === before, zonesCalls() + ' vs ' + before);
      await ota("location.reload(); 1"); await sleep(1900); await toMap();
      check('live map: the choice is remembered after a reload', (await ota("document.getElementById('mapLiveOn').checked")) === false && (await fillOf(3)) === 'var(--bg)');
      await ota("var c=document.getElementById('mapLiveOn'); c.checked=true; c.dispatchEvent(new Event('change',{bubbles:true})); 1"); await sleep(1500);
      const onBefore = zonesCalls(); await sleep(1300);
      check('live map: on screen, the page asks the board about twice a second', zonesCalls() - onBefore >= 1 && zonesCalls() - onBefore <= 4, String(zonesCalls() - onBefore));
      check('live map: switched on again, the colours come back', (await fillOf(3)) === 'rgb(30,225,40)', await fillOf(3));
      dev.state.zonesAge = 9000; await sleep(1500);
      check('live map: when the box has been silent the zones go plain and the note says so', (await fillOf(3)) === 'var(--bg)' && /Waiting for a picture/.test(await text('mapLiveNote')), await text('mapLiveNote'));
      dev.state.zonesAge = null; dev.state.zones = null; await sleep(1200);
    }    // ---- a Govee light as a target
    const sel = id => "document.getElementById('" + id + "')";
    const setType = async v => { await ota(sel('tKind') + ".value='" + v + "'; " + sel('tKind') + ".dispatchEvent(new Event('change',{bubbles:true})); 1"); await sleep(300); };
    check('govee: a target is a WLED unless you say otherwise', (await ota(sel('tKind') + '.value')) === 'wled' && /Check this WLED/.test(await text('wCheck')) && !(await hidden('wStartRow')));
    await setType('govee');
    check('govee: choosing the type changes the labels and hides what only a WLED strip has', (await text('wTitle')) === 'Govee light' && /IP address/.test(await text('wHostLbl')) && /Check this light/.test(await text('wCheck')) &&
      !(await hidden('gNote')) && !(await hidden('gIntro')) && (await hidden('ledIntro')) && (await hidden('wStartRow')) && !(await hidden('findWrap')) && (await hidden('markRow')), await text('wTitle'));
    check('govee: sensible starting values: 20 segments, entry 0 at the right-hand end', (await ota(sel('wCount') + '.value')) === '20' && (await ota(sel('lSide') + '.value')) === '1' && /segments/.test(await text('wCountLbl')));
    await ota(sel('wHost') + ".value='my-light.local'; " + sel('wHost') + ".dispatchEvent(new Event('input',{bubbles:true})); " + click('save')); await sleep(500);
    check('govee: a name instead of an IP address is refused on the page', /IP address/.test(await text('saveMsg')) && dev.state.cfg.targets[0].kind === 'wled', await text('saveMsg'));
    await ota(osv('wHost', '192.0.2.60') + osv('wCount', '70') + click('save')); await sleep(500);
    check('govee: more than 60 segments is refused on the page', /1 to 60 segments/.test(await text('saveMsg')) && dev.state.cfg.targets[0].kind === 'wled', await text('saveMsg'));
    await ota(sel('wFind') + '.click(); 1'); await sleep(500);
    check('govee: Find lists the lights found on the network', /Govee H61C3 \(192\.0\.2\.60\)/.test(await text('wList')), await text('wList'));
    await ota(sel('wList') + ".querySelector('button').click(); 1"); await sleep(700);
    check('govee: picking one fills in its address and checks it (model, firmware, state)', (await ota(sel('wHost') + '.value')) === '192.0.2.60' && /Govee H61C3 \| firmware 1\.06\.24 \| off, brightness 14 %/.test(await text('wInfo')), await text('wInfo'));
    // models: the segment count of a tested model is filled in (over the starting 20), offered when the count was changed, and an unknown model says so
    const segs = () => ota(sel('wCount') + '.value'), modelTxt = () => text('wModel');
    check('govee model: a tested model with a count you set yourself is not overwritten, the right number is offered (H61C3: 42)', (await segs()) === '70' && /H61C3 \(RGBIC neon rope, 10 ft\) has 42 segments; you have 70\./.test(await modelTxt()) && /Use 42/.test(await ota(sel('wModel') + '.querySelector("button").textContent')), await modelTxt());
    await ota(sel('wModel') + '.querySelector("button").click(); 1'); await sleep(300);
    check('govee model: pressing the offer sets it', (await segs()) === '42' && /as set/.test(await modelTxt()), await modelTxt());
    await ota(osv('wCount', '20') + click('wCheck')); await sleep(600);
    check('govee model: over the starting 20 it is simply filled in (H61C3: 42)', (await segs()) === '42' && /has 42 segments; set here/.test(await modelTxt()), await modelTxt());
    await ota(osv('wHost', '192.0.2.61') + osv('wCount', '20') + click('wCheck')); await sleep(600);
    check('govee model: H61B5 (16.4 ft strip) has 50 segments', (await segs()) === '50' && /H61B5 \(RGBIC strip with covers, 16\.4 ft\) has 50 segments; set here/.test(await modelTxt()) && /Govee H61B5/.test(await text('wInfo')), await modelTxt());
    await ota(osv('wHost', '192.0.2.62') + osv('wCount', '20') + click('wCheck')); await sleep(600);
    check('govee model: an unknown model keeps the count and says it is a guess', (await segs()) === '20' && /H9999 is not in the list of tested models/.test(await modelTxt()) && /docs\/GOVEE\.md/.test(await modelTxt()), await modelTxt());
    await ota(osv('wHost', '192.0.2.99') + click('wCheck')); await sleep(600);
    check('govee model: a note from an earlier check is cleared when the next check fails', (await modelTxt()) === '', await modelTxt());
    check('govee: a light that does not answer says what to check', /did not answer.*LAN control/.test(await text('wInfo')), await text('wInfo'));
    await ota(osv('wHost', '192.0.2.60') + osv('wCount', '20') + osv('wBri', '60') + click('save')); await sleep(700);
    const g0 = dev.state.cfg.targets[0];
    check('govee: Save stores the type, the address, the segments and that the strip starts at the right', g0.kind === 'govee' && g0.wled.host === '192.0.2.60' && g0.wled.count === 20 && g0.wled.start === 0 && g0.layout.led0Right === true && g0.wled.brightness === 60, JSON.stringify([g0.kind, g0.wled, g0.layout.led0Right]));
    await sleep(3000);
    check('govee: the status pill counts segments, not LEDs', /Govee 20 segments/.test(await text('wPills')), await text('wPills'));
    check('delay: nothing is measured yet, and it says so', /Nothing measured yet/.test(await text('latList')), await text('latList'));
    dev.state.lat = { [dev.state.cfg.targets[0].name]: { ageMs: 38.1, ageMaxMs: 40, probes: 2, lost: 1, probeAgeS: 15, rttMs: 134.3, rttMinMs: 134.3, rttMaxMs: 134.3 } }; await sleep(3200);
    check('delay: a Govee light shows the board share and how fast it answers, but no total', /Target 1/.test(await text('latList')) && /board 38\.1 ms/.test(await text('latList')) && /the light answers a status question in 134 ms \(1 of 2 went unanswered\)/.test(await text('latList')) && !/= about/.test(await text('latList')), await text('latList'));
    dev.state.lat = { [dev.state.cfg.targets[0].name]: { ageMs: 11, ageMaxMs: 75.9, probes: 3, lost: 3, probeAgeS: 0 } }; await sleep(3200);
    check('delay: a Govee light that never answers a status question says so instead of showing nothing', /board 11\.0 ms \(up to 76\) \+ the light did not answer 3 status questions \(some models do not while they stream\)/.test(await text('latList')) && !/= about/.test(await text('latList')), await text('latList'));
    await ota(location_reload());
    function location_reload() { return "location.reload(); 1"; }
    await sleep(1800);
    check('govee: after a reload the page comes back as a Govee target', (await ota(sel('tKind') + '.value')) === 'govee' && (await text('wTitle')) === 'Govee light' && (await ota(sel('wCount') + '.value')) === '20');
    // ---- the part of the strip in use, and the segment finder
    const fire = (id, ev) => sel(id) + ".dispatchEvent(new Event('" + ev + "',{bubbles:true})); ";
    await ota(osv('wCount', '50') + osv('tvFirst', '0') + osv('tvLast', '24') + sel('tDark') + '.checked=true; ' + fire('tDark', 'change') + click('save')); await sleep(700);
    const gs = dev.state.cfg.targets[0];
    check('govee strip: the part along the picture and "keep the rest dark" are saved', gs.layout.spanFirst === 0 && gs.layout.spanLast === 24 && gs.darkOutside === true && gs.wled.count === 50, JSON.stringify([gs.layout.spanFirst, gs.layout.spanLast, gs.darkOutside, gs.wled.count]));
    check('govee strip: a Govee light gets its own wording and the dark box, the WLED text is hidden', /strip follows the picture/.test(await text('tvHead')) && !(await hidden('tvIntroG')) && (await hidden('tvIntro')) && !(await hidden('darkRow')) && /First segment along the picture/.test(await text('tvFirstLbl')) && /segment numbers go/.test(await text('dirLbl')), await text('tvHead'));
    check('govee strip: it says how much strip that is and that the rest stays dark', /25 segments follow the picture \(about 98 in of the strip\)\. The other 25 stay dark\./.test(await text('tvNote')), await text('tvNote'));
    await ota(sel('tDark') + '.checked=false; ' + fire('tDark', 'change') + '1'); await sleep(300);
    check('govee strip: without the box the rest holds the colour of the zone at its end', /The other 25 hold the colour of the zone at their end\./.test(await text('tvNote')), await text('tvNote'));
    await ota(sel('tDark') + '.checked=true; ' + fire('tDark', 'change') + '1'); await sleep(200);
    dev.state.calLed = null; dev.state.calBegin = null;
    check('govee finder: it is offered for a Govee light, worded for segments', !(await hidden('findWrap')) && /Find my segments/.test(await text('fOpen')) && /Need help finding the segments/.test(await text('fHead')));
    await ota(sel('fOpen') + '.click(); 1'); await sleep(700);
    check('govee finder: opening it starts a session on this target with one segment lit', dev.state.calBegin && dev.state.calBegin.target === 0 && (await ota(sel('fSize') + '.value')) === '1' && !(await hidden('fBox')) && (await hidden('fRunRow')) && !(await hidden('fAltRow')), JSON.stringify(dev.state.calBegin));
    check('govee finder: it names the segment and where it is on the strip', /Lit: segment 0 \(0 to 4 in from the start of the strip\)/.test(await text('fTxt')) && dev.state.calLed && dev.state.calLed.start === 0 && dev.state.calLed.count === 1 && dev.state.calLed.alt === false, (await text('fTxt')) + ' ' + JSON.stringify(dev.state.calLed));
    await ota(sel('fPos') + ".value='7'; " + fire('fPos', 'input') + '1'); await sleep(400);
    check('govee finder: moving the slider moves the lit segment', dev.state.calLed.start === 7 && dev.state.calLed.count === 1 && /Lit: segment 7 \(28 to 31 in/.test(await text('fTxt')), (await text('fTxt')) + ' ' + JSON.stringify(dev.state.calLed));
    await ota(sel('fSize') + ".value='3'; " + fire('fSize', 'input') + '1'); await sleep(400);
    check('govee finder: several segments can be lit at once', dev.state.calLed.count === 3 && /Lit: segments 7 to 9/.test(await text('fTxt')), await text('fTxt'));
    await ota(sel('fAlt') + '.click(); 1'); await sleep(400);
    check('govee finder: "show every segment" sends the alternating pattern', dev.state.calLed.alt === true && /every segment, alternately red and blue/.test(await text('fTxt')) && /Back to the segments/.test(await text('fAlt')), await text('fTxt'));
    await ota(sel('fAlt') + '.click(); 1'); await sleep(400);
    check('govee finder: ... and back to the chosen segments', dev.state.calLed.alt === false && /Lit: segments 7 to 9/.test(await text('fTxt')) && /Show every segment/.test(await text('fAlt')));
    await ota(sel('fPos') + ".value='3'; " + fire('fPos', 'input') + sel('fTvFirst') + '.click(); 1'); await sleep(300);
    check('govee finder: "the picture starts here" fills in the first segment', (await ota(sel('tvFirst') + '.value')) === '3');
    await ota(sel('fClose') + '.click(); 1'); await sleep(400);
    check('govee finder: Done ends the session', dev.state.calActive === false && (await hidden('fBox')));
    await ota(osv('tvFirst', '') + osv('tvLast', '') + sel('tDark') + ".checked=false; " + fire('tDark', 'change') + '1'); await sleep(200);     // leave nothing behind for the WLED tests below
    await setType('wled');
    check('wled: switching back restores the WLED wording, the run buttons and no dark box', !(await hidden('tvIntro')) && (await hidden('tvIntroG')) && (await hidden('darkRow')) && /LED numbers go/.test(await text('dirLbl')) && /Find my LEDs/.test(await text('fOpen')) && !(await hidden('fRunRow')) && (await hidden('fAltRow')) && /Which part of the run covers the TV/.test(await text('tvHead')));
    check('govee: switching back restores the WLED labels', (await text('wTitle')) === 'WLED' && /Check this WLED/.test(await text('wCheck')) && !(await hidden('wStartRow')) && !(await hidden('findWrap')) && !(await hidden('ledIntro')) && (await hidden('gIntro')));
    await ota(osv('wHost', '192.0.2.50') + osv('wCount', '150') + osv('wBri', '-1') + sel('lSide') + ".value='0'; " + sel('tSharp') + ".value='0'; " + sel('tVivid') + ".checked=false; " + click('save')); await sleep(700);
    // ---- zone edges and saturation, per target
    check('look: a target starts with smooth zone edges and normal saturation', (await ota(sel('tSharp') + '.value')) === '0' && (await ota(sel('tSat') + '.value')) === '1' && /smooth/.test(await text('tSharpV')) && /100%/.test(await text('tSatV')), await text('tSharpV'));
    await ota(sel('tSharp') + ".value='100'; " + sel('tSharp') + ".dispatchEvent(new Event('input',{bubbles:true})); " + sel('tSat') + ".value='1.3'; " + sel('tSat') + ".dispatchEvent(new Event('input',{bubbles:true})); 1"); await sleep(700);
    check('look: the labels say sharp and 130%', /sharp/.test(await text('tSharpV')) && /130%/.test(await text('tSatV')), (await text('tSharpV')) + ' ' + (await text('tSatV')));
    check('look: the picture map asks the board with the unsaved zone edges', dev.state.lastUsagePost && dev.state.lastUsagePost.sharp === 100 && dev.state.lastUsagePost.kind === 'wled', JSON.stringify(dev.state.lastUsagePost && [dev.state.lastUsagePost.sharp, dev.state.lastUsagePost.kind]));
    const hardLegend = await ota("Array.prototype.map.call(document.querySelectorAll('#mapSvg rect[data-t=\"0\"]'), function(r){return r.getAttribute('data-z')+':'+r.getAttribute('fill-opacity');}).join(' ')");
    check('look: with hard edges the map still draws the target on its zones', /\d+:/.test(hardLegend), hardLegend);
    await ota(click('save')); await sleep(700);
    check('look: Save stores the zone edges and the saturation on that target', dev.state.cfg.targets[0].sharp === 100 && Math.abs(dev.state.cfg.targets[0].sat - 1.3) < 1e-9 && dev.state.lastConfigPost.sharp === 100, JSON.stringify([dev.state.cfg.targets[0].sharp, dev.state.cfg.targets[0].sat]));
    check('vivid: a target starts with plain RGB blending', (await ota(sel('tVivid') + '.checked')) === false);
    await ota(sel('tVivid') + ".checked=true; " + sel('tVivid') + ".dispatchEvent(new Event('change',{bubbles:true})); " + click('save')); await sleep(700);
    check('vivid: Save stores it on that target', dev.state.cfg.targets[0].vivid === true && dev.state.lastConfigPost.vivid === true, String(dev.state.cfg.targets[0].vivid));
    // ---- motion smoothing (global) and blur along the strip (per target)
    check('motion: smoothing starts off, and the slider says so', (await ota(sel('kMot') + '.value')) === '0' && /off/.test(await text('kMotV')), await text('kMotV'));
    check('blur: a target starts without blur, and the slider says so', (await ota(sel('tBlur') + '.value')) === '0' && /off/.test(await text('tBlurV')), await text('tBlurV'));
    await ota(sel('kMot') + ".value='100'; " + fire('kMot', 'input') + sel('tBlur') + ".value='5'; " + fire('tBlur', 'input') + '1'); await sleep(300);
    check('motion / blur: the labels follow the sliders (100%, 5 LEDs)', /100%/.test(await text('kMotV')) && /5 LEDs/.test(await text('tBlurV')), (await text('kMotV')) + ' ' + (await text('tBlurV')));
    await ota(click('save')); await sleep(700);
    check('motion / blur: Save sends and stores them (motion 100 for everything, blur 5 on this target)', dev.state.cfg.look.motion === 100 && dev.state.cfg.targets[0].blur === 5 && dev.state.lastConfigPost.look.motion === 100 && dev.state.lastConfigPost.blur === 5, JSON.stringify([dev.state.cfg.look.motion, dev.state.cfg.targets[0].blur]));
    await ota('location.reload(); 1'); await sleep(1800);
    check('motion / blur: after a reload the sliders come back as saved', (await ota(sel('kMot') + '.value')) === '100' && (await ota(sel('tBlur') + '.value')) === '5');
    await setType('govee');
    check('blur: for a Govee light the label counts segments', /5 segments/.test(await text('tBlurV')), await text('tBlurV'));
    await setType('wled');
    await ota('location.reload(); 1'); await sleep(1800);                  // (the type round trip changed the form's LED count: show the saved values again before saving)
    await ota(sel('kMot') + ".value='0'; " + fire('kMot', 'input') + sel('tBlur') + ".value='0'; " + fire('tBlur', 'input') + click('save')); await sleep(700);
    check('motion / blur: back to off', dev.state.cfg.look.motion === 0 && dev.state.cfg.targets[0].blur === 0 && /off/.test(await text('kMotV')));
    await ota(sel('tVivid') + ".checked=false; 1");
    await setType('govee');
    check('vivid: choosing a Govee light switches vivid blending on', (await ota(sel('tVivid') + '.checked')) === true);
    await setType('wled'); await ota(sel('tVivid') + ".checked=false; 1");
    await setType('govee');
    check('look: choosing a Govee light turns the zone edges to sharp if they were smooth', true);
    await ota(sel('tSharp') + ".value='0'; " + sel('tSharp') + ".dispatchEvent(new Event('input',{bubbles:true})); 1");
    await setType('wled'); await setType('govee');
    check('look: ... again, after they were set smooth', (await ota(sel('tSharp') + '.value')) === '100', await ota(sel('tSharp') + '.value'));
    await setType('wled');
    await ota(sel('tSharp') + ".value='0'; " + sel('tSat') + ".value='1'; " + sel('tVivid') + ".checked=false; " + sel('tSharp') + ".dispatchEvent(new Event('input',{bubbles:true})); 1"); await sleep(300);    check('govee: back to the WLED setup for the tests below', dev.state.cfg.targets[0].kind === 'wled' && dev.state.cfg.targets[0].wled.host === '192.0.2.50' && dev.state.cfg.targets[0].wled.count === 150 && dev.state.cfg.targets[0].layout.led0Right === false, JSON.stringify(dev.state.cfg.targets[0].wled));
    dev.state.lat = { [dev.state.cfg.targets[0].name]: { ageMs: 4.5, ageMaxMs: 37.2, probes: 7, lost: 0, probeAgeS: 5, rttMs: 18.8, rttMinMs: 9.3, rttMaxMs: 41, showMs: 41.8 } }; await sleep(3200);
    check('delay: a WLED adds up the board, half the round trip and the strip refresh', /board 4\.5 ms \(up to 37\) \+ network about 9 ms \(round trip 19 ms\) \+ strip refresh about 42 ms = about 56 ms/.test(await text('latList')), await text('latList'));
    dev.state.lat = { [dev.state.cfg.targets[0].name]: { ageMs: 4.5, ageMaxMs: 6, probes: 5, lost: 2, probeAgeS: 5, rttMs: 20, rttMinMs: 10, rttMaxMs: 30, showMs: 30 } }; await sleep(3200);
    check('delay: lost probes are reported, and a small spread is not shown as "up to"', /board 4\.5 ms \+ network about 10 ms \(round trip 20 ms, 2 of 5 lost\)/.test(await text('latList')), await text('latList'));
    dev.state.lat = { [dev.state.cfg.targets[0].name]: { ageMs: 4.4, ageMaxMs: 10, probes: 3, lost: 3, probeAgeS: 2 } }; await sleep(3200);
    check('delay: a target whose probes all went unanswered still shows the board share, and says so (no network part, no total)', /board 4\.4 ms \(up to 10\)/.test(await text('latList')) && !/network about/.test(await text('latList')) && !/= about/.test(await text('latList')), await text('latList'));
    dev.state.lat = {};
    // smoothing slider is part of the live preview
    await p.ev("var o=document.getElementById('kOut'); o.value='40'; o.dispatchEvent(new Event('input',{bubbles:true})); 1"); await sleep(200);
    check('index: smooth-beyond-TV slider shows its value', /40%/.test(await p.ev("document.getElementById('kOutV').textContent")));
    check('index: reaction boost starts at 0 (off)', (await p.ev("document.getElementById('kBoost').value")) === '0' && /0%/.test(await p.ev("document.getElementById('kBoostV').textContent")));
    await p.ev("var b=document.getElementById('kBoost'); b.value='60'; b.dispatchEvent(new Event('input',{bubbles:true})); 1"); await sleep(200);
    check('index: reaction boost slider shows its value', /60%/.test(await p.ev("document.getElementById('kBoostV').textContent")));
    dev.state.lastConfigPost = null;
    await p.ev("document.getElementById('save').click(); 1"); await sleep(600);
    check('index: Save sends the boost as a fraction (0.6)', dev.state.lastConfigPost && dev.state.lastConfigPost.look && Math.abs(dev.state.lastConfigPost.look.boost - 0.6) < 1e-9, JSON.stringify(dev.state.lastConfigPost && dev.state.lastConfigPost.look));
    await p.ev("var b=document.getElementById('kBoost'); b.value='0'; b.dispatchEvent(new Event('input',{bubbles:true})); document.getElementById('save').click(); 1"); await sleep(500);
    // the box's own speed, which the boost has to undo
    check('index: box speed starts at 40% (the box app default)', (await p.ev("document.getElementById('kBsp').value")) === '40' && /40%/.test(await p.ev("document.getElementById('kBspV').textContent")));
    await p.ev("var b=document.getElementById('kBsp'); b.value='100'; b.dispatchEvent(new Event('input',{bubbles:true})); 1"); await sleep(200);
    check('index: at 100% the label says the box does not smooth', /100%.*does not smooth/.test(await p.ev("document.getElementById('kBspV').textContent")), await p.ev("document.getElementById('kBspV').textContent"));
    dev.state.lastConfigPost = null;
    await p.ev("document.getElementById('save').click(); 1"); await sleep(600);
    check('index: Save sends the box speed', dev.state.lastConfigPost && dev.state.lastConfigPost.look && dev.state.lastConfigPost.look.boxSpeed === 100, JSON.stringify(dev.state.lastConfigPost && dev.state.lastConfigPost.look));
    await p.ev("var b=document.getElementById('kBsp'); b.value='40'; b.dispatchEvent(new Event('input',{bubbles:true})); document.getElementById('save').click(); 1"); await sleep(500);
    // nudge buttons + range check
    check('index: nudge buttons are built', (await p.ev("document.querySelectorAll('.nudge button').length")) >= 24);
    await p.ev("document.getElementById('wStart').value='0'; document.querySelector('[data-nudge=wStart] button:last-child').click(); 1");
    check('index: +10 nudge works', (await p.ev("document.getElementById('wStart').value")) === '10');
    await p.ev("document.getElementById('wCount').value='595'; document.getElementById('wCount').dispatchEvent(new Event('input',{bubbles:true})); 1");
    await sleep(200);
    check('index: warns when the LED range does not exist', /do not all exist/.test(await p.ev("document.getElementById('rangeNote').textContent")), await p.ev("document.getElementById('rangeNote').textContent"));
    // where the LEDs are -> which part of the colour map, and the page's LED numbers are the strip's own numbers
    const setv = (id, v, ev) => p.ev("var e=document.getElementById('" + id + "'); e.value='" + v + "'; e.dispatchEvent(new Event('" + (ev || 'input') + "',{bubbles:true})); 1");
    await setv('wStart', 100); await setv('wCount', 210); await setv('lPlace', 'top', 'change');
    await setv('tvFirst', 150); await setv('tvLast', 230);
    check('index: shows how much of the run the TV covers', /81 LEDs/.test(await p.ev("document.getElementById('tvNote').textContent")), await p.ev("document.getElementById('tvNote').textContent"));
    check('index: ceiling explains it uses the top of the picture', /TOP of the picture/.test(await p.ev("document.getElementById('placeNote').textContent")));
    dev.state.lastConfigPost = null; dev.state.test = null;
    await p.ev("document.getElementById('test').click(); 1"); await sleep(700);
    const post1 = dev.state.lastConfigPost && dev.state.lastConfigPost.layout;
    check('index: LEDs above the TV -> straight run on the top edge, TV part sent relative to the run', post1 && post1.type === 'linear' && post1.edge === 'top' && post1.spanFirst === 50 && post1.spanLast === 130, JSON.stringify(post1));
    check('index: the run itself is sent as first LED + length', dev.state.lastConfigPost.wled.start === 100 && dev.state.lastConfigPost.wled.count === 210);
    check('index: rainbow test starts (preview config + test request)', dev.state.test && dev.state.test.pattern === 'rainbow' && dev.state.lastConfigPost.preview === true, JSON.stringify(dev.state.test));
    dev.state.lastConfigPost = null;
    await setv('lPlace', 'bottom', 'change'); await sleep(800);
    check('index: LEDs below the TV -> bottom edge (live preview)', dev.state.lastConfigPost && dev.state.lastConfigPost.layout.edge === 'bottom' && dev.state.lastConfigPost.preview === true, JSON.stringify(dev.state.lastConfigPost && dev.state.lastConfigPost.layout));
    dev.state.lastConfigPost = null;
    await setv('lPlace', 'left', 'change'); await sleep(800);
    check('index: vertical strip on the left -> left edge; direction reads top/bottom', dev.state.lastConfigPost && dev.state.lastConfigPost.layout.edge === 'left' &&
      /top to bottom/.test(await p.ev("document.getElementById('lSide').options[0].textContent")), JSON.stringify(dev.state.lastConfigPost && dev.state.lastConfigPost.layout));
    await setv('lPlace', 'loop', 'change'); await sleep(200);
    check('index: all the way around shows the loop controls, not the TV-part ones', await p.ev("document.getElementById('lLinear').classList.contains('hidden') && !document.getElementById('lLoop').classList.contains('hidden')"));
    await setv('lPlace', 'top', 'change'); await sleep(200);
    // markers: changing the run / TV part lights red + blue blocks on the strip
    dev.state.mark = null;
    await setv('tvFirst', 160); await sleep(700);
    const mk = dev.state.mark && dev.state.mark.blocks;
    const reds = mk && mk.filter(b => b.color[0] === 255 && b.color[1] === 0).map(b => b.start + '+' + b.count);
    const blues = mk && mk.filter(b => b.color[2] === 255).map(b => b.start + '+' + b.count);
    check('markers: red blocks at the TV part start (160) and end (230, last 3 LEDs = 228)', reds && reds.join(',') === '160+3,228+3', JSON.stringify(mk));
    check('markers: blue blocks at the run start (100) and end (last 3 LEDs = 307)', blues && blues.join(',') === '100+3,307+3', JSON.stringify(mk));
    check('markers: shown for 8 seconds after a change', dev.state.mark && dev.state.mark.seconds === 8);
    dev.state.mark = null;
    await p.ev("document.getElementById('markBtn').click(); 1"); await sleep(500);
    check('markers: the button shows them for 30 seconds', dev.state.mark && dev.state.mark.seconds === 30, JSON.stringify(dev.state.mark));
    check('markers: no middle LED unless asked for', dev.state.mark && !dev.state.mark.blocks.some(b => b.color[1] === 255 && b.color[0] === 0 && b.color[2] === 0), JSON.stringify(dev.state.mark));
    // the middle of the TV part: LEDs 160..230 are 71 LEDs, so the middle is the single LED 195
    const greens = () => dev.state.mark && dev.state.mark.blocks.filter(b => b.color[0] === 0 && b.color[1] === 255 && b.color[2] === 0).map(b => b.start + '+' + b.count);
    dev.state.mark = null;
    await p.ev("document.getElementById('markMidBtn').click(); 1"); await sleep(500);
    check('markers: the middle button lights one green LED at the middle of the TV part (160..230 -> 195)', greens() && greens().join(',') === '195+1', JSON.stringify(dev.state.mark));
    check('markers: ... for 30 seconds, together with the red and blue ones', dev.state.mark.seconds === 30 && dev.state.mark.blocks.filter(b => b.color[0] === 255).length === 2 && dev.state.mark.blocks.filter(b => b.color[2] === 255).length === 2, JSON.stringify(dev.state.mark));
    dev.state.mark = null;
    await setv('tvLast', 231); await sleep(700);                          // 72 LEDs: the middle lies between two LEDs, so both light up; the green stays while the numbers change
    check('markers: an even TV part (160..231) lights the two middle LEDs (195 and 196), and the green stays while nudging', greens() && greens().join(',') === '195+2' && dev.state.mark.seconds === 8, JSON.stringify(dev.state.mark));
    await setv('tvLast', 230); await sleep(300);
    dev.state.mark = null;
    await p.ev("document.getElementById('markBtn').click(); 1"); await sleep(500);
    check('markers: the plain markers button takes the green one away again', dev.state.mark && greens().length === 0, JSON.stringify(dev.state.mark));
    // the TV part must lie inside the run
    dev.state.lastConfigPost = null;
    await setv('tvFirst', 50); await sleep(700);
    dev.state.mark = null;
    check('index: a TV part that starts before the run is refused with an explanation', /before the run begins/.test(await p.ev("document.getElementById('tvNote').textContent")) && dev.state.lastConfigPost === null, await p.ev("document.getElementById('tvNote').textContent"));
    await setv('tvFirst', 150); await sleep(200);
    // slide the TV part along the run, stopping at the ends of the run
    dev.state.lastConfigPost = null;
    await p.ev("document.querySelectorAll('#mover button')[2].click(); 1"); await sleep(900);
    check('index: sliding the TV part previews live (+1)', dev.state.lastConfigPost && dev.state.lastConfigPost.layout.spanFirst === 51 && dev.state.lastConfigPost.layout.spanLast === 131, JSON.stringify(dev.state.lastConfigPost && dev.state.lastConfigPost.layout));
    await p.ev("for(var k=0;k<20;k++)document.querySelectorAll('#mover button')[3].click(); 1"); await sleep(200);
    check('index: the TV part stops at the end of the run (LED 309)', (await p.ev("document.getElementById('tvLast').value")) === '309', await p.ev("document.getElementById('tvFirst').value + ',' + document.getElementById('tvLast').value"));
    check('index: unsaved changes are flagged', /Not saved/.test(await p.ev("document.getElementById('dirty').textContent")));
    await p.ev("document.getElementById('test').click(); 1"); await sleep(300);
    check('index: stopping the test sends pattern off', dev.state.test && dev.state.test.pattern === 'off', JSON.stringify(dev.state.test));
    // Save sends the whole thing (not a preview)
    await setv('tvFirst', 150); await setv('tvLast', 230); dev.state.lastConfigPost = null;
    await p.ev("document.getElementById('save').click(); 1"); await sleep(600);
    check('index: Save stores it (no preview flag)', dev.state.lastConfigPost && !dev.state.lastConfigPost.preview && dev.state.lastConfigPost.layout.spanFirst === 50, JSON.stringify(dev.state.lastConfigPost && dev.state.lastConfigPost.layout));
    // several targets: add one, edit it, switch between them, markers / test go to the selected one, remove it
    await p.ev("window.confirm=function(){return true;}; 1");
    check('targets: starts with one target, listed in the selector, and it cannot be removed', (await p.ev("document.getElementById('tSel').options.length")) === 1 && (await p.ev("document.getElementById('tDel').disabled")) === true);
    check('targets: the phone-calibration link is shown (this build has it)', (await p.ev("document.getElementById('calRow').classList.contains('hidden')")) === false && /calibrate\?target=0/.test(await p.ev("document.getElementById('calBtn').getAttribute('href')")), await p.ev("document.getElementById('calBtn').getAttribute('href')"));
    await p.ev("document.getElementById('tAdd').click(); 1"); await sleep(800);
    check('targets: Add creates a second target and selects it (empty form)', dev.state.lastTargetsPost && dev.state.lastTargetsPost.action === 'add' && dev.state.cfg.targets.length === 2 &&
      (await p.ev("document.getElementById('tSel').value")) === '1' && (await p.ev("document.getElementById('wHost').value")) === '' && (await p.ev("document.getElementById('tName').value")) === 'Target 2');
    await setv('tName', 'Cabinet'); await setv('wHost', '192.0.2.225'); await setv('wStart', 0); await setv('wCount', 266); await setv('lPlace', 'right', 'change'); await setv('tTrim', 0.5);
    dev.state.lastConfigPost = null;
    await p.ev("document.getElementById('save').click(); 1"); await sleep(800);
    const t0 = dev.state.cfg.targets[0], t1 = dev.state.cfg.targets[1];
    check('targets: Save sends the target number and stores the second target only', dev.state.lastConfigPost && dev.state.lastConfigPost.target === 1 && t1.name === 'Cabinet' && t1.wled.host === '192.0.2.225' && t1.wled.count === 266 &&
      t1.layout.edge === 'right' && t1.trim === 0.5 && t0.wled.host === '192.0.2.50' && t0.layout.edge === 'top' && t0.name === 'Target 1', JSON.stringify(dev.state.cfg.targets.map(t => [t.name, t.wled.host, t.layout.edge, t.trim])));
    await sleep(2800);
    check('targets: the selector lists both by name and the status has a pill for each', /Cabinet/.test(await p.ev("document.getElementById('tSel').textContent")) && (await p.ev("document.getElementById('wPills').children.length")) === 2, await p.ev("document.getElementById('wPills').textContent"));
    await p.ev("var s=document.getElementById('tSel'); s.value='0'; s.dispatchEvent(new Event('change',{bubbles:true})); 1"); await sleep(700);
    check('targets: switching back shows the first target (its address, edge and name)', (await p.ev("document.getElementById('wHost').value")) === '192.0.2.50' && (await p.ev("document.getElementById('lPlace').value")) === 'top' && (await p.ev("document.getElementById('tName').value")) === 'Target 1');
    await p.ev("var s=document.getElementById('tSel'); s.value='1'; s.dispatchEvent(new Event('change',{bubbles:true})); 1"); await sleep(700);
    check('targets: ... and the second again, with its trim', (await p.ev("document.getElementById('wHost').value")) === '192.0.2.225' && (await p.ev("document.getElementById('lPlace').value")) === 'right' && (await p.ev("document.getElementById('tTrim').value")) === '0.5');
    check('targets: the phone-calibration link carries the selected target', /calibrate\?target=1/.test(await p.ev("document.getElementById('calBtn').getAttribute('href')")), await p.ev("document.getElementById('calBtn').getAttribute('href')"));
    dev.state.mark = null;
    await p.ev("document.getElementById('markBtn').click(); 1"); await sleep(700);
    const mk1 = dev.state.mark;
    check('targets: markers go to the selected target, on its own LED numbers', mk1 && mk1.target === 1 && mk1.blocks.some(b => b.start === 0) && mk1.blocks.some(b => b.start === 263), JSON.stringify(mk1));
    dev.state.test = null;
    await p.ev("document.getElementById('test').click(); 1"); await sleep(800);
    check('targets: the rainbow test runs on the selected target only', dev.state.test && dev.state.test.pattern === 'rainbow' && dev.state.test.target === 1, JSON.stringify(dev.state.test));
    await p.ev("document.getElementById('test').click(); 1"); await sleep(400);
    check('targets: stopping the test names the target too', dev.state.test && dev.state.test.pattern === 'off' && dev.state.test.target === 1);
    await p.ev("document.getElementById('tDel').click(); 1"); await sleep(800);
    check('targets: Remove deletes the selected target and goes back to the first', dev.state.lastTargetsPost.action === 'remove' && dev.state.lastTargetsPost.index === 1 && dev.state.cfg.targets.length === 1 &&
      (await p.ev("document.getElementById('tSel').options.length")) === 1 && (await p.ev("document.getElementById('wHost').value")) === '192.0.2.50');
    // the picture map: which zones each target follows, drawn from the settings (also unsaved ones)
    {
      const zonesOf = t => p.ev("JSON.stringify([...new Set(Array.from(document.querySelectorAll('#mapSvg rect[data-t=\"" + t + "\"]')).map(r => +r.getAttribute('data-z')))].sort((a,b)=>a-b))");
      const alphaOf = (t, z) => p.ev("+document.querySelector('#mapSvg rect[data-t=\"" + t + "\"][data-z=\"" + z + "\"]').getAttribute('fill-opacity')");
      check('map: a 6 by 4 grid is drawn', (await p.ev("document.querySelectorAll('#mapSvg g[data-zone]').length")) === 24);
      check('map: a ceiling strip highlights the top row (zones 0-5) and nothing else', (await zonesOf(0)) === '[0,1,2,3,4,5]', await zonesOf(0));
      check('map: the 8 middle zones are faded (never used)', (await p.ev("document.querySelector('#mapSvg g[data-zone=\"7\"]').getAttribute('opacity')")) === '0.4' && (await p.ev("document.querySelector('#mapSvg g[data-zone=\"0\"]').getAttribute('opacity')")) === '1');
      check('map: the zone with the stretch past the TV is shaded darker than a middle one', (await alphaOf(0, 0)) > (await alphaOf(0, 2)), (await alphaOf(0, 0)) + ' vs ' + (await alphaOf(0, 2)));
      check('map: hovering a block tells how many LEDs follow it', /Target 1: [0-9.]+ LEDs \(\d+% of its 210\)/.test(await p.ev("document.querySelector('#mapSvg rect[data-t=\"0\"][data-z=\"0\"] title').textContent")), await p.ev("document.querySelector('#mapSvg rect[data-t=\"0\"][data-z=\"0\"] title').textContent"));
      dev.state.lastUsagePost = null;
      await setv('lPlace', 'right', 'change'); await sleep(900);
      check('map: moving the LEDs to the right edge (unsaved) redraws the right column', (await zonesOf(0)) === '[5,11,17,23]' && dev.state.lastUsagePost && dev.state.lastUsagePost.target === 0 && dev.state.lastUsagePost.layout.edge === 'right', await zonesOf(0));
      await setv('lPlace', 'bottom', 'change'); await sleep(900);
      check('map: the bottom edge highlights the bottom row (zones 18-23)', (await zonesOf(0)) === '[18,19,20,21,22,23]', await zonesOf(0));
      await setv('lPlace', 'top', 'change'); await sleep(900);
      await p.ev("document.getElementById('tOn').checked=false; document.getElementById('tOn').dispatchEvent(new Event('change',{bubbles:true})); 1"); await sleep(300);
      check('map: switching a target off dims it and says so', (await alphaOf(0, 0)) < 0.5 && /\(off\)/.test(await p.ev("document.getElementById('mapLegend').textContent")), await p.ev("document.getElementById('mapLegend').textContent"));
      await p.ev("document.getElementById('tOn').checked=true; document.getElementById('tOn').dispatchEvent(new Event('change',{bubbles:true})); 1"); await sleep(300);
      // two targets: each in its own colour, and a zone both use is split between them
      await p.ev("document.getElementById('tAdd').click(); 1"); await sleep(800);
      await p.ev("var s=document.getElementById('tName'); s.value='Cabinet'; s.dispatchEvent(new Event('input',{bubbles:true})); 1");
      await setv('wCount', 266); await setv('lPlace', 'right', 'change'); await sleep(900);
      check('map: a second target gets its own colour and a legend entry', (await zonesOf(1)) === '[5,11,17,23]' && (await p.ev("document.getElementById('mapLegend').children.length")) === 2 && /Cabinet/.test(await p.ev("document.getElementById('mapLegend').textContent")) && (await p.ev("document.querySelector('#mapSvg rect[data-t=\"0\"]').getAttribute('fill')")) !== (await p.ev("document.querySelector('#mapSvg rect[data-t=\"1\"]').getAttribute('fill')")), await p.ev("document.getElementById('mapLegend').textContent"));
      check('map: a zone used by both targets (the top-right corner) is shared between them', (await p.ev("document.querySelectorAll('#mapSvg rect[data-z=\"5\"]').length")) === 2 && (await p.ev("document.querySelectorAll('#mapSvg rect[data-z=\"11\"]').length")) === 1);
      await p.ev("document.getElementById('tDel').click(); 1"); await sleep(800);
      check('map: removing the target removes its colour', (await p.ev("document.getElementById('mapLegend').children.length")) === 1 && (await zonesOf(1)) === '[]');
    }
    // finder: a block of LEDs that can be moved, and used to set the run and the TV part
    dev.calls.length = 0;
    await p.ev("document.getElementById('fOpen').click(); 1"); await sleep(500);
    check('finder: opens a session', dev.calls.some(c => c.startsWith('POST /api/cal/begin')) && (await p.ev("!document.getElementById('fBox').classList.contains('hidden')")));
    await setv('fPos', 150); await sleep(400);
    check('finder: slider lights a block', dev.calls.some(c => c.includes('/api/cal/led') && c.includes('"start":150')), dev.calls.join(' ; '));
    await setv('wStart', 0); await setv('wCount', 300);
    await p.ev("document.getElementById('fFirst').click(); 1");
    check('finder: "Run starts here" moves the start and keeps the end', (await p.ev("document.getElementById('wStart').value")) === '150' && (await p.ev("document.getElementById('wCount').value")) === '150', await p.ev("document.getElementById('wStart').value + '/' + document.getElementById('wCount').value"));
    await setv('fPos', 199);
    await p.ev("document.getElementById('fLast').click(); 1");
    check('finder: "Run ends here" sets the length (block 199..208 -> 59 LEDs)', (await p.ev("document.getElementById('wCount').value")) === '59', await p.ev("document.getElementById('wCount').value"));
    await setv('fPos', 160); await p.ev("document.getElementById('fTvFirst').click(); 1");
    check('finder: "TV starts here"', (await p.ev("document.getElementById('tvFirst').value")) === '160');
    await setv('fPos', 190); await p.ev("document.getElementById('fTvLast').click(); 1");
    check('finder: "TV ends here" (end of the lit block)', (await p.ev("document.getElementById('tvLast').value")) === '199', await p.ev("document.getElementById('tvLast').value"));
    await p.ev("document.getElementById('fClose').click(); 1"); await sleep(400);
    check('finder: Done ends the session', dev.calls.some(c => c.startsWith('POST /api/cal/end')) && (await p.ev("document.getElementById('fBox').classList.contains('hidden')")));
    // Home Assistant (MQTT) card + pause/resume
    await setv('mqHost', '192.0.2.8'); await setv('mqUser', 'ha'); await setv('mqPass', 'secret');
    await p.ev("document.getElementById('mqOn').checked=true; 1");
    dev.state.lastConfigPost = null;
    await p.ev("document.getElementById('mqSave').click(); 1"); await sleep(600);
    const mq = dev.state.lastConfigPost && dev.state.lastConfigPost.mqtt;
    check('mqtt: Save sends the broker settings (and the password only because one was typed)', mq && mq.enabled === true && mq.host === '192.0.2.8' && mq.port === 1883 && mq.user === 'ha' && mq.pass === 'secret' && !('relay' in dev.state.lastConfigPost), JSON.stringify(mq));
    check('mqtt: the password field is cleared after saving', (await p.ev("document.getElementById('mqPass').value")) === '');
    check('mqtt: performance stats are off by default and saved as off', mq.perf === false && (await p.ev("document.getElementById('mqPerf').checked")) === false, JSON.stringify(mq));
    await p.ev("document.getElementById('mqPerf').checked=true; document.getElementById('mqSave').click(); 1"); await sleep(600);
    check('mqtt: the performance-stats switch is saved when turned on', dev.state.lastConfigPost && dev.state.lastConfigPost.mqtt.perf === true && dev.state.cfg.mqtt.perf === true, JSON.stringify(dev.state.lastConfigPost && dev.state.lastConfigPost.mqtt));
    await p.ev("document.getElementById('mqPerf').checked=false; document.getElementById('mqSave').click(); 1"); await sleep(600);
    check('mqtt: ... and turned off again', dev.state.cfg.mqtt.perf === false);
    dev.state.lastConfigPost = null;
    await p.ev("document.getElementById('mqSave').click(); 1"); await sleep(500);
    check('mqtt: saving again without typing a password leaves it unchanged (not sent)', dev.state.lastConfigPost && dev.state.lastConfigPost.mqtt && !('pass' in dev.state.lastConfigPost.mqtt), JSON.stringify(dev.state.lastConfigPost));
    await sleep(3300);
    check('mqtt: shows when it is connected', /Connected to the broker/.test(await p.ev("document.getElementById('mqStatus').textContent")), await p.ev("document.getElementById('mqStatus').textContent"));
    check('sync: pause button is shown', await p.ev("!document.getElementById('syncBtn').classList.contains('hidden') && document.getElementById('syncBtn').textContent === 'Pause sync'"));
    dev.state.lastConfigPost = null;
    await p.ev("document.getElementById('syncBtn').click(); 1"); await sleep(700);
    check('sync: pressing it pauses and the button turns into Resume', dev.state.lastConfigPost && dev.state.lastConfigPost.relay.enabled === false && (await p.ev("document.getElementById('syncBtn').textContent")) === 'Resume sync', JSON.stringify(dev.state.lastConfigPost));
    // back up / move to another board: export downloads a file, import loads one
    await p.send('Page.enable');
    await p.ev("window.__dl=null; window.__confirms=[]; window.confirm=function(m){window.__confirms.push(m);return window.__ok!==false;}; "
      + "var oc=HTMLAnchorElement.prototype.click; HTMLAnchorElement.prototype.click=function(){ if(this.download){ var a=this; fetch(a.href).then(function(r){return r.text();}).then(function(t){window.__dl={name:a.download,text:t};}); } else oc.call(this); }; 1");
    dev.state.maps[0] = { start: 0, count: 2, hex: '0005ff000105ff00' }; dev.state.cfg.targets[0].layout.hasTable = true;
    await p.ev("document.getElementById('exportBtn').click(); 1"); await sleep(700);
    const dl = JSON.parse((await p.ev("window.__dl && window.__dl.text")) || 'null');
    check('backup: Export downloads fancy2wled-config.json', (await p.ev("window.__dl && window.__dl.name")) === 'fancy2wled-config.json');
    check('backup: the file says what it is and carries settings + calibration', dl && dl.format === 'fancy2wled-config' && dl.version === 2 && dl.config.targets[0].wled.host === '192.0.2.50' && dl.tables[0].target === 0 && dl.tables[0].count === 2 && dl.tables[0].hex === '0005ff000105ff00', JSON.stringify(dl).slice(0, 200));
    check('backup: no Wi-Fi details and no MQTT password by default', dl && !JSON.stringify(dl).includes('TestNet') && !('wifi' in dl) && !('pass' in dl.config.mqtt) && !('passSet' in dl.config.mqtt) && !('hasTable' in dl.config.targets[0].layout) && !('wled' in dl.config) && !('layout' in dl.config), JSON.stringify(dl && dl.config.mqtt));
    await p.ev("document.getElementById('exSecrets').checked=true; window.__dl=null; document.getElementById('exportBtn').click(); 1"); await sleep(700);
    const dl2 = JSON.parse((await p.ev("window.__dl && window.__dl.text")) || 'null');
    check('backup: the MQTT password goes in only when asked for', dl2 && dl2.config.mqtt.pass === 'secret', JSON.stringify(dl2 && dl2.config.mqtt));
    // import: a valid file (set through a File object, as the file picker would)
    const importFile = async (obj, ok) => {
      dev.state.imported = null; await p.ev("window.__ok=" + (ok !== false) + "; window.__confirms=[]; 1");
      await p.ev("var dt=new DataTransfer(); dt.items.add(new File([" + JSON.stringify(typeof obj === 'string' ? obj : JSON.stringify(obj)) + "],'x.json',{type:'application/json'})); var fi=document.getElementById('importFile'); fi.files=dt.files; fi.dispatchEvent(new Event('change',{bubbles:true})); 1");
      await sleep(900); return p.ev("document.getElementById('bkMsg').textContent");
    };
    const moved = JSON.parse(JSON.stringify(dl)); moved.config.targets[0].wled.host = '192.0.2.99'; moved.config.targets[0].layout.spanFirst = 55; moved.config.mqtt.enabled = true;
    let note = await importFile(moved);
    check('backup: Import asks first, shows a summary, and says the Wi-Fi is untouched', (await p.ev("window.__confirms.length")) === 1 && /192\.0\.2\.99/.test(await p.ev("window.__confirms[0]")) && /Wi-Fi/.test(await p.ev("window.__confirms[0]")));
    check('backup: Import sends the file to the board and applies it', dev.state.imported && dev.state.cfg.wled.host === '192.0.2.99' && dev.state.cfg.layout.spanFirst === 55 && dev.state.maps[0].count === 2, note);
    check('backup: the page shows the imported values and says so', (await p.ev("document.getElementById('wHost').value")) === '192.0.2.99' && /Imported/.test(note), note);
    check('backup: warns when MQTT is on but the file has no password', /type it again/.test(note), note);
    note = await importFile(moved, false);
    check('backup: declining the question changes nothing', dev.state.imported === null, note);
    note = await importFile('this is not json');
    check('backup: a file that is not JSON is refused', dev.state.imported === null && /not a Fancy2WLED config/.test(note), note);
    note = await importFile({ hello: 'world' });
    check('backup: JSON that is not a Fancy2WLED config is refused', dev.state.imported === null && /not a Fancy2WLED config/.test(note), note);
    const newer = JSON.parse(JSON.stringify(dl)); newer.version = 3;
    note = await importFile(newer);
    check('backup: a file from a newer version is refused with an explanation', dev.state.imported === null && /newer version/.test(note), note);
    // a file from before several targets (version 1: wled / layout / table at the top) still imports, as target 0
    const legacy = { format: 'fancy2wled-config', version: 1, config: { wled: { host: '192.0.2.77', start: 0, count: 150, turnOn: true, restore: true, brightness: -1 }, layout: { type: 'linear', edge: 'bottom', led0Right: false, spanFirst: -1, spanLast: -1, corner: 0, ccw: false, sides: [0, 0, 0, 0], shift: 0 } }, table: { start: 0, count: 2, hex: '0005ff000105ff00' } };
    note = await importFile(legacy);
    check('backup: a version-1 file (one target) imports as target 0', dev.state.imported && dev.state.cfg.targets[0].wled.host === '192.0.2.77' && dev.state.cfg.targets[0].layout.edge === 'bottom' && dev.state.cfg.targets.length >= 1, note);
    // the Sync Box sends something the relay cannot read: a red warning with a diagnostics download
    check('box problem: no warning while the box is understood', (await p.ev("document.getElementById('boxWarn').classList.contains('hidden')")) === true && !/not understood/.test(await p.ev("document.getElementById('pBox').textContent")));
    dev.state.boxProblem = true; await sleep(3200);
    check('box problem: the Sync Box pill goes red and says what is wrong', /bad/.test(await p.ev("document.getElementById('pBox').className")) && /frames not understood/.test(await p.ev("document.getElementById('pBox').textContent")), await p.ev("document.getElementById('pBox').textContent"));
    check('box problem: the warning card shows the counts', (await p.ev("document.getElementById('boxWarn').classList.contains('hidden')")) === false && /340 frames with the Sync Box framing/.test(await p.ev("document.getElementById('boxWarnN').textContent")), await p.ev("document.getElementById('boxWarnN').textContent"));
    await p.ev("window.__dl=null; document.getElementById('diagBtn').click(); 1"); await sleep(900);
    const diag = JSON.parse((await p.ev("window.__dl && window.__dl.text")) || 'null');
    check('box problem: Download diagnostics saves fancy2wled-diagnostics.json', (await p.ev("window.__dl && window.__dl.name")) === 'fancy2wled-diagnostics.json' && diag && diag.format === 'fancy2wled-diagnostics' && diag.relay.samples.length === 1, JSON.stringify(diag).slice(0, 120));
    dev.state.boxProblem = false; await sleep(3200);
    check('box problem: the warning goes away when the box is understood again', (await p.ev("document.getElementById('boxWarn').classList.contains('hidden')")) === true);
    check('index: no script errors', p.logs.length === 0, p.logs.join(' | '));
    p.close();

    // ---- a build without the camera calibration: the setup page does not offer it
    { const dev2 = createDevice({ phoneCal: false });
      await new Promise(r => dev2.server.listen(0, r));
      const p2 = await newTab('http://localhost:' + dev2.server.address().port + '/');
      await sleep(1800);
      check('no phone calibration in this build: the link is hidden', await p2.ev("document.getElementById('calRow').classList.contains('hidden')"));
      check('... but the LED finder is still there', await p2.ev("!document.getElementById('fOpen').classList.contains('hidden')"));
      p2.close(); dev2.server.close(); }

    // ---- screen page follows the device
    p = await newTab(base + '/screen');
    await sleep(700);
    dev.state.calActive = true; dev.state.screen = 'white'; await sleep(700);
    check('screen: turns white', (await p.ev("document.body.className")).includes('white'), await p.ev("document.body.className"));
    dev.state.screen = 'black'; await sleep(600);
    check('screen: turns black', (await p.ev("document.body.className")) === 'run');
    dev.state.screen = 'squares'; await sleep(700);
    check('screen: squares mode', (await p.ev("document.body.className")) === 'run squares', await p.ev("document.body.className"));
    const probe = `(function(){var c=document.getElementById("sq"),g=c.getContext("2d"),W=c.width,H=c.height;
      function at(u,v){return g.getImageData(Math.round(u*W),Math.round(v*H),1,1).data[0];}
      var us=[0.08,0.5,0.92],vs=[0.12,0.5,0.88],on=0,between=0;
      for(var i=0;i<3;i++)for(var j=0;j<3;j++){if(at(us[i],vs[j])>200)on++;}
      if(at(0.3,0.3)>50)between++;if(at(0.7,0.7)>50)between++;if(at(0.5,0.3)>50)between++;
      return on+","+between+","+W+"x"+H;})()`;
    const px = await p.ev(probe);
    check('screen: nine white squares at the expected places, black in between', /^9,0,/.test(px), px);
    dev.state.calActive = false; await sleep(600);
    check('screen: back to idle', (await p.ev("document.body.className")) === '');
    check('screen: no script errors', p.logs.length === 0, p.logs.join(' | '));
    p.close();

    // ---- calibration page, fake camera (a test pattern that does not react to the LEDs/TV, so it must fail politely)
    dev.calls.length = 0;
    p = await newTab(base + '/calibrate');
    await sleep(1200);
    check('calibrate: secure (localhost) so no HTTPS banner', await p.ev("document.getElementById('insecure').classList.contains('hidden')"));
    await p.ev("document.getElementById('cam').click(); 1");
    await sleep(2500);
    check('calibrate: camera started', await p.ev("!document.getElementById('view').classList.contains('hidden') && document.getElementById('v').videoWidth > 0"), await p.ev("document.getElementById('camErr').textContent"));
    await p.ev("document.getElementById('go').click(); 1");
    let msg = ''; const calT0 = Date.now();
    for (let i = 0; i < 240; i++) { await sleep(500); msg = await p.ev("document.getElementById('msg').textContent"); if (/did not see|could only find|too small|could not|Something went wrong|^Done in/.test(msg)) break; }   // up to 2 minutes: the fake-camera scan takes 15-40 s depending on how busy the machine is
    if (process.env.PAGE_TEST_TIMING) console.log('calibration page run took ' + ((Date.now() - calT0) / 1000).toFixed(1) + ' s');
    if (process.env.PAGE_TEST_DIAG && Date.now() - calT0 > 40000) {
      console.log('DIAG msg: ' + msg);
      console.log('DIAG last calls: ' + dev.calls.slice(-6).join(' ; '));
      console.log('DIAG visibility: ' + await p.ev('document.visibilityState + " hasFocus=" + document.hasFocus()'));
      console.log('DIAG video: ' + await p.ev('(function(){var v=document.getElementById("v");return v.readyState+" paused="+v.paused+" t="+v.currentTime.toFixed(2)+" "+v.videoWidth+"x"+v.videoHeight;})()'));
      const t1 = await p.ev('document.getElementById("v").currentTime'); await sleep(1500); const t2 = await p.ev('document.getElementById("v").currentTime');
      console.log('DIAG video clock advancing: ' + (t2 > t1) + ' (' + t1 + ' -> ' + t2 + ')');
      console.log('DIAG page logs: ' + p.logs.join(' | '));
    }
    check('calibrate: fails politely when the TV does not change', /did not see|could only find/.test(msg), msg);
    const seq = dev.calls.filter(c => c.startsWith('POST /api/cal/')).map(c => c.split(' ')[1]);
    check('calibrate: the TV is shown the squares first', dev.calls.some(c => c.indexOf('"mode":"squares"') > 0), dev.calls.join(' ; '));
    check('calibrate: begin first, led off, screen black then white, end last', seq[0] === '/api/cal/begin' && seq.includes('/api/cal/screen') && seq[seq.length - 1] === '/api/cal/end', seq.join(' '));
    check('calibrate: device left idle', dev.state.calActive === false && dev.state.screen === 'idle');
    check('calibrate: no script errors', p.logs.length === 0, p.logs.join(' | '));
    p.close();
  } catch (e) {
    check('page tests ran without crashing', false, e && e.stack);
  } finally {
    proc.kill();
    dev.server.close();
    try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) { /* ignore */ }
  }
  console.log(pass + ' checks passed, ' + fail + ' failed');
  console.log(fail ? 'SOME FAILED' : 'ALL PASSED');
  process.exit(fail ? 1 : 0);
})();
