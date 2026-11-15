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
  lite: false,          // lite mode: no animations, lazy images, no muted autoplay
  discardMinutes: 0,    // unload background tabs after this many minutes; 0 = never
  tabLimit: 0,          // close the least recently used tab above this many tabs; 0 = no limit
  pauseHidden: false    // pause videos when their tab goes to the background
};

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
function tvEffective(s, hardware, host, hwH264) {
  var hw = hardware !== false, h264hw = hw && hwH264 === true;
  return {
    enabled: !tvHostIn(host, s.disabledSites),
    hardware: hw,
    maxHeight: s.maxRes === "auto" ? (hw ? 1080 : 720) : Number(s.maxRes),
    maxFps: s.maxFps === "auto" ? (hw && !h264hw ? 60 : 30) : Number(s.maxFps),
    h264Only: s.codec === "h264" || (s.codec === "auto" && (!hw || h264hw)),
    ua: tvHostIn(host, s.desktopSites) || (s.googleDesktop !== false && tvIsGoogle(host)) ? null : tvUserAgent(s),
    lite: !!s.lite,
    pauseHidden: !!s.pauseHidden
  };
}

function tvLoad() {
  return chrome.storage.local.get(["settings", "hardware", "hwH264"]).then(function (r) {
    return { settings: Object.assign({}, TV_DEFAULTS, r.settings || {}), hardware: r.hardware !== false,
             hwH264: r.hwH264 === true };
  });
}

function tvSave(settings) {
  return chrome.storage.local.set({ settings: settings });
}
