// SPDX-License-Identifier: GPL-3.0-or-later
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2] || 'engine/data/native-integration/popup-owner.js', 'utf8');
function fixture({privateContext = false, nested = false, web = false} = {}) {
    const calls = [], nativeResult = [{id: 37, windowId: 12}]; // No URL grant.
    const owner = {id: 37, windowId: 12, incognito: false};
    const runtime = {};
    let missing = false, denied = false;
    const nativeQuery = (info, callback) => {
        calls.push(JSON.parse(JSON.stringify(info)));
        if (denied) return Promise.reject(new Error('Native permission denied'));
        if (callback) { callback(nativeResult); return; }
        return Promise.resolve(nativeResult);
    };
    const tabs = {query: nativeQuery, get: (id, callback) => {
        assert.equal(id, 37);
        if (callback) {
            if (missing) runtime.lastError = {message: 'No tab with id: 37'};
            callback(missing ? undefined : owner);
            delete runtime.lastError;
            return;
        }
        return missing ? Promise.reject(new Error('No tab with id: 37')) : Promise.resolve(owner);
    }};
    const window = {};
    const context = vm.createContext({window, top: nested ? {} : window,
        location: {protocol: web ? 'https:' : 'chrome-extension:'},
        chrome: {tabs, runtime, extension: {inIncognitoContext: privateContext}}});
    vm.runInContext(`(${source})(37)`, context);
    return {tabs, runtime, calls, owner, nativeResult, nativeQuery,
        missing: () => { missing = true; }, deny: () => { denied = true; }};
}
(async () => {
    const f = fixture();
    const query = {active: true, currentWindow: true, url: 'https://allowed.example/*', status: 'complete'};
    assert.equal(await f.tabs.query(query), f.nativeResult);
    assert.deepEqual(f.calls.pop(), {active: true, windowId: 12, url: query.url, status: 'complete'});
    assert.equal(query.currentWindow, true);
    assert.equal(Object.hasOwn(f.nativeResult[0], 'url'), false, 'Do not fabricate protected native fields');
    let callbackResult;
    f.tabs.query({active: true, lastFocusedWindow: true}, value => { callbackResult = value; });
    assert.equal(callbackResult, f.nativeResult);
    assert.deepEqual(f.calls.pop(), {active: true, windowId: 12});
    await f.tabs.query({active: true, windowId: 42});
    assert.deepEqual(f.calls.pop(), {active: true, windowId: 42});
    await f.tabs.query({currentWindow: false});
    assert.deepEqual(f.calls.pop(), {currentWindow: false});
    await f.tabs.query({active: true});
    assert.deepEqual(f.calls.pop(), {active: true});
    f.deny();
    await assert.rejects(f.tabs.query({currentWindow: true}), /Native permission denied/);
    const closed = fixture(); closed.missing();
    await assert.rejects(closed.tabs.query({currentWindow: true}), /No tab/);
    let callbackError;
    closed.tabs.query({currentWindow: true}, result => {
        assert.equal(result.length, 0);
        callbackError = closed.runtime.lastError?.message;
    });
    assert.equal(callbackError, 'No tab with id: 37');
    assert.equal(closed.calls.length, 0, 'Never fall back to another window when the owner closes');
    const privateOwner = fixture(); privateOwner.owner.incognito = true;
    await assert.rejects(privateOwner.tabs.query({currentWindow: true}), /private tabs/);
    privateOwner.tabs.query({currentWindow: true}, result => assert.equal(result.length, 0));
    assert.equal(privateOwner.calls.length, 0);
    for (const options of [{privateContext: true}, {nested: true}, {web: true}]) {
        const excluded = fixture(options);
        assert.equal(excluded.tabs.query, excluded.nativeQuery);
    }
    console.log('POPUP_OWNER_PASS native filters/results/errors, callback/promise, closed owner and private/web/frame isolation');
})().catch(error => { console.error(error); process.exitCode = 1; });
