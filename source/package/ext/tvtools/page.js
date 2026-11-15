// TV Tools, page (main) world: limits the video quality that sites pick, on every site.
// Three levers, because players choose their stream in different ways:
//  1. Codec and size checks: MediaSource.isTypeSupported, canPlayType and navigator.mediaCapabilities answer "not
//     supported" above the limit (YouTube and other players ask with width/height/framerate).
//  2. HLS master playlists (.m3u8): stream variants above the limit are removed before the player sees them.
//  3. DASH manifests (.mpd): video representations above the limit are removed the same way.
// At least one stream is always kept (the smallest), so a video never loses all its streams.
// Nothing is changed in Cloudflare's challenge pages and frames (its bot check flags changed browser functions and
// then never lets the page through), on sites where TV Tools is turned off, and navigator is left alone when the
// site gets Chromium's own user agent (Desktop version). Cloudflare's "Just a moment..." page is served on the site's
// own URL: there nothing is wrapped and the site switches to the Desktop version (bridge.js, bg.js). On other 403 and
// 503 responses only the user agent stays consistent with the request header.
(function () {
  "use strict";
  if (/(^|\.)challenges\.cloudflare\.com$/.test(location.hostname) ||
      location.pathname.indexOf("/cdn-cgi/challenge-platform/") === 0) return;
  // Cloudflare's challenge page (its response has the Server-Timing entry "chlray"): nothing is changed, not even the
  // user agent. bridge.js switches the site to the Desktop version and reloads it.
  try {
    var nav0 = performance.getEntriesByType("navigation")[0];
    if (nav0 && (nav0.serverTiming || []).some(function (t) { return t.name === "chlray"; })) return;
  } catch (e) { /* no navigation timing: carry on */ }
  var KEY = "__tvtools_cfg";
  // Defaults until the bridge sends the settings (first visit to a site): the extension's own defaults.
  var cfg = {
    enabled: true, hardware: true, maxHeight: 1080, maxFps: 60, h264Only: false,
    ua: "Mozilla/5.0 (Web0S; Linux/SmartTV) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/151.0.0.0 " +
      "Safari/537.36 WebAppManager"
  };
  try {
    var cached = localStorage.getItem(KEY);
    if (cached) cfg = Object.assign(cfg, JSON.parse(cached));
  } catch (e) { /* no storage here: defaults */ }
  // Turned off for this site (cached from the last visit): leave the page completely untouched.
  if (cfg.enabled === false) return;
  document.addEventListener("tvtools-config", function (ev) {
    try { cfg = Object.assign(cfg, JSON.parse(ev.detail)); } catch (e) {}
  });
  document.dispatchEvent(new CustomEvent("tvtools-hello"));

  // USER AGENT: the network requests get it from a declarativeNetRequest rule (bg.js); scripts read it here.
  // With a TV or custom user agent, navigator.userAgentData is hidden so sites go by the user agent string.
  (function () {
    var NP = window.Navigator && Navigator.prototype;
    if (!NP || !cfg.ua) return;   // Desktop version: Chromium's own navigator, unchanged
    var dUA = Object.getOwnPropertyDescriptor(NP, "userAgent");
    var dAV = Object.getOwnPropertyDescriptor(NP, "appVersion");
    var dUD = Object.getOwnPropertyDescriptor(NP, "userAgentData");
    if (dUA && dUA.get) {
      Object.defineProperty(NP, "userAgent", { configurable: true, enumerable: dUA.enumerable,
        get: function () { return cfg.ua || dUA.get.call(this); } });
    }
    if (dAV && dAV.get) {
      Object.defineProperty(NP, "appVersion", { configurable: true, enumerable: dAV.enumerable,
        get: function () { return cfg.ua ? cfg.ua.replace(/^Mozilla\//, "") : dAV.get.call(this); } });
    }
    if (dUD && dUD.get) {
      Object.defineProperty(NP, "userAgentData", { configurable: true, enumerable: dUD.enumerable,
        get: function () { return cfg.ua ? undefined : dUD.get.call(this); } });
    }
  })();

  // A challenge or error page (Cloudflare's interstitial is a 403/503 on the site's own URL): no other changes.
  try {
    var nav = performance.getEntriesByType("navigation")[0];
    if (nav && (nav.responseStatus === 403 || nav.responseStatus === 503)) return;
  } catch (e) { /* no navigation timing: carry on */ }

  var NON_H264 = /av01|av1|vp0?9|vp8|hev1|hvc1|dvh1|dvhe/i;

  // A WxH stream is over the limit when its short side is above the height limit, or its long side is above the
  // matching 16:9 width (so 1920x800 counts as 1080p and portrait 1080x1920 as 1080p too). 5% slack for 1088 etc.
  function overSize(w, h) {
    if (!w && !h) return false;
    var lim = cfg.maxHeight;
    var shortSide = Math.min(w || h, h || w), longSide = Math.max(w || 0, h || 0);
    return shortSide > lim * 1.05 || longSide > (lim * 16 / 9) * 1.05;
  }
  function overFps(f) { return !!f && f > cfg.maxFps + 0.5; }
  function badCodec(c) { return cfg.h264Only && !!c && NON_H264.test(c); }

  function typeOver(t) {
    if (!cfg.enabled || typeof t !== "string") return false;
    var w = /width=(\d+)/.exec(t), h = /height=(\d+)/.exec(t), f = /framerate=([\d.]+)/.exec(t);
    return badCodec(t) || overSize(w ? +w[1] : 0, h ? +h[1] : 0) || overFps(f ? +f[1] : 0);
  }

  // 1. Capability checks
  function wrapCheck(obj, name, no) {
    var orig = obj && obj[name];
    if (typeof orig !== "function") return;
    obj[name] = function (t) { return typeOver(t) ? no : orig.apply(this, arguments); };
  }
  wrapCheck(window.MediaSource, "isTypeSupported", false);
  wrapCheck(window.ManagedMediaSource, "isTypeSupported", false);
  wrapCheck(window.HTMLMediaElement && HTMLMediaElement.prototype, "canPlayType", "");

  var mc = navigator.mediaCapabilities;
  if (mc && typeof mc.decodingInfo === "function") {
    var origInfo = mc.decodingInfo.bind(mc);
    mc.decodingInfo = function (c) {
      var v = c && c.video;
      if (cfg.enabled && v && (badCodec(v.contentType) || overSize(v.width, v.height) || overFps(v.framerate))) {
        return Promise.resolve({ supported: false, smooth: false, powerEfficient: false, configuration: c });
      }
      var p = origInfo(c);
      // The TV's hardware decoder plays what it supports smoothly; Chromium can't tell, so say so.
      if (v && cfg.hardware) {
        p = p.then(function (r) {
          return r.supported ? Object.assign({}, r, { smooth: true, powerEfficient: true }) : r;
        });
      }
      return p;
    };
  }

  // 2./3. Manifest filtering
  function keepSet(items, isOver, sizeOf) {
    var keep = items.filter(function (it) { return !isOver(it); });
    if (keep.length) return keep;
    if (!items.length) return items;
    // Everything is over the limit: keep the smallest stream rather than none.
    var smallest = items.slice().sort(function (a, b) { return sizeOf(a) - sizeOf(b); })[0];
    return [smallest];
  }

  function attr(line, name) {
    var m = new RegExp("[,:]" + name + "=(\"[^\"]*\"|[^,]*)").exec(line);
    return m ? m[1].replace(/^"|"$/g, "") : "";
  }

  function filterHls(text) {
    var lines = text.split(/\r?\n/);
    var variants = [];
    for (var i = 0; i < lines.length; i++) {
      var l = lines[i];
      if (l.indexOf("#EXT-X-STREAM-INF:") === 0 || l.indexOf("#EXT-X-I-FRAME-STREAM-INF:") === 0) {
        var res = /^(\d+)x(\d+)$/.exec(attr(l, "RESOLUTION"));
        var v = {
          tag: i, uri: -1, iframe: l.indexOf("#EXT-X-I-FRAME") === 0,
          w: res ? +res[1] : 0, h: res ? +res[2] : 0,
          fps: parseFloat(attr(l, "FRAME-RATE")) || 0, codecs: attr(l, "CODECS")
        };
        if (!v.iframe) {
          for (var j = i + 1; j < lines.length; j++) {
            if (lines[j] && lines[j].charAt(0) !== "#") { v.uri = j; break; }
          }
        }
        variants.push(v);
      }
    }
    if (!variants.length) return text;
    var isOver = function (v) { return overSize(v.w, v.h) || overFps(v.fps) || badCodec(v.codecs); };
    var sizeOf = function (v) { return v.w * v.h; };
    var drop = {};
    [false, true].forEach(function (iframe) {
      var group = variants.filter(function (v) { return v.iframe === iframe; });
      var keep = keepSet(group, isOver, sizeOf);
      group.forEach(function (v) {
        if (keep.indexOf(v) < 0) { drop[v.tag] = 1; if (v.uri >= 0) drop[v.uri] = 1; }
      });
    });
    if (!Object.keys(drop).length) return text;
    return lines.filter(function (_, n) { return !drop[n]; }).join("\n");
  }

  function num(el, name) {
    var v = el.getAttribute(name);
    if (!v) return 0;
    var p = v.split("/");
    return p.length === 2 ? (+p[0] / (+p[1] || 1)) : +v;
  }

  function filterDash(text) {
    var doc;
    try { doc = new DOMParser().parseFromString(text, "application/xml"); } catch (e) { return text; }
    if (!doc || doc.getElementsByTagName("parsererror").length) return text;
    var changed = false;
    var sets = doc.getElementsByTagNameNS("*", "AdaptationSet");
    for (var i = 0; i < sets.length; i++) {
      var set = sets[i];
      var reps = Array.prototype.slice.call(set.getElementsByTagNameNS("*", "Representation"));
      var isVideo = /video/i.test(set.getAttribute("contentType") || set.getAttribute("mimeType") || "") ||
        reps.some(function (r) { return /video/i.test(r.getAttribute("mimeType") || "") || r.hasAttribute("height"); });
      if (!isVideo || !reps.length) continue;
      var items = reps.map(function (r) {
        return {
          el: r,
          w: num(r, "width") || num(set, "width"), h: num(r, "height") || num(set, "height"),
          fps: num(r, "frameRate") || num(set, "frameRate"),
          codecs: r.getAttribute("codecs") || set.getAttribute("codecs") || ""
        };
      });
      var keep = keepSet(items,
        function (it) { return overSize(it.w, it.h) || overFps(it.fps) || badCodec(it.codecs); },
        function (it) { return it.w * it.h; });
      items.forEach(function (it) {
        if (keep.indexOf(it) < 0) { it.el.parentNode.removeChild(it.el); changed = true; }
      });
    }
    return changed ? new XMLSerializer().serializeToString(doc) : text;
  }

  function filterManifest(text) {
    if (!cfg.enabled || typeof text !== "string") return text;
    try {
      if (/^\s*#EXTM3U/.test(text) && text.indexOf("#EXT-X-STREAM-INF") >= 0) return filterHls(text);
      if (text.indexOf("<MPD") >= 0) return filterDash(text);
    } catch (e) { /* never break a page: unfiltered */ }
    return text;
  }

  function maybeManifest(url, type) {
    // Only real manifests: buffering other responses here would delay streaming bodies.
    return /\.(m3u8|mpd)(\?|#|$)/i.test(url || "") || /mpegurl|dash\+xml/i.test(type || "");
  }

  // fetch()
  var origFetch = window.fetch;
  if (typeof origFetch === "function") {
    window.fetch = function () {
      var p = origFetch.apply(this, arguments);
      if (!cfg.enabled) return p;
      return p.then(function (r) {
        try {
          if (!r || r.status !== 200 || !maybeManifest(r.url, r.headers.get("content-type"))) return r;
        } catch (e) { return r; }
        return r.clone().text().then(function (t) {
          var f = filterManifest(t);
          if (f === t) return r;
          var nr = new Response(f, { status: r.status, statusText: r.statusText, headers: r.headers });
          // Players resolve relative stream URLs against response.url; a new Response has none.
          Object.defineProperties(nr, {
            url: { value: r.url }, redirected: { value: r.redirected }, type: { value: r.type }
          });
          return nr;
        }, function () { return r; });
      });
    };
  }

  // XMLHttpRequest (hls.js, dash.js and Shaka use it by default)
  var XO = window.XMLHttpRequest && XMLHttpRequest.prototype;
  if (XO) {
    var dText = Object.getOwnPropertyDescriptor(XO, "responseText");
    var dResp = Object.getOwnPropertyDescriptor(XO, "response");
    var origOpen = XO.open;
    XO.open = function (method, url) {
      this.__tvUrl = String(url);
      this.__tvDone = false;
      this.__tvText = null;
      this.__tvBuf = null;
      return origOpen.apply(this, arguments);
    };
    var filtered = function (x) {
      if (x.__tvDone) return;
      if (x.readyState !== 4) return;
      x.__tvDone = true;
      if (!cfg.enabled || x.status !== 200) return;
      try {
        if (!maybeManifest(x.responseURL || x.__tvUrl, x.getResponseHeader("content-type"))) return;
        var rt = x.responseType;
        if (rt === "" || rt === "text") {
          var t = dText.get.call(x), f = filterManifest(t);
          if (f !== t) x.__tvText = f;
        } else if (rt === "arraybuffer") {
          var buf = dResp.get.call(x);
          var s = new TextDecoder().decode(buf), g = filterManifest(s);
          if (g !== s) x.__tvBuf = new TextEncoder().encode(g).buffer;
        }
      } catch (e) { /* unfiltered */ }
    };
    if (dText && dText.get) {
      Object.defineProperty(XO, "responseText", {
        configurable: true, enumerable: dText.enumerable,
        get: function () { filtered(this); return this.__tvText !== null && this.__tvText !== undefined ? this.__tvText : dText.get.call(this); }
      });
    }
    if (dResp && dResp.get) {
      Object.defineProperty(XO, "response", {
        configurable: true, enumerable: dResp.enumerable,
        get: function () {
          filtered(this);
          if (this.__tvText !== null && this.__tvText !== undefined) return this.__tvText;
          if (this.__tvBuf) return this.__tvBuf;
          return dResp.get.call(this);
        }
      });
    }
  }
  // MEDIA KEYS (r/webos, LG C3, 2026-11-05): the remote's Play (evdev 207) and Pause (119), and buttons mapped to them
  // with Input Hook, reach the page as keydown "MediaPlay" / "Pause"; desktop Chrome sends media keys to the player
  // over D-Bus (MPRIS), which the TV doesn't have, so nothing happened. Here they control the page's main video:
  // the playing one, else the biggest visible one. A site that handles the key itself (calls preventDefault, like
  // YouTube's TV layout with keyCode 19/415) is left alone. Same-origin iframes are searched too.
  function mediaIn(doc, out) {
    try {
      doc.querySelectorAll("video, audio").forEach(function (m) { out.push(m); });
      doc.querySelectorAll("iframe").forEach(function (f) { try { if (f.contentDocument) mediaIn(f.contentDocument, out); } catch (e) {} });
    } catch (e) {}
    return out;
  }
  function mainMedia() {
    var all = mediaIn(document, []);
    var playing = all.filter(function (m) { return !m.paused && !m.ended; });
    if (playing.length) return playing[0];
    var best = null, area = 0;
    all.forEach(function (m) {
      var r = m.getBoundingClientRect(), a = r.width * r.height;
      if (a > area) { area = a; best = m; }
    });
    return best;
  }
  var MEDIA_KEYS = { MediaPlay: "play", MediaPause: "pause", Pause: "pause", MediaPlayPause: "toggle", MediaStop: "pause" };
  if (window === window.top) {
    window.addEventListener("keydown", function (e) {
      var act = MEDIA_KEYS[e.key] || (e.keyCode === 415 ? "play" : e.keyCode === 19 ? "pause" : null);
      if (!act) return;
      setTimeout(function () {
        if (e.defaultPrevented) return;
        var m = mainMedia();
        if (!m) return;
        if (act === "toggle") act = m.paused ? "play" : "pause";
        if (act === "play") { var p = m.play(); if (p && p.catch) p.catch(function () {}); } else m.pause();
      }, 0);
    }, false);
  }
})();
