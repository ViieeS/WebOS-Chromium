// TV Tools background worker: hardware-video flag from the launcher, the user agent rule, tab unloading and the
// tab limit.
"use strict";
importScripts("settings.js");

// run.sh writes hw.json next to this file: {"hardware": true} when the TV's hardware decoder is in use, plus
// "h264": true when it decodes only H.264 (webOS 4, NDL API 1).
fetch(chrome.runtime.getURL("hw.json"))
  .then(function (r) { return r.json(); })
  .then(function (j) { return chrome.storage.local.set({ hardware: j.hardware !== false, hwH264: j.h264 === true }); })
  .catch(function () { return chrome.storage.local.set({ hardware: true, hwH264: false }); });

// USER AGENT for network requests (page.js covers navigator.userAgent). One dynamic rule: the TV or custom user
// agent on every request, except to and from the sites set to "desktop version". Client hints that would still say
// "Chromium on Linux" are removed so sites see one consistent browser.
var UA_RULE = 1, GOOGLE_RULE = 2;
var ALL_TYPES = ["main_frame", "sub_frame", "stylesheet", "script", "image", "font", "object", "xmlhttprequest",
  "ping", "csp_report", "media", "websocket", "webtransport", "webbundle", "other"];

function applyUserAgent(s) {
  var ua = tvUserAgent(s);
  var rules = [];
  if (ua) {
    // Cloudflare's challenge frames keep Chromium's own user agent, the one they see in navigator (page.js does not
    // run there): a header that differs from navigator fails the challenge.
    var condition = { resourceTypes: ALL_TYPES, excludedRequestDomains: ["challenges.cloudflare.com"] };
    if (s.desktopSites && s.desktopSites.length) {
      condition.excludedRequestDomains = condition.excludedRequestDomains.concat(s.desktopSites);
      condition.excludedInitiatorDomains = s.desktopSites.slice();
    }
    rules.push({
      id: UA_RULE, priority: 1, condition: condition,
      action: {
        type: "modifyHeaders",
        requestHeaders: [
          { header: "user-agent", operation: "set", value: ua },
          { header: "sec-ch-ua", operation: "remove" },
          { header: "sec-ch-ua-mobile", operation: "remove" },
          { header: "sec-ch-ua-platform", operation: "remove" },
          { header: "sec-ch-ua-full-version-list", operation: "remove" },
          { header: "sec-ch-ua-platform-version", operation: "remove" }
        ]
      }
    });
  }
  // Google search (2026-11-05): with the TV user agent Google drops udm=14, so its "Web" filter (and
  // tenbluelinks.org) didn't work. A higher-priority allow rule keeps the UA rule off Google's own hosts.
  if (ua && s.googleDesktop !== false) {
    rules.push({
      id: GOOGLE_RULE, priority: 2, action: { type: "allow" },
      condition: { regexFilter: "^https?://([^./]+\\.)*google\\.[a-z]{2,3}(\\.[a-z]{2})?(:[0-9]+)?/", resourceTypes: ALL_TYPES }
    });
  }
  return chrome.declarativeNetRequest.updateDynamicRules({ removeRuleIds: [UA_RULE, GOOGLE_RULE], addRules: rules })
    .catch(function (e) { console.warn("TV Tools: user agent rule", e); });
}

// TAB LIMIT: above the limit, close the tabs you used least recently. Never the visible tab, the new tab, a tab
// playing sound or a pinned tab.
function enforceTabLimit(s, newTabId) {
  var limit = Number(s.tabLimit) || 0;
  if (limit <= 0) return;
  chrome.tabs.query({}).then(function (tabs) {
    var extra = tabs.length - limit;
    if (extra <= 0) return;
    var victims = tabs.filter(function (t) {
      return !t.active && !t.pinned && !t.audible && t.id !== newTabId;
    }).sort(function (a, b) { return (a.lastAccessed || 0) - (b.lastAccessed || 0); }).slice(0, extra);
    if (victims.length) chrome.tabs.remove(victims.map(function (t) { return t.id; })).catch(function () {});
  });
}

// CLOUDFLARE CHALLENGE (bridge.js): a site that got the TV or custom user agent switches to the Desktop version and
// the tab reloads, once. Cloudflare's clearance only works with the user agent the challenge ran with.
chrome.runtime.onMessage.addListener(function (msg, sender) {
  if (!msg || msg.cfChallenge !== true || !sender.tab || sender.frameId !== 0 || !sender.url) return;
  var host;
  try { host = new URL(sender.url).hostname; } catch (e) { return; }
  tvLoad().then(function (r) {
    var s = r.settings;
    if (!host || !tvEffective(s, r.hardware, host, r.hwH264).ua) return;   // already Chromium's own user agent
    s.desktopSites = (s.desktopSites || []).concat([host]);
    return applyUserAgent(s)
      .then(function () { return tvSave(s); })
      .then(function () { return chrome.tabs.reload(sender.tab.id); });
  }).catch(function (e) { console.warn("TV Tools: Cloudflare challenge", e); });
});

tvLoad().then(function (r) { applyUserAgent(r.settings); });
chrome.storage.onChanged.addListener(function (c, area) {
  if (area !== "local" || !c.settings) return;
  var s = Object.assign({}, TV_DEFAULTS, c.settings.newValue || {});
  applyUserAgent(s);
  enforceTabLimit(s, -1);
});
chrome.tabs.onCreated.addListener(function (tab) {
  tvLoad().then(function (r) { enforceTabLimit(r.settings, tab.id); });
});

chrome.alarms.create("tvtools-discard", { periodInMinutes: 1 });
chrome.alarms.onAlarm.addListener(function (alarm) {
  if (alarm.name !== "tvtools-discard") return;
  tvLoad().then(function (r) {
    var minutes = Number(r.settings.discardMinutes) || 0;
    if (minutes <= 0) return;
    var cutoff = Date.now() - minutes * 60000;
    return chrome.tabs.query({}).then(function (tabs) {
      tabs.forEach(function (t) {
        // Never the visible tab, a tab playing sound, a pinned tab, or one already unloaded.
        if (t.active || t.audible || t.pinned || t.discarded || !t.lastAccessed) return;
        if (t.lastAccessed < cutoff) chrome.tabs.discard(t.id).catch(function () {});
      });
    });
  });
});
