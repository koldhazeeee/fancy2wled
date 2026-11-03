// Source hygiene: the web UI and firmware sources must be plain ASCII with LF line endings and no BOM.
// (A stray non-ASCII character in an embedded page shows up as garbled text on the device.)
// Also: no real device addresses (private IPv4 / MAC) outside a few documented examples.
//   node test/test_lint.js
'use strict';
const fs = require('fs');
const path = require('path');

const ROOT = path.join(__dirname, '..');
const DIRS = ['web', 'src', 'include', 'tools', 'test', 'docs', '.github'];
const FILES = ['README.md', 'LICENSE', 'platformio.ini', '.gitignore'];
const EXT = new Set(['.html', '.js', '.css', '.cpp', '.h', '.py', '.md', '.ini', '.yml', '']);
let problems = 0, checked = 0;

function walk(dir, out) {
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    const p = path.join(dir, e.name);
    if (e.isDirectory()) { if (e.name !== 'node_modules' && e.name !== '.pio' && e.name !== '__pycache__') walk(p, out); }
    else if (EXT.has(path.extname(e.name)) && e.name !== 'golden.txt') out.push(p);
  }
}
const list = FILES.map(f => path.join(ROOT, f)).filter(fs.existsSync);
for (const d of DIRS) if (fs.existsSync(path.join(ROOT, d))) walk(path.join(ROOT, d), list);

// No real addresses in the repository. A private-range IPv4 address or a MAC address in a source, test, doc or workflow file has to be one of the
// example values below (help text, simulators, mock device); anything else is probably a real device on someone's network and fails the check.
// (Screenshots cannot be checked this way: take them from the mock device, see tools/make_screenshots.js.)
const EXAMPLE_IPS = new Set(['192.168.1.10', '192.168.1.60', '192.168.1.77', '192.168.5.5']);
const PRIVATE_IP = /\b(?:10\.\d{1,3}\.\d{1,3}\.\d{1,3}|172\.(?:1[6-9]|2\d|3[01])\.\d{1,3}\.\d{1,3}|192\.168\.\d{1,3}\.\d{1,3})\b/g;
const MAC = /\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b/g;
const isExampleMac = m => /^(AA:BB:CC|00:00:00)/i.test(m);

for (const f of list) {
  const b = fs.readFileSync(f);
  checked++;
  const rel = path.relative(ROOT, f);
  b.toString('latin1').split('\n').forEach((text, n) => {
    for (const ip of text.match(PRIVATE_IP) || []) if (!EXAMPLE_IPS.has(ip)) { console.log('address that is not an example (' + ip + ') in ' + rel + ' line ' + (n + 1) + ': use 192.0.2.x or one of the examples in test/test_lint.js'); problems++; }
    for (const mac of text.match(MAC) || []) if (!isExampleMac(mac)) { console.log('MAC address that is not an example in ' + rel + ' line ' + (n + 1) + ': use AA:BB:CC:DD:EE:FF'); problems++; }
  });
  if (b[0] === 0xEF && b[1] === 0xBB && b[2] === 0xBF) { console.log('BOM: ' + rel); problems++; }
  if (b.includes(13)) { console.log('CRLF line endings: ' + rel); problems++; }
  for (let i = 0; i < b.length; i++) {
    if (b[i] > 127) {
      const line = b.slice(0, i).toString('latin1').split('\n').length;
      console.log('non-ASCII byte in ' + rel + ' line ' + line);
      problems++;
      break;
    }
  }
}
console.log(checked + ' files checked, ' + problems + ' problems');
console.log(problems ? 'SOME FAILED' : 'ALL PASSED');
process.exit(problems ? 1 : 0);
