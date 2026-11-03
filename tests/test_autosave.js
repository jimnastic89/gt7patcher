// Regression: patch toggles auto-save the config; there is no Save button.
// Executes the production UI script with a small DOM/fetch fixture. No console access.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../web/index.html'), 'utf8');
assert(!html.includes('id="save"'), 'the Save button must be removed');

const elements = new Map();
for (const match of html.matchAll(/<[a-z]+\b([^>]*\bid="([^"]+)"[^>]*)>/g)) {
    elements.set(match[2], {
        value: '', checked: false, disabled: false, hidden: /\bhidden\b/.test(match[1]),
        textContent: '', attributes: {}, listeners: {},
        classList: { add() {}, remove() {}, toggle() {} },
        append() {}, querySelectorAll() { return [] },
        addEventListener(event, handler) { this.listeners[event] = handler },
        setAttribute(key, value) { this.attributes[key] = value }
    });
}
elements.set('patch-6', { checked: false });
elements.set('patch-1', { checked: false, dataset: { bit: '1' } });
const state = {
    token: 'test-token', engineRunning: true, gameRunning: 1, version: '0.3.1',
    config: { mask: 0, preset: 0, units: 0, language: 2, country: -1, speed: 0, mass: 0, power: 2, torque: 2, temperature: 1, autostart: 0 },
    features: [], message: 'Ready', live: { available: false }
};
const requests = [];
const context = vm.createContext({
    document: { activeElement: null, getElementById: id => elements.get(id) || null,
        createElement: () => ({ dataset: {}, classList: { add() {}, remove() {} }, append() {}, setAttribute() {} }),
        querySelectorAll: selector => selector === '#features input[data-bit]' ? [{ checked: true, dataset: { bit: '1' } }] : [] },
    Option: function () {}, URLSearchParams, confirm: () => true,
    clearTimeout, setTimeout, setInterval() {},
    async fetch(url, options) { requests.push({ url, options }); return { ok: true, json: async () => state } }
});
vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], context);
const evaluate = code => vm.runInContext(code, context);
const settle = () => new Promise(resolve => setImmediate(resolve));

(async () => {
    await settle();
    assert.equal(requests.filter(r => r.url === '/api/config').length, 0, 'no config post during boot');

    // The change handler calls mark(), which schedules a debounced auto-save.
    elements.get('patch-1').checked = true;
    evaluate('mark()');
    await new Promise(resolve => setTimeout(resolve, 400));

    const posts = requests.filter(r => r.url === '/api/config');
    assert.equal(posts.length, 1, 'one auto-save POST after a patch toggle');
    assert.match(posts[0].options.body, /mask=2/);
    assert.equal(posts[0].options.headers['X-GT7-Token'], 'test-token');
    assert.equal(evaluate('dirty'), false, 'dirty cleared after auto-save');
    console.log('PASS: patch toggles auto-save the configuration and the Save button is gone');
})().catch(error => { console.error(error); process.exitCode = 1 });
