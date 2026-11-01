// SPDX-License-Identifier: GPL-3.0-or-later
// Keep native API permission filtering; only the popup's window context changes.
ownerTabId => {
    if (window !== top || location.protocol !== 'chrome-extension:' ||
        !globalThis.chrome?.tabs || chrome.extension?.inIncognitoContext) return;
    const tabs = chrome.tabs, query = tabs.query.bind(tabs), get = tabs.get.bind(tabs);
    const forOwner = (info, owner) => {
        const target = {...info, windowId: owner.windowId};
        delete target.currentWindow;
        delete target.lastFocusedWindow;
        return target;
    };
    tabs.query = (info, callback) => {
        if (!info || info.windowId !== undefined ||
            (info.currentWindow !== true && info.lastFocusedWindow !== true))
            return query(info, callback);
        if (typeof callback === 'function') {
            get(ownerTabId, owner => {
                if (chrome.runtime.lastError || !owner || owner.incognito) {
                    callback([]);
                    return;
                }
                query(forOwner(info, owner), callback);
            });
            return;
        }
        return get(ownerTabId).then(owner => {
            if (owner.incognito) throw new Error('Extension popups cannot access private tabs');
            return query(forOwner(info, owner));
        });
    };
}
