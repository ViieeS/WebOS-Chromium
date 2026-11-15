"use strict";
(function () {
  var state;

  function renderList(id, key, emptyText) {
    var ul = document.getElementById(id);
    ul.textContent = "";
    var list = state.settings[key] || [];
    if (!list.length) {
      ul.appendChild(tvEl("li", "empty", emptyText));
      return;
    }
    list.forEach(function (h) {
      var li = tvEl("li");
      li.appendChild(tvEl("span", "host", h));
      var b = tvEl("button", "", "Remove");
      b.onclick = function () {
        state.settings[key] = list.filter(function (x) { return x !== h; });
        tvSave(state.settings).then(renderSites);
      };
      li.appendChild(b);
      ul.appendChild(li);
    });
  }

  function renderSites() {
    renderList("dsites", "desktopSites", "None.");
    renderList("sites", "disabledSites", "None. Video limits apply on every site.");
  }

  function render() {
    var st = document.getElementById("status");
    st.textContent = tvStatusText(state.hardware, state.hwH264);
    st.classList.toggle("on", state.hardware);
    tvRender(document.getElementById("groups"), state, {});
    renderSites();
  }

  tvLoad().then(function (r) {
    state = r;
    render();
    var on = document.querySelector("button.on");
    if (on) on.focus();
  });

  document.getElementById("reset").onclick = function () {
    state.settings = Object.assign({}, TV_DEFAULTS, { disabledSites: [], desktopSites: [] });
    tvSave(state.settings).then(render);
  };

  chrome.storage.onChanged.addListener(function (c, area) {
    if (area === "local" && c.settings && state) {
      state.settings = Object.assign({}, TV_DEFAULTS, c.settings.newValue || {});
      render();
    }
  });
})();
