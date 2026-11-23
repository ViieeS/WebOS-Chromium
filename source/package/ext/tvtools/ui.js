// TV Tools: the settings UI, shared by the popup and the options page. Big buttons, visible focus, remote friendly.
"use strict";

var TV_GROUPS = [
  {
    id: "lite", title: "Lite mode", section: "browsing",
    help: "Lighter pages: no animations, images load when you scroll to them, muted videos don't start by themselves, " +
      "and JavaScript uses less memory but runs slower. Auto turns it on when the TV had less than 200 MB free when " +
      "the browser opened. The JavaScript part changes the next time the browser opens.",
    options: [["auto", "Auto"], ["off", "Off"], ["on", "On"]]
  },
  {
    id: "uaMode", title: "Website version", section: "browsing",
    help: "TV asks sites for their TV layout (YouTube opens its TV app). Desktop asks for the normal desktop pages.",
    options: [["tv", "TV"], ["desktop", "Desktop"], ["custom", "Custom"]]
  },
  {
    id: "maxRes", title: "Maximum video resolution", section: "video",
    help: "Applies to videos on every site. Lower uses less memory and data. 4K may stutter or lag, " +
      "especially when a video starts; if it does, go back to Auto or 1080p.",
    options: [["auto", "Auto"], [2160, "4K"], [1080, "1080p"], [720, "720p"], [480, "480p"], [360, "360p"]]
  },
  {
    id: "maxFps", title: "Maximum frame rate", section: "video",
    options: [["auto", "Auto"], [60, "60 fps"], [30, "30 fps"]]
  },
  {
    id: "codec", title: "Video codec", section: "video",
    help: "H.264 only is the most compatible choice if a video won't play.",
    options: [["auto", "Auto"], ["h264", "H.264 only"]]
  },
  {
    id: "discardMinutes", title: "Unload background tabs after", section: "memory",
    help: "Frees memory. An unloaded tab reloads when you open it again.",
    options: [[0, "Never"], [5, "5 min"], [15, "15 min"], [30, "30 min"], [60, "1 hour"]]
  },
  {
    id: "tabLimit", title: "Maximum open tabs", section: "memory",
    help: "Opening one more tab closes the tab you used least recently (never a pinned tab or one playing sound).",
    options: [[0, "No limit"], [3, "3"], [5, "5"], [8, "8"], [10, "10"]]
  },
  {
    id: "pauseHidden", title: "Pause videos in background tabs", section: "memory",
    options: [[false, "Off"], [true, "On"]]
  }
];

function tvAutoText(id, hw, h264hw, lowMem, freeMB) {
  h264hw = hw && h264hw === true;
  if (id === "lite") {
    var mb = typeof freeMB === "number" ? " (" + freeMB + " MB free at start)" : "";
    return (lowMem ? "Auto = On" : "Auto = Off") + mb;
  }
  if (id === "maxRes") return hw ? "Auto = 1080p" : "Auto = 720p";
  if (id === "maxFps") return hw && !h264hw ? "Auto = 60 fps" : "Auto = 30 fps";
  if (id === "codec") return hw && !h264hw ? "Auto = any codec the TV decodes" : "Auto = H.264";
  return "";
}

function tvEl(tag, cls, text) {
  var e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
}

// Renders the setting groups into `root`. `opts.compact` hides help texts (popup).
function tvRender(root, state, opts) {
  opts = opts || {};
  root.textContent = "";
  var sections = { browsing: "Browsing", video: "Video", memory: "Memory" };
  Object.keys(sections).forEach(function (sec) {
    var card = tvEl("section", "card");
    card.appendChild(tvEl("h2", "", sections[sec]));
    TV_GROUPS.filter(function (g) { return g.section === sec; }).forEach(function (g) {
      var row = tvEl("div", "group");
      var label = tvEl("div", "label", g.title);
      row.appendChild(label);
      var seg = tvEl("div", "seg");
      seg.setAttribute("role", "radiogroup");
      seg.setAttribute("aria-label", g.title);
      g.options.forEach(function (o) {
        var b = tvEl("button", "opt", o[1]);
        var selected = String(state.settings[g.id]) === String(o[0]);
        b.setAttribute("role", "radio");
        b.setAttribute("aria-checked", selected ? "true" : "false");
        if (selected) b.classList.add("on");
        b.addEventListener("click", function () {
          state.settings[g.id] = o[0];
          tvSave(state.settings).then(function () { tvRender(root, state, opts); refocus(g.id, o[0]); });
        });
        b.dataset.group = g.id;
        b.dataset.value = String(o[0]);
        seg.appendChild(b);
      });
      row.appendChild(seg);
      var note = tvAutoText(g.id, state.hardware, state.hwH264, state.lowMem, state.freeMB);
      if (note && String(state.settings[g.id]) === "auto") row.appendChild(tvEl("div", "hint", note));
      if (g.id === "maxRes" && String(state.settings.maxRes) === "2160")
        row.appendChild(tvEl("div", "hint", "4K may stutter or lag, especially when a video starts."));
      if (g.id === "uaMode" && state.settings.uaMode === "custom") row.appendChild(tvCustomUA(state, opts));
      if (!opts.compact && g.help) row.appendChild(tvEl("div", "help", g.help));
      card.appendChild(row);
    });
    root.appendChild(card);
  });

  function refocus(group, value) {
    var b = root.querySelector('button[data-group="' + group + '"][data-value="' + String(value) + '"]');
    if (b) b.focus();
  }
}

function tvStatusText(hw, h264hw) {
  if (hw && h264hw === true) return "Hardware video decoding is on (H.264 only)";
  return hw ? "Hardware video decoding is on" : "Hardware video decoding is off (software decoding)";
}

// Custom user agent: a text field and Save (OK on the field opens the TV's keyboard).
function tvCustomUA(state, opts) {
  var box = tvEl("div", "custom");
  if (opts.compact) {
    box.appendChild(tvEl("div", "hint", state.settings.customUA ? "Custom: " + state.settings.customUA
                                                                : "Set the custom user agent in All settings"));
    return box;
  }
  var input = tvEl("input");
  input.type = "text";
  input.value = state.settings.customUA || "";
  input.placeholder = "Mozilla/5.0 (...)";
  input.setAttribute("aria-label", "Custom user agent");
  var save = tvEl("button", "", "Save");
  save.onclick = function () {
    state.settings.customUA = input.value.trim();
    tvSave(state.settings);
  };
  box.appendChild(input);
  box.appendChild(save);
  if (!input.value) box.appendChild(tvEl("div", "hint", "Until you save one, sites get the normal desktop version."));
  return box;
}
