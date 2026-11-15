"use strict";
(function () {
  var state;
  var host = "";

  // Adds or removes `host` in the settings list `key`, saves, and refocuses `btn`.
  function toggle(key, add, btn) {
    var list = (state.settings[key] || []).filter(function (h) { return h !== host; });
    if (add) list.push(host);
    state.settings[key] = list;
    return tvSave(state.settings).then(function () { renderSite(); document.getElementById(btn).focus(); });
  }

  function renderSite() {
    var card = document.getElementById("sitecard");
    if (!host) { card.hidden = true; return; }
    card.hidden = false;
    document.getElementById("host").textContent = host;
    var off = state.settings.disabledSites.indexOf(host) >= 0;
    document.getElementById("sitesub").textContent = off ? "Video limits are off here" : "Video limits apply here";
    var b = document.getElementById("sitebtn");
    b.textContent = off ? "Turn on" : "Turn off";
    b.onclick = function () { toggle("disabledSites", !off, "sitebtn"); };

    var row = document.getElementById("uarow");
    var ua = tvUserAgent(state.settings);
    row.hidden = !ua;  // the global setting is already "Desktop"
    if (ua) {
      var google = tvIsGoogle(host) && state.settings.googleDesktop !== false;
      var desk = google || state.settings.desktopSites.indexOf(host) >= 0;
      document.getElementById("uasub").textContent = desk ? "Desktop version of this site"
        : (state.settings.uaMode === "tv" ? "TV version of this site" : "Custom user agent on this site");
      var u = document.getElementById("uabtn");
      u.textContent = desk ? (state.settings.uaMode === "tv" ? "Use TV" : "Use custom") : "Use desktop";
      u.onclick = google ? function () {  // Google's built-in desktop default: switch it off for all Google sites
        state.settings.googleDesktop = false;
        tvSave(state.settings).then(function () { renderSite(); document.getElementById("uabtn").focus(); });
      } : function () {
        if (!desk && tvIsGoogle(host)) state.settings.googleDesktop = true;
        toggle("desktopSites", !desk, "uabtn");
      };
    }
  }

  Promise.all([tvLoad(), chrome.tabs.query({ active: true, currentWindow: true })]).then(function (r) {
    state = r[0];
    state.settings.disabledSites = (state.settings.disabledSites || []).slice();
    state.settings.desktopSites = (state.settings.desktopSites || []).slice();
    var tab = r[1] && r[1][0];
    try {
      var url = new URL(tab && tab.url);
      if (url.protocol === "http:" || url.protocol === "https:") host = url.hostname;
    } catch (e) {}
    var st = document.getElementById("status");
    st.textContent = tvStatusText(state.hardware, state.hwH264);
    if (state.hardware) st.classList.add("on");
    renderSite();
    tvRender(document.getElementById("groups"), state, { compact: true });
    document.getElementById("more").onclick = function () { chrome.runtime.openOptionsPage(); window.close(); };
    var first = document.querySelector('button[data-group="lite"].on');
    if (first) first.focus();
  });
})();
