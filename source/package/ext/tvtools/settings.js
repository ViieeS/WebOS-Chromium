// TV Tools: settings shared by the background worker, the content bridge, the popup and the options page.
"use strict";

// The user agent of LG's own webOS TV web runtime, with this browser's Chromium version. Sites such as YouTube
// recognise "Web0S" / "SmartTV" and serve their TV layout, which is made for the remote.
var TV_UA = "Mozilla/5.0 (Web0S; Linux/SmartTV) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/151.0.0.0 " +
  "Safari/537.36 WebAppManager";

var TV_DEFAULTS = {
  maxRes: "auto",       // "auto" | 2160 | 1080 | 720 | 480 | 360 (2160 = 4K, opt-in: may stutter at start)
  maxFps: "auto",       // "auto" | 60 | 30
  codec: "auto",        // "auto" | "h264"
  disabledSites: [],    // host names where the video limits are off
  uaMode: "tv",         // "tv" | "desktop" | "custom": which version of websites to ask for
  customUA: "",
  desktopSites: [],     // host names that always get the normal (desktop) user agent
  googleDesktop: true,  // Google search gets the desktop user agent (with the TV one it drops udm=14, the "Web" filter)
  lite: "auto",         // "auto" | "on" | "off": no animations, lazy images, no muted autoplay, plus V8 lite mode
                        // (set by run.sh at start). Auto = on when the TV had less than 200 MB free at start.
  discardMinutes: "auto", // unload background tabs after this many minutes; 0 = never. "auto" (2.5.0): after 30
                        // minutes (10 while Lite mode is on), and the oldest one when the TV runs low on memory
  tabLimit: 0,          // close the least recently used tab above this many tabs; 0 = no limit
  pauseHidden: false,   // pause videos when their tab goes to the background
  settingsVersion: 2    // 2 (2.5.0): discardMinutes may be "auto"
};

// Tab unloading for "auto": minutes (Lite mode on / off), and below how many MB available the oldest background tab
// goes. Chrome's Memory Saver does the time part too, but even its "aggressive" level waits 2 hours. 100 MB: webOS's memory manager closes the whole app at about 60-75 MB available (owner's 43UR7500), and
// Chrome's own urgent discarding (patch 0064) acts only at critical pressure, below 20 MB (run.sh).
var TV_LITE_DISCARD_MINUTES = 10, TV_DISCARD_MINUTES = 30, TV_LOW_MEMORY_MB = 100;

// true when `host` is `site` or one of its subdomains
function tvHostIn(host, list) {
  return !!host && (list || []).some(function (s) { return host === s || host.slice(-s.length - 1) === "." + s; });
}

// Google's own sites on any country domain (google.com, www.google.de, google.co.uk), not YouTube.
var TV_GOOGLE_RE = /^([^.\/]+\.)*google\.[a-z]{2,3}(\.[a-z]{2})?$/i;
function tvIsGoogle(host) { return !!host && TV_GOOGLE_RE.test(host); }

// The user agent to send, or null for Chromium's own (desktop) one.
function tvUserAgent(s) {
  if (s.uaMode === "tv") return TV_UA;
  if (s.uaMode === "custom" && s.customUA && s.customUA.trim()) return s.customUA.trim();
  return null;
}

// What "Auto" means. With the TV's hardware decoder: any codec up to 1080p60 (bigger streams don't fit the TV's
// memory). With a hardware decoder for H.264 only (webOS 4, NDL API 1): H.264 up to 1080p30; other codecs would be
// decoded in software, and 1080p VP9/AV1 in software filled an 844 MB TV until webOS closed the app (2026-11-04).
// Without it (software decoding): H.264 up to 720p30, the only thing the CPU decodes smoothly.
// Lite mode setting: "auto", "on" or "off" (before 1.1.1 it was false/true; false was the default, so it becomes Auto).
function tvLiteSetting(v) {
  if (v === true || v === "on") return "on";
  return v === "off" ? "off" : "auto";
}

function tvEffective(s, hardware, host, hwH264, lowMem) {
  var hw = hardware !== false, h264hw = hw && hwH264 === true;
  return {
    enabled: !tvHostIn(host, s.disabledSites),
    hardware: hw,
    maxHeight: s.maxRes === "auto" ? (hw ? 1080 : 720) : Number(s.maxRes),
    maxFps: s.maxFps === "auto" ? (hw && !h264hw ? 60 : 30) : Number(s.maxFps),
    h264Only: s.codec === "h264" || (s.codec === "auto" && (!hw || h264hw)),
    ua: tvHostIn(host, s.desktopSites) || (s.googleDesktop !== false && tvIsGoogle(host)) ? null : tvUserAgent(s),
    lite: tvLiteSetting(s.lite) === "on" || (tvLiteSetting(s.lite) === "auto" && lowMem === true),
    pauseHidden: !!s.pauseHidden
  };
}

// Lite mode in effect: On, or Auto on a TV that had less than 200 MB free when the browser opened.
function tvLiteOn(s, lowMem) {
  var l = tvLiteSetting(s.lite);
  return l === "on" || (l === "auto" && lowMem === true);
}

// Minutes after which background tabs are unloaded (0 = never), and whether low memory unloads one at once.
function tvDiscard(s, lowMem) {
  if (s.discardMinutes === "auto") {
    return { minutes: tvLiteOn(s, lowMem) ? TV_LITE_DISCARD_MINUTES : TV_DISCARD_MINUTES, onLowMemory: true };
  }
  return { minutes: Number(s.discardMinutes) || 0, onLowMemory: false };
}

function tvLoad() {
  return chrome.storage.local.get(["settings", "hardware", "hwH264", "lowMem", "freeMB"]).then(function (r) {
    var s = Object.assign({}, TV_DEFAULTS, r.settings || {});
    s.lite = tvLiteSetting(s.lite);
    // Settings saved before 2.5.0 hold discardMinutes 0, the old default "Never": they move to Auto once.
    if (r.settings && !(r.settings.settingsVersion >= 2) && Number(r.settings.discardMinutes) === 0) {
      s.discardMinutes = "auto";
    }
    return { settings: s, hardware: r.hardware !== false, hwH264: r.hwH264 === true, lowMem: r.lowMem === true,
             freeMB: typeof r.freeMB === "number" ? r.freeMB : null };
  });
}

function tvSave(settings) {
  return chrome.storage.local.set({ settings: settings });
}
