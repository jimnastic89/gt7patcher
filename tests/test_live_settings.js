// Execute the production UI script with a small DOM/fetch fixture. No console access.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const html = fs.readFileSync(path.join(__dirname, '../web/index.html'), 'utf8');
assert.match(html, /id="game-tab"[^>]*>Language &amp; Units<\/button>/);
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
elements.set('patch-6', { checked: true });
const state = {
    token: 'test-token', engineRunning: true, gameRunning: 1, version: '0.23',
    config: { mask: 64, preset: 0, units: 0, language: 2, speed: 0, mass: 0, power: 2, torque: 2, temperature: 1, autostart: 0 },
    features: [], message: 'Ready', live: { available: true, credits: 43700, session: '105:e415c45160',
        wheelWidth: { available: true, front: 1, rear: 2, modelCode: 431, sequence: 7 } }
};
const requests = [];
let poll;
let failApply = false;
let holdStatus = false, releaseStatus;
const context = vm.createContext({
    document: { activeElement: null, getElementById: id => { assert(elements.has(id), id); return elements.get(id) }, querySelectorAll: () => [] },
    Option: function () {}, URLSearchParams, confirm: () => true,
    clearTimeout() {}, setTimeout() { return 1 },
    setInterval(fn, delay) { assert.equal(delay, 5000); poll = fn },
    async fetch(url, options) {
        requests.push({ url, options });
        if (url === '/api/status' && holdStatus) {
            const snapshot = JSON.parse(JSON.stringify(state));
            return new Promise(resolve => { releaseStatus = () => resolve({ ok: true, json: async () => snapshot }) });
        }
        if (url === '/api/live/credits') {
            if (failApply) return { ok: false, text: async () => 'Balance changed; refresh and retry.' };
            state.live.credits = Number(new URLSearchParams(options.body).get('amount'));
        }
        if (url === '/api/live/wheel-width') {
            if (failApply) return { ok: false, text: async () => 'The selected car changed; refresh and retry.' };
            const values = new URLSearchParams(options.body);
            state.live.wheelWidth.front = Number(values.get('front'));
            state.live.wheelWidth.rear = Number(values.get('rear'));
            state.live.wheelWidth.sequence++;
        }
        return { ok: true, json: async () => JSON.parse(JSON.stringify(state)) };
    }
});
vm.runInContext(html.match(/<script>([\s\S]*?)<\/script>/)[1], context);
const element = id => elements.get(id);
const evaluate = code => vm.runInContext(code, context);
const settle = () => new Promise(resolve => setImmediate(resolve));

(async () => {
    await settle();
    assert.equal(element('credits').value, '43700');
    assert.equal(element('apply-credits').disabled, false);
    assert.equal(element('wheel-width-front').value, '1');
    assert.equal(element('wheel-width-rear').value, '2');
    assert.match(element('wheel-width-status').textContent, /Model code 431/);
    element('live-tab').onclick();
    assert.equal(element('live').hidden, false);
    assert.equal(element('controls').hidden, true);
    assert.equal(element('game').hidden, true);

    // Live editing does not dirty the next-launch config; polling preserves drafts.
    element('credits').value = '80000'; element('credits').listeners.input();
    element('form').listeners.change({ target: { id: 'credits' } });
    assert.equal(evaluate('dirty'), false);
    state.live.credits = 44000;
    poll(); await settle();
    assert.equal(element('credits').value, '80000');
    state.live.wheelWidth = { available: true, front: -1, rear: 3, modelCode: 912, sequence: 8 };
    poll(); await settle();
    assert.equal(element('wheel-width-front').value, '-1');
    assert.equal(element('wheel-width-rear').value, '3');
    assert.match(element('wheel-width-status').textContent, /Model code 912/);
    assert.match(element('credit-balance').textContent, /44,000/);
    evaluate('mark()');
    state.live.credits = 45000;
    poll(); await settle();
    assert.match(element('credit-balance').textContent, /45,000/);
    assert.equal(evaluate('dirty'), true);

    await element('apply-credits').onclick();
    const apply = requests.find(request => request.url === '/api/live/credits');
    assert.equal(apply.options.body, 'amount=80000&expected=45000&session=105:e415c45160');
    assert.equal(apply.options.headers['X-GT7-Token'], 'test-token');
    assert.equal(evaluate('dirty'), true);
    assert.match(element('credit-balance').textContent, /80,000/);
    assert.equal(element('credits').value, '80000');

    element('wheel-width-front').value = '7'; element('wheel-width-front').listeners.input();
    element('wheel-width-rear').value = '-4'; element('wheel-width-rear').listeners.input();
    assert.equal(element('apply-wheel-width').disabled, false);
    await element('apply-wheel-width').onclick();
    const wheelApply = requests.find(request => request.url === '/api/live/wheel-width');
    assert.equal(wheelApply.options.body, 'front=7&rear=-4&expectedFront=-1&expectedRear=3&modelCode=912&sequence=8');
    assert.equal(element('wheel-width-front').value, '7');
    assert.equal(element('wheel-width-rear').value, '-4');

    for (const invalid of ['', '-1', '1.5', '1e5', '100000001', '2147483648', '99999999999']) {
        element('credits').value = invalid; element('credits').listeners.input();
        assert.equal(element('apply-credits').disabled, true, invalid);
        const count = requests.length;
        await element('apply-credits').onclick();
        assert.equal(requests.length, count);
    }
    for (const invalid of ['', '-129', '128', '1.5', '1e2']) {
        element('wheel-width-front').value = invalid; element('wheel-width-front').listeners.input();
        assert.equal(element('apply-wheel-width').disabled, true, invalid);
    }
    element('credits').value = '90000'; element('credits').listeners.input();
    failApply = true;
    await element('apply-credits').onclick();
    assert.match(element('credit-feedback').textContent, /Balance changed/);
    assert.equal(element('credits').value, '90000');

    // Losing the process invalidates drafts; a new session gets a fresh balance.
    state.live = { available: false, credits: null, session: null, wheelWidth: { available: false, front: null, rear: null, modelCode: null, sequence: null } };
    poll(); await settle();
    assert.equal(element('credits').disabled, true);
    assert.equal(element('apply-credits').disabled, true);
    state.live = { available: true, credits: 123, session: '106:e400001000', wheelWidth: { available: true, front: 0, rear: 0, modelCode: 17, sequence: 9 } };
    poll(); await settle();
    assert.equal(element('credits').value, '123');
    assert.equal(element('credits').disabled, false);
    // A delayed poll started before Apply cannot replace its newer result.
    failApply = false; holdStatus = true;
    poll(); await settle();
    element('credits').value = '456'; element('credits').listeners.input();
    await element('apply-credits').onclick();
    releaseStatus(); await settle();
    assert.match(element('credit-balance').textContent, /456/);
    assert.equal(element('credits').value, '456');
    element('patch-6').checked = false;
    evaluate('mark()');
    assert.equal(element('live-tab').hidden, true);
    assert.equal(element('live').hidden, true);
    assert.equal(element('patches').hidden, false);
    assert.equal(element('apply-credits').disabled, true);
    console.log('PASS: live polling, draft preservation, config independence, apply requests, errors, validation, and session changes');
})().catch(error => { console.error(error); process.exitCode = 1 });
