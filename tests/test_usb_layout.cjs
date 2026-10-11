const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const root = path.join(__dirname, '..');
const source = fs.readFileSync(path.join(root, 'nyx/nyx_gui/frontend/gui_tools.c'), 'utf8');
const fonts = fs.readFileSync(path.join(root, 'bdk/libs/lvgl/lv_fonts/interui_20.c'), 'utf8');
const glyphs = new Map([...fonts.matchAll(/\.w_px = (\d+).*Unicode: U\+([0-9a-f]+)/gi)]
  .map(match => [parseInt(match[2], 16), Number(match[1])]));
const decode = literal => JSON.parse(literal.replace(/\\([0-7]{3})/g,
  (_, octal) => '\\u' + parseInt(octal, 8).toString(16).padStart(4, '0')));
function map(name) {
  const block = source.match(new RegExp(`static const char \\*${name}\\[\\] =\\s*\\{([\\s\\S]*?)\\};`));
  assert(block, `missing ${name}`);
  const entries = [...block[1].matchAll(/"(?:\\.|[^"\\])*"/g)].map(match => decode(match[0]));
  assert.equal(entries.pop(), '');
  const rows = [[]];
  for (const entry of entries) {
    if (entry === '\n') rows.push([]);
    else {
      const code = entry.charCodeAt(0);
      const controlled = (code & 0xc0) === 0x80;
      assert(!controlled || !(code & 0x28), 'hidden or disabled key: ' + entry);
      rows.at(-1).push({text: controlled ? entry.slice(1) : entry, weight: controlled ? code & 7 : 1});
    }
  }
  return rows;
}
function rect(name, x, y, w, h) { return {name, x, y, w, h}; }
function testRegions(combined) {
  const regions = [rect('preview', 24, 104, combined ? 858 : 1184, 54),
    rect('pages', 24, 166, combined ? 858 : 1184, 64),
    rect('keys', 24, 272, combined ? 858 : 1184, 340),
    rect('ESC repeat', 824, 16, 184, 64), rect('Close', 1024, 16, 184, 64)];
  if (combined) regions.push(rect('touchpad', 902, 104, 306, 406), rect('mouse clicks', 902, 522, 306, 72));
  for (const region of regions) {
    assert(region.x >= 0 && region.y >= 0 && region.x + region.w <= 1232 && region.y + region.h <= 672,
      'outside panel: ' + region.name);
    // Require these tested dimensions to exist in the real UI, not just the test.
    if (region.name === 'keys') assert(source.includes('key_width, 340'));
    if (region.name === 'Close') assert(source.includes('btn_exit, 1024, 16'));
    if (region.name === 'touchpad') assert(source.includes('usb_hid_kb_mousepad, 902, 104'));
  }
  for (let i = 0; i < regions.length; i++) for (let j = i + 1; j < regions.length; j++) {
    const a = regions[i], b = regions[j];
    assert(!(a.x < b.x + b.w && a.x + a.w > b.x && a.y < b.y + b.h && a.y + a.h > b.y),
      `${a.name} overlaps ${b.name}`);
  }
}
let count = 0;
for (const mode of ['lc', 'uc', 'sym', 'fn']) {
  const rows = map('usb_hid_kb_map_' + mode);
  assert.equal(rows.length, 4);
  const keys = rows.flat();
  assert(!keys.some(key => key.text === 'Close'));
  for (const required of ['Space', 'Esc', 'Tab', 'Enter', 'Bksp']) assert(keys.some(key => key.text === required));
  const space = keys.find(key => key.text === 'Space');
  assert.equal(space.weight, 6);
  for (const width of [858, 1184]) for (const row of rows) {
    const units = row.reduce((sum, key) => sum + key.weight, 0);
    const usable = width - 16 - (row.length - 1) * 8;
    for (const key of row) {
      const keyWidth = Math.floor(usable * key.weight / units);
      const textWidth = [...key.text].reduce((sum, char) => sum + (glyphs.get(char.charCodeAt(0)) ?? 20) + 1, 0);
      assert(keyWidth >= 48, `${mode}: tiny touch target ${key.text}: ${keyWidth}`);
      assert(keyWidth >= textWidth + 12, `${mode}: label doesn't fit ${key.text}`);
      count++;
    }
  }
}
const functions = map('usb_hid_kb_map_fn').flat().map(key => key.text);
for (let n = 1; n <= 12; n++) assert(functions.includes('F' + n));
for (const arrow of ['Left', 'Right', 'Up', 'Down']) assert(functions.includes(arrow));
testRegions(false);
testRegions(true);
console.log(`Layout tests passed: ${count} key bounds/labels, four pages, separate Close/touchpad regions`);

if (process.argv.includes('--keys') || process.argv.includes('--key-source')) {
  const os = require('node:os');
  const {execFileSync} = require('node:child_process');
  const start = source.indexOf('static bool _usb_hid_kb_map_char(');
  const end = source.indexOf('static lv_res_t _usb_hid_kb_exit_action(');
  assert(start >= 0 && end > start);
  const cases = [];
  function expect(char, usage, modifier = 0) {
    cases.push(`txt[0]=${char.charCodeAt(0)}; assert(_usb_hid_kb_map_char(txt,&mod,&usage)); assert(usage==${usage} && mod==${modifier});`);
  }
  for (let i = 0; i < 26; i++) {
    expect(String.fromCharCode(97 + i), 4 + i);
    expect(String.fromCharCode(65 + i), 4 + i, 2);
  }
  for (let i = 0; i < 10; i++) {
    expect('1234567890'[i], 0x1e + i);
    expect('!@#$%^&*()'[i], 0x1e + i, 2);
  }
  for (const [lower, upper, code] of [['-', '_', 0x2d], ['=', '+', 0x2e], ['[', '{', 0x2f],
    [']', '}', 0x30], ['\\', '|', 0x31], [';', ':', 0x33], ["'", '"', 0x34], ['`', '~', 0x35],
    [',', '<', 0x36], ['.', '>', 0x37], ['/', '?', 0x38]]) {
    expect(lower, code);
    expect(upper, code, 2);
  }
  const named = {Left: 0x50, Right: 0x4f, Up: 0x52, Down: 0x51, Ins: 0x49,
    Del: 0x4c, Home: 0x4a, End: 0x4d, PgUp: 0x4b, PgDn: 0x4e};
  for (let i = 1; i <= 12; i++) named['F' + i] = 0x39 + i;
  for (const [key, code] of Object.entries(named)) {
    cases.push(`assert(_usb_hid_kb_map_named_usage(${JSON.stringify(key)},&mod,&usage)); assert(usage==${code} && mod==0);`);
  }
  const generateOnly = process.argv.includes('--key-source');
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'hekate-key-map-'));
  const file = generateOnly ? process.argv[process.argv.indexOf('--key-source') + 1] : path.join(temp, 'test.c');
  const exe = path.join(temp, 'test');
  fs.writeFileSync(file, '#include <assert.h>\n#include <string.h>\n#include <usb/usbd.h>\n' +
    source.slice(start, end) + '\nint main(void) { char txt[2]={0}; u8 mod,usage;\n' + cases.join('\n') + '\nreturn 0; }\n');
  if (!generateOnly) {
    execFileSync('gcc', ['-std=gnu11', '-Wall', '-Wextra', '-I' + path.join(root, 'bdk'), file, '-o', exe], {stdio: 'inherit'});
    execFileSync(exe, [], {stdio: 'inherit'});
    console.log(`Key mapping tests passed: ${cases.length} real UI HID usage/modifier mappings`);
  } else console.log(`Generated ${cases.length} real UI HID usage/modifier mapping tests in ${file}`);
}
