// TV Tools, isolated world: hands the settings to page.js (main world), lite mode, pausing background videos.
"use strict";
(function () {
  var KEY = "__tvtools_cfg";
  var host = location.hostname;
  var current = null;

  function push(cfg) {
    current = cfg;
    var json = JSON.stringify(cfg);
    // page.js reads this cache synchronously at document_start on the next visit to this site.
    try { localStorage.setItem(KEY, json); } catch (e) { /* sandboxed frames, storage off */ }
    document.dispatchEvent(new CustomEvent("tvtools-config", { detail: json }));
    applyLite(cfg.lite);
  }

  function refresh() {
    tvLoad().then(function (r) { push(tvEffective(r.settings, r.hardware, host, r.hwH264)); }).catch(function () {});
  }

  refresh();
  chrome.storage.onChanged.addListener(function (changes, area) {
    if (area === "local") refresh();
  });
  // page.js asks again if it started after our first event.
  document.addEventListener("tvtools-hello", function () { if (current) push(current); });

  // Cloudflare's challenge page in the tab: bg.js switches the site to the Desktop version (Chromium's own user
  // agent) and reloads it. Cloudflare's frame always sees Chromium's user agent, and the check failed when the site's
  // requests sent the TV one.
  try {
    var nav = window.top === window && performance.getEntriesByType("navigation")[0];
    if (nav && (nav.serverTiming || []).some(function (t) { return t.name === "chlray"; })) {
      chrome.runtime.sendMessage({ cfChallenge: true }).catch(function () {});
    }
  } catch (e) { /* no navigation timing */ }

  // Pause playing videos when the tab goes to the background (option "Pause videos in background tabs").
  document.addEventListener("visibilitychange", function () {
    if (!current || !current.pauseHidden || document.visibilityState !== "hidden") return;
    var media = document.querySelectorAll("video, audio");
    for (var i = 0; i < media.length; i++) {
      if (!media[i].paused) media[i].pause();
    }
  });

  // LITE MODE: no CSS animations or transitions, images and frames load only when they come near the screen, and
  // muted videos that start by themselves (hover previews, background clips) are paused. Videos you start play.
  var liteStyle = null, liteObserver = null;
  var LITE_CSS = "*,*::before,*::after{animation:none!important;transition:none!important;" +
    "scroll-behavior:auto!important}";

  function lazify(node) {
    if (node.nodeType !== 1) return;
    var list = node.matches && node.matches("img,iframe") ? [node] : node.querySelectorAll ? node.querySelectorAll("img,iframe") : [];
    for (var i = 0; i < list.length; i++) {
      if (!list[i].hasAttribute("loading")) list[i].setAttribute("loading", "lazy");
      if (list[i].tagName === "IMG" && !list[i].hasAttribute("decoding")) list[i].setAttribute("decoding", "async");
    }
  }

  function onPlay(ev) {
    var v = ev.target;
    if (!current || !current.lite || !v || v.tagName !== "VIDEO") return;
    var ua = navigator.userActivation;
    if (v.muted && !(ua && ua.isActive)) v.pause();
  }

  function applyLite(on) {
    if (on && !liteStyle) {
      liteStyle = document.createElement("style");
      liteStyle.textContent = LITE_CSS;
      (document.head || document.documentElement).appendChild(liteStyle);
      lazify(document.documentElement);
      liteObserver = new MutationObserver(function (records) {
        for (var i = 0; i < records.length; i++) {
          var added = records[i].addedNodes;
          for (var j = 0; j < added.length; j++) lazify(added[j]);
        }
      });
      liteObserver.observe(document.documentElement, { childList: true, subtree: true });
      document.addEventListener("play", onPlay, true);
    } else if (!on && liteStyle) {
      liteStyle.remove();
      liteStyle = null;
      liteObserver.disconnect();
      liteObserver = null;
      document.removeEventListener("play", onPlay, true);
    }
  }
})();
