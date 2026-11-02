// Source hygiene: the web UI and firmware sources must be plain ASCII with LF line endings and no BOM.
// (A stray non-ASCII character in an embedded page shows up as garbled text on the device.)
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

for (const f of list) {
  const b = fs.readFileSync(f);
  checked++;
  const rel = path.relative(ROOT, f);
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
